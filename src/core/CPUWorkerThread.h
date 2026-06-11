#ifndef CPUWorkerThread_h
#define CPUWorkerThread_h

#include <QMutex>
#include <QWaitCondition>
#include <pthread.h>
#include <queue>
#include <functional>

// Forward declaration
class EmSession;

/**
 * CPUWorkerThread
 *
 * Dedicated thread for executing ReControl command handlers that
 * would otherwise block the Qt main thread.  Operations like
 * EmSessionStopper block until the CPU reaches a cycle/syscall
 * boundary; running these handlers here keeps the Qt event loop
 * responsive.
 *
 * NOTE: This thread does NOT run the m68k CPU loop.  The CPU
 * continues to run on the omni_thread created by
 * EmSession::CreateThread (EmSession::Run -> CallCPU ->
 * EmCPU68K::Execute).  This thread only dispatches handler
 * lambdas queued by ReControl commands.
 *
 * Uses raw pthreads (not QThread) so that shutdown() can call
 * pthread_join, giving TSAN a proper happens-before point and
 * preventing its thread-registry CHECK failure on reload (which
 * Qt's detach-based lifecycle triggered by reusing pthread_t).
 */
class CPUWorkerThread
{
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

    CPUWorkerThread();
    ~CPUWorkerThread();

    /**
     * Start the worker thread.
     */
    void start();

    /**
     * Queue a command for execution by the CPU worker thread.
     * Blocks until command is queued (non-blocking internally).
     */
    void queueCommand(const Command& cmd);

    /**
     * Request graceful shutdown. Blocks until thread exits (pthread_join).
     */
    void shutdown();

private:
    QMutex fMutex;
    QWaitCondition fWakeupSignal;
    std::queue<Command> fCommandQueue;
    bool fShouldStop;

    pthread_t fPthread;
    bool fStarted;

    static void* threadFunc(void* arg);
    void run();

    // Helper: Dequeue next command, blocking if none available
    Command dequeueCommand();

    // Helper: Execute command handler, run response in main thread
    void executeCommand(const Command& cmd);
};

#endif /* CPUWorkerThread_h */
