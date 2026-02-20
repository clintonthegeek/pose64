#include "EmCommon.h"

// Undefine Palm OS macros that conflict with Qt
#undef daysInYear
#undef monthsInYear

#include "CPUWorkerThread.h"
#include <QCoreApplication>
#include <QMutexLocker>

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
    Command stopCmd;
    stopCmd.type = CMD_SHUTDOWN;
    stopCmd.handler = nullptr;
    stopCmd.response = nullptr;

    queueCommand(stopCmd);
    wait();  // Wait for thread to exit
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

        // Queue response lambda to run on main thread event loop
        if (cmd.response) {
            auto response = cmd.response;
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                [response]() { response(); },
                Qt::QueuedConnection
            );
        }
    } catch (const std::exception& e) {
        emit errorOccurred(QString::fromStdString(e.what()));
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
        emit errorOccurred(QString::fromStdString(e.what()));
    }
}
