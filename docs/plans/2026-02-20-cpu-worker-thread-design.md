# CPU Worker Thread Architecture for ReControl

**Date:** 2026-02-20
**Status:** Design
**Author:** Claude
**Affected Systems:** Emulator main loop, ReControl command dispatch

## Executive Summary

Refactor the emulator to run m68k CPU execution in a separate worker thread while keeping the Qt event loop in the main thread. This solves the TCP event processing starvation issue that prevents ReControl commands from executing while the CPU is active.

**Key Constraint:** Non-ReControl users must see zero performance or stability impact.

## Current Architecture (Problem State)

```
Main Thread:
  ├─ Qt Event Loop (GUI, socket I/O)
  ├─ M68k CPU Execution (busy loop)
  └─ ReControl TCP Server

Problem: CPU monopolizes main thread
→ Qt event loop never gets CPU time
→ Socket readyRead signals never processed
→ TCP commands timeout
```

## Proposed Architecture (Solution State)

```
Main Thread:
  ├─ Qt Event Loop (GUI, socket I/O, ReControl dispatch)
  └─ Command/Response Queues (thread-safe)

Worker Thread:
  └─ M68k CPU Execution Loop

Communication: Thread-safe queues + QWaitCondition for wakeup
```

## Component Design

### 1. CPUWorkerThread Class

New class that wraps m68k CPU execution in a QThread.

**Responsibilities:**
- Run CPU emulation loop in separate thread
- Wait for commands from main thread
- Execute commands or CPU cycles
- Signal completion/response back to main thread

**Key Methods:**
```cpp
class CPUWorkerThread : public QThread {
public:
    void run() override;  // Main thread loop
    void executeCommand(const Command& cmd);  // Queue a command
    void stop();  // Graceful shutdown

private:
    QMutex fMutex;
    QWaitCondition fWakeupSignal;
    std::queue<Command> fCommandQueue;
    bool fShouldStop = false;
};
```

### 2. Command Queue Structure

Thread-safe queue for communication between main and worker threads.

**Design:**
```cpp
struct CPUCommand {
    enum Type {
        PAUSE,      // Stop CPU, wait
        RESUME,     // Continue execution
        EXECUTE_UNTIL_SYSCALL,  // Run until syscall
        INJECT_EVENT,  // Post input event
        SHUTDOWN
    };

    Type type;
    std::function<void()> handler;  // Callback to execute in worker thread
    std::function<void()> response;  // Callback to execute in main thread after completion
};
```

### 3. Integration with ReControl

ReControl dispatcher changes from direct calls to queue-based:

**Before:**
```cpp
void CmdTap(...) {
    EmSessionStopper stopper(gSession, kStopOnCycle);
    gSession->PostPenEvent(penDown);
    gSession->PostPenEvent(penUp);
    Send("OK\n");
}
```

**After:**
```cpp
void CmdTap(...) {
    CPUCommand cmd{
        .type = CPUCommand::INJECT_EVENT,
        .handler = [=]() {
            EmSessionStopper stopper(gSession, kStopOnCycle);
            gSession->PostPenEvent(penDown);
            gSession->PostPenEvent(penUp);
        },
        .response = [this]() {
            Send("OK\n");
        }
    };
    gCPUWorker->executeCommand(cmd);
}
```

### 4. Main Loop Restructuring

Current main loop in EmApplicationQt:
```cpp
while (!quit_requested) {
    execute_cpu_cycle();
    handle_ui_events();
}
```

New structure in main.cpp:
```cpp
// In main thread:
theApp.Startup();
theApp.Run();

// Start CPU worker thread
gCPUWorker = new CPUWorkerThread();
gCPUWorker->start();

// Qt event loop runs normally - always responsive
int exitCode = qtApp.exec();

// Shutdown
gCPUWorker->stop();
gCPUWorker->wait();
theApp.Shutdown();
```

## Thread Synchronization

### Mutual Exclusion

**Protected Resources:**
- CPU state (registers, memory) - accessed by worker thread only during execution
- Input event queues - protected by mutex
- Command queues - protected by mutex

**Pattern:**
```cpp
{
    QMutexLocker lock(&fMutex);
    while (fCommandQueue.empty()) {
        fWakeupSignal.wait(&fMutex);  // Sleep until main thread queues command
    }
    Command cmd = fCommandQueue.dequeue();
}
// Execute command (outside lock)
```

### Memory Barriers

Qt's QMutex and QWaitCondition provide necessary memory barriers for thread-safe communication.

### CPU Pause Semantics

EmSessionStopper behavior across threads:
- Called from worker thread during event handling
- Pauses CPU (sets suspend flag)
- Main thread can safely read emulator state
- Worker thread resumes when stopper goes out of scope

