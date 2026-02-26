#ifndef CPUWorkerThread_h
#define CPUWorkerThread_h

#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <queue>
#include <functional>

// Forward declaration
class EmSession;

/**
 * CPUWorkerThread
 *
 * Dedicated thread for executing ReControl command handlers that
 * would otherwise block the Qt main thread.  Operations like
 * EmSessionStopper, PostPenEvent, and PostKeyEvent internally call
 * PrvWakeUpCPU which blocks until the CPU reaches a syscall
 * boundary.  Running these handlers here keeps the Qt event loop
 * responsive.
 *
 * NOTE: This thread does NOT run the m68k CPU loop.  The CPU
 * continues to run on the omni_thread created by
 * EmSession::CreateThread (EmSession::Run -> CallCPU ->
 * EmCPU68K::Execute).  This thread only dispatches handler
 * lambdas queued by ReControl commands.
 */
class CPUWorkerThread : public QThread
{
    Q_OBJECT

public:
    enum CommandType {
        CMD_PAUSE,           // Pause CPU, wait for next command
        CMD_RESUME,          // Resume CPU execution
        CMD_EXECUTE_CYCLE,   // Execute one CPU cycle
        CMD_INJECT_EVENT,    // Post input event to session
        CMD_SHUTDOWN         // Stop thread and exit
    };

    struct Command {
        CommandType type;
        std::function<void()> handler;      // Execute in worker thread
        std::function<void()> response;     // Execute in main thread after handler
    };

    CPUWorkerThread(QObject* parent = nullptr);
    ~CPUWorkerThread();

    /**
     * Queue a command for execution by the CPU worker thread.
     * Blocks until command is queued (non-blocking internally).
     */
    void queueCommand(const Command& cmd);

    /**
     * Request graceful shutdown. Blocks until thread exits.
     */
    void shutdown();

    /**
     * Thread entry point — command processing loop.
     * Blocks on the queue, executes handler lambdas, delivers
     * response callbacks to the main thread.  Overrides QThread::run().
     */
    void run() override;

signals:
    // Emitted when a command's response callback should run in main thread
    void commandCompleted();

    // Emitted if an error occurs in worker thread
    void errorOccurred(const QString& message);

private:
    QMutex fMutex;
    QWaitCondition fWakeupSignal;
    std::queue<Command> fCommandQueue;
    bool fShouldStop;

    // Helper: Dequeue next command, blocking if none available
    Command dequeueCommand();

    // Helper: Execute command handler, run response in main thread
    void executeCommand(const Command& cmd);
};

#endif /* CPUWorkerThread_h */
