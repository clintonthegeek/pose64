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
 * Runs m68k CPU emulation in a separate thread to prevent
 * blocking the Qt event loop. Receives commands via thread-safe
 * queue and signals completion when done.
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
     * Main thread loop - runs CPU emulation.
     * Overrides QThread::run().
     */
    void run() override;

protected:
    void timerEvent(QTimerEvent* event) override;

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
    Command fCurrentCommand;

    // Helper: Dequeue next command, blocking if none available
    Command dequeueCommand();

    // Helper: Execute command handler, emit response in main thread
    void executeCommand(const Command& cmd);
};

#endif /* CPUWorkerThread_h */