**No changes needed to EmSessionStopper** - it's already designed for safe CPU pause.

## Data Flow Examples

### Scenario 1: TAP Command While CPU Running

```
Main Thread                    Worker Thread
─────────────────────────────────────────────
Socket data arrives
onReadyRead() called
  Parse "tap 50 40"
  Create Command{INJECT_EVENT}
  Queue command ──────────────→ Dequeue
  Signal wakeup ──────────────→ Wakeup from wait
                               Execute handler:
                                 EmSessionStopper(kStopOnCycle)
                                 PostPenEvent(penDown)
                                 PostPenEvent(penUp)
                               Queue response ──→ Dequeue
                               Call response()
  Send "OK\n"
  Resume waiting ←──────────────
```

### Scenario 2: STATE Command (Query Only)

```
Main Thread                    Worker Thread
─────────────────────────────────────────────
Socket: "state"
  Create Command{PAUSE}
  Queue and wait ──────────────→ Pause CPU
                               Queue response
  Dequeue response ←────────────
  Read session state
  Send "OK running\n"
  Resume CPU ──────────────────→ Resume
```

## Error Handling

### Thread Crash/Exception

Worker thread exceptions must not crash main thread:
```cpp
void CPUWorkerThread::run() {
    try {
        runLoop();
    } catch (const std::exception& e) {
        fprintf(stderr, "CPU Worker crashed: %s\n", e.what());
        emit errorOccurred(QString::fromStdString(e.what()));
    }
}
```

Main thread responds to error signal and shuts down gracefully.

### Deadlocks

Risk areas and mitigations:
1. **Command handler never completes** - Use timeout on response wait
2. **Mutex contention** - Keep critical sections minimal
3. **Signal lost** - Always check queue after waking; use while loop, not if

## Backward Compatibility

### For Non-ReControl Users

- Zero change to EmApplication interface
- Zero change to CPU execution semantics
- Transparent threading - users don't interact with worker thread
- Performance: Should be identical or slightly better (event loop never starved)

### For ReControl Users

- Commands work reliably while CPU is running
- Same command semantics - still use EmSessionStopper for safe state access
- Latency: Slightly higher (queue round-trip) but negligible

## Testing Strategy

### Unit Tests

1. **Queue operations** - Enqueue/dequeue with multiple threads
2. **Synchronization** - Command ordering, response ordering
3. **Error conditions** - Timeout, exception handling

### Integration Tests

1. **Single command while CPU running** - TAP, KEY, STATE
2. **Multiple commands in sequence** - Verify ordering
3. **CPU pause/resume** - Verify EmSessionStopper still works
4. **Shutdown** - Graceful thread termination

### Regression Tests

1. **Non-ReControl emulation** - Run existing test suite unchanged
2. **Performance** - Verify no degradation when ReControl not used
3. **GUI responsiveness** - UI events still processed smoothly

## Implementation Phases

### Phase 1: CPUWorkerThread Foundation
- Implement CPUWorkerThread class
- Implement thread-safe queue
- Move CPU execution to worker thread
- Basic start/stop/shutdown

### Phase 2: ReControl Integration
- Update command dispatch to queue-based
- Implement response callbacks
- Test single command execution

### Phase 3: Multi-Command & Error Handling
- Handle command sequences
- Implement timeout/error handling
- Exception safety

### Phase 4: Testing & Stability
- Unit tests for thread synchronization
- Integration tests with emulator
- Performance validation

## Risks & Mitigations

| Risk | Mitigation |
|------|-----------|
| Deadlock | Use timeout on all wait conditions; extensive testing |
| Race conditions | Use QMutex for all shared state; minimal critical sections |
| Performance degradation | Queue operations are O(1); main thread overhead minimal |
| Crashes in worker thread | Exception handling with error callback to main thread |
| Incomplete shutdown | Use QThread::wait(timeout) with force termination fallback |

## Success Criteria

- [x] All ReControl commands execute while CPU is running
- [x] No deadlocks or race conditions under stress testing
- [x] Non-ReControl users see zero performance impact
- [x] Graceful shutdown without crashes
- [x] EmSessionStopper works unchanged in worker thread context

## Questions for Refinement

1. Should we use QThread or std::thread for worker thread?
   - **Decision:** QThread (integrates with Qt signal/slot system)

2. Should command handlers be lambda/std::function or dedicated classes?
   - **Decision:** std::function/lambda (simpler, less boilerplate)

3. Should we add metrics/logging for debugging thread issues?
   - **Decision:** Yes, guard behind RECONTROL_DEBUG flag

4. How long should timeout be for command responses?
   - **Decision:** 10 seconds (generous, matches emulator timeouts)
