#include "CPUWorkerThread.h"
#include "EmSession.h"
#include <QCoreApplication>
#include <QMutexLocker>
#include <cstdio>

CPUWorkerThread::CPUWorkerThread(QObject* parent)
    : QThread(parent), fShouldStop(false)
{
}

CPUWorkerThread::~CPUWorkerThread()
{
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
    fprintf(stderr, "[CPUWorkerThread] Shutdown requested\n");
    fflush(stderr);

    Command stopCmd;
    stopCmd.type = CMD_SHUTDOWN;
    stopCmd.handler = nullptr;
    stopCmd.response = nullptr;

    queueCommand(stopCmd);
    wait();  // Wait for thread to exit

    fprintf(stderr, "[CPUWorkerThread] Shutdown complete\n");
    fflush(stderr);
}

Command CPUWorkerThread::dequeueCommand()
{
    QMutexLocker locker(&fMutex);

    // Wait for command if queue is empty
    while (fCommandQueue.empty() && !fShouldStop) {
        fprintf(stderr, "[CPUWorkerThread] Waiting for command...\n");
        fflush(stderr);
        fWakeupSignal.wait(&fMutex);
    }

    if (fCommandQueue.empty()) {
        fprintf(stderr, "[CPUWorkerThread] Stop signal received, exiting\n");
        fflush(stderr);
        Command empty;
        empty.type = CMD_SHUTDOWN;
        return empty;
    }

    Command cmd = fCommandQueue.front();
    fCommandQueue.pop();
    fprintf(stderr, "[CPUWorkerThread] Dequeued command type=%d\n", (int)cmd.type);
    fflush(stderr);
    return cmd;
}

void CPUWorkerThread::executeCommand(const Command& cmd)
{
    try {
        fprintf(stderr, "[CPUWorkerThread] Executing command handler\n");
        fflush(stderr);

        if (cmd.handler) {
            cmd.handler();
        }

        fprintf(stderr, "[CPUWorkerThread] Command handler complete\n");
        fflush(stderr);

        // Emit signal to run response in main thread
        if (cmd.response) {
            fprintf(stderr, "[CPUWorkerThread] Queuing response callback for main thread\n");
            fflush(stderr);
            QCoreApplication::postEvent(this, new QEvent(QEvent::User), Qt::NormalEventPriority);
            fCurrentCommand = cmd;
        }
    } catch (const std::exception& e) {
        fprintf(stderr, "[CPUWorkerThread] Exception: %s\n", e.what());
        fflush(stderr);
        emit errorOccurred(QString::fromStdString(e.what()));
    }
}

void CPUWorkerThread::timerEvent(QTimerEvent* event)
{
    // Execute response callback in main thread context
    if (fCurrentCommand.response) {
        fprintf(stderr, "[CPUWorkerThread] Executing response callback in main thread\n");
        fflush(stderr);
        fCurrentCommand.response();
        fCurrentCommand.handler = nullptr;
        fCurrentCommand.response = nullptr;
    }
    QThread::timerEvent(event);
}

void CPUWorkerThread::run()
{
    fprintf(stderr, "[CPUWorkerThread] Worker thread started\n");
    fflush(stderr);

    try {
        while (true) {
            Command cmd = dequeueCommand();

            if (cmd.type == CMD_SHUTDOWN) {
                fprintf(stderr, "[CPUWorkerThread] Shutdown command received, exiting loop\n");
                fflush(stderr);
                break;
            }

            executeCommand(cmd);
        }
    } catch (const std::exception& e) {
        fprintf(stderr, "[CPUWorkerThread] Fatal error: %s\n", e.what());
        fflush(stderr);
    }

    fprintf(stderr, "[CPUWorkerThread] Worker thread exiting\n");
    fflush(stderr);
}
