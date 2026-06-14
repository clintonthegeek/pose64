#include "EmCommon.h"

#include "PalmMacroUndefs.h"	// Phase 5: daysInYear/monthsInYear undef

#include "CPUWorkerThread.h"
#include <QCoreApplication>
#include <QMutexLocker>
#include <stdio.h>

CPUWorkerThread::CPUWorkerThread()
    : fShouldStop(false), fStarted(false)
{
}

CPUWorkerThread::~CPUWorkerThread()
{
    if (fStarted)
        pthread_detach(fPthread);   // prevent resource leak if not joined
}

void CPUWorkerThread::start()
{
    fStarted = true;
    pthread_create(&fPthread, nullptr, &CPUWorkerThread::threadFunc, this);
}

void* CPUWorkerThread::threadFunc(void* arg)
{
    static_cast<CPUWorkerThread*>(arg)->run();
    return nullptr;
}

void CPUWorkerThread::queueCommand(const Command& cmd)
{
    {
        QMutexLocker locker(&fMutex);
        fCommandQueue.push(cmd);
    }
    fWakeupSignal.wakeOne();
}

void CPUWorkerThread::shutdown()
{
    {
        QMutexLocker locker(&fMutex);
        fShouldStop = true;
    }

    Command stopCmd;
    stopCmd.type = CMD_SHUTDOWN;
    stopCmd.handler = nullptr;
    stopCmd.response = nullptr;
    queueCommand(stopCmd);

    // Bounded wait: a stuck in-flight handler must not block the main thread
    // forever.  After 1.2, any EmSessionStopper inside a handler self-releases
    // within its own deadline (5000ms), so 8s comfortably exceeds all
    // legitimate cases.
    if (fStarted) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += 8;

        int rc = pthread_timedjoin_np(fPthread, nullptr, &deadline);
        if (rc != 0) {
            fprintf(stderr, "[CPUWorker] handler did not exit in 8s (rc=%d); detaching\n", rc);
            pthread_detach(fPthread);
        }
        fStarted = false;
    }
}

CPUWorkerThread::Command CPUWorkerThread::dequeueCommand()
{
    QMutexLocker locker(&fMutex);

    // Wait for command if queue is empty
    while (fCommandQueue.empty() && !fShouldStop) {
        fWakeupSignal.wait(&fMutex);
    }

    if (fCommandQueue.empty()) {
        Command empty;
        empty.type = CMD_SHUTDOWN;
        return empty;
    }

    Command cmd = fCommandQueue.front();
    fCommandQueue.pop();
    return cmd;
}

void CPUWorkerThread::executeCommand(const Command& cmd)
{
    try {
        if (cmd.handler) {
            cmd.handler();
        }
    } catch (const std::exception& e) {
        fprintf(stderr, "[CPUWorker] exception in handler: %s\n", e.what());
    } catch (...) {
        fprintf(stderr, "[CPUWorker] unknown exception in handler\n");
    }

    // ALWAYS fire the response callback, even if the handler threw.
    // RAII objects (EmSessionStopper) have already cleaned up
    // via stack unwinding in the catch blocks above.
    try {
        if (cmd.response) {
            auto response = cmd.response;
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                [response]() { response(); },
                Qt::QueuedConnection
            );
        }
    } catch (...) {
        // Last resort — don't let the response scheduling kill us
    }
}

void CPUWorkerThread::run()
{
    try {
        while (true) {
            Command cmd = dequeueCommand();

            if (cmd.type == CMD_SHUTDOWN) {
                break;
            }

            executeCommand(cmd);
        }
    } catch (const std::exception& e) {
        fprintf(stderr, "[CPUWorker] thread terminated by exception: %s\n", e.what());
    } catch (...) {
        fprintf(stderr, "[CPUWorker] thread terminated by unknown exception\n");
    }
}
