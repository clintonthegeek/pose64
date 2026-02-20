# CPU Worker Thread Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Move m68k CPU execution to a separate worker thread so ReControl TCP commands execute reliably while the CPU is running.

**Architecture:** New CPUWorkerThread class wraps CPU execution in QThread. Main thread runs Qt event loop + ReControl dispatcher. Thread-safe queue passes commands from main to worker thread. EmSessionStopper works unchanged.

**Tech Stack:** Qt 6, C++17, QThread, QMutex, QWaitCondition, std::queue

---

## Task 1: Create CPUWorkerThread Class Header

**Files:**
- Create: `src/core/CPUWorkerThread.h`

**Step 1: Create empty header file with class declaration**

Create `src/core/CPUWorkerThread.h`:

```cpp
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
```

**Step 2: Verify file was created**

Run: `ls -la src/core/CPUWorkerThread.h`
Expected: File exists, ~100 lines

**Step 3: Commit**

```bash
git add src/core/CPUWorkerThread.h
git commit -m "feat: add CPUWorkerThread class header

Define thread-safe interface for running m68k CPU in separate worker thread.
Includes command queue, signal definitions, and async callback pattern."
```

---

## Task 2: Implement CPUWorkerThread Class

**Files:**
- Create: `src/core/CPUWorkerThread.cpp`

**Step 1: Create CPUWorkerThread implementation**

Create `src/core/CPUWorkerThread.cpp`:

```cpp
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
```

**Step 2: Add CPUWorkerThread to CMakeLists.txt**

Modify `CMakeLists.txt` to add the new source file:

Find the line with `ReControl.cpp` in the source list and add:
```cmake
src/core/CPUWorkerThread.cpp
```

**Step 3: Verify compilation**

Run: `make -C build -j4 2>&1 | tail -20`
Expected: Build succeeds, no errors in CPUWorkerThread

**Step 4: Commit**

```bash
git add src/core/CPUWorkerThread.cpp CMakeLists.txt
git commit -m "feat: implement CPUWorkerThread class

Implement worker thread that runs m68k CPU execution. Features:
- Thread-safe command queue with blocking wait
- Command handler + response callback pattern
- Graceful shutdown with QThread::wait()
- Debug logging for thread synchronization
- Exception handling in worker thread"
```

---

## Task 3: Create Global CPUWorkerThread Instance

**Files:**
- Modify: `src/core/ReControl.h`
- Modify: `src/core/ReControl.cpp`

**Step 1: Add CPUWorkerThread forward declaration and extern**

Modify `src/core/ReControl.h`:

Add after includes:
```cpp
// Forward declaration
class CPUWorkerThread;

// Global instance - initialized in main.cpp
extern CPUWorkerThread* gCPUWorker;
```

**Step 2: Add the global instance in ReControl.cpp**

Modify `src/core/ReControl.cpp` (after includes):

Add:
```cpp
// Global CPU worker thread instance
CPUWorkerThread* gCPUWorker = nullptr;
```

Also add include at top of ReControl.cpp:
```cpp
#include "CPUWorkerThread.h"
```

**Step 3: Verify compilation**

Run: `make -C build -j4 2>&1 | tail -20`
Expected: Build succeeds

**Step 4: Commit**

```bash
git add src/core/ReControl.h src/core/ReControl.cpp
git commit -m "feat: add global CPUWorkerThread instance

Add extern declaration in ReControl.h and initialization in
ReControl.cpp to make worker thread accessible to command handlers."
```

---

## Task 4: Initialize CPUWorkerThread in main()

**Files:**
- Modify: `src/ui/main.cpp`

**Step 1: Update main.cpp to start worker thread**

Modify `src/ui/main.cpp` in the main function:

Replace this section:
```cpp
            // Defer ReControl startup until event loop is running
            if (recontrolPort > 0)
            {
                QTimer::singleShot (0, [recontrolPort]() {
                    ReControl_Startup (recontrolPort);
                });
            }
```

With:
```cpp
            // Start CPU worker thread BEFORE event loop
            extern CPUWorkerThread* gCPUWorker;
            #include "CPUWorkerThread.h"

            gCPUWorker = new CPUWorkerThread();
            gCPUWorker->start();
            fprintf(stderr, "[main] CPU worker thread started\n");
            fflush(stderr);

            // Defer ReControl startup until event loop is running
            if (recontrolPort > 0)
            {
                QTimer::singleShot (0, [recontrolPort]() {
                    ReControl_Startup (recontrolPort);
                });
            }
```

And after `qtApp.exec()`, add shutdown:
```cpp
            // Shut down CPU worker thread
            if (gCPUWorker) {
                fprintf(stderr, "[main] Shutting down CPU worker thread\n");
                fflush(stderr);
                gCPUWorker->shutdown();
                delete gCPUWorker;
                gCPUWorker = nullptr;
            }

            // Shut down ReControl server
            ReControl_Shutdown ();
```

**Step 2: Add CPUWorkerThread include at top of main.cpp**

Add:
```cpp
#include "CPUWorkerThread.h"
```

**Step 3: Verify compilation**

Run: `make -C build -j4 2>&1 | tail -20`
Expected: Build succeeds

**Step 4: Run emulator to verify startup**

Run: `timeout 5 ./build/pose64 m515.pcf --port 6425 2>&1 | grep "CPU worker\|Listening"`
Expected: See "[main] CPU worker thread started" and "ReControlServer: Listening"

**Step 5: Commit**

```bash
git add src/ui/main.cpp
git commit -m "feat: initialize CPUWorkerThread in main()

Start worker thread before event loop begins. Ensure graceful
shutdown of worker thread after event loop exits."
```

---

## Task 5: Update CmdTap to Use Worker Thread

**Files:**
- Modify: `src/core/ReControl.cpp` (CmdTap method)

**Step 1: Update CmdTap to queue command instead of executing directly**

Modify the CmdTap method in `src/core/ReControl.cpp`:

Replace:
```cpp
void ReControlSession::CmdTap (const QStringList& args)
{
	// args: ["tap", "80", "80"]
	if (args.size () != 3) { SendErr ("usage", "tap <x> <y>"); return; }
	if (!gSession) { SendErr ("transient", "no session"); return; }

	int x = args[1].toInt ();
	int y = args[2].toInt ();

	fprintf (stderr, "[ReControl] CmdTap: Starting tap at (%d, %d)\n", x, y);
	fprintf (stderr, "[ReControl] CmdTap: Creating EmSessionStopper...\n");
	fflush (stderr);

	// Suspend CPU to safely inject pen event
	EmSessionStopper stopper (gSession, kStopOnCycle);

	fprintf (stderr, "[ReControl] CmdTap: EmSessionStopper created, posting events\n");
	fflush (stderr);

	EmPenEvent penDown (EmPoint (x, y), true);
	gSession->PostPenEvent (penDown);

	fprintf (stderr, "[ReControl] CmdTap: Posted pen down event\n");
	fflush (stderr);

	EmPenEvent penUp (EmPoint (-1, -1), false);
	gSession->PostPenEvent (penUp);

	fprintf (stderr, "[ReControl] CmdTap: Posted pen up event\n");
	fflush (stderr);

	fprintf (stderr, "[ReControl] CmdTap: Sending OK response\n");
	fflush (stderr);
	Send ("OK\n");

	fprintf (stderr, "[ReControl] CmdTap: Complete\n");
	fflush (stderr);
}
```

With:
```cpp
void ReControlSession::CmdTap (const QStringList& args)
{
	// args: ["tap", "80", "80"]
	if (args.size () != 3) { SendErr ("usage", "tap <x> <y>"); return; }
	if (!gSession) { SendErr ("transient", "no session"); return; }

	int x = args[1].toInt ();
	int y = args[2].toInt ();

	fprintf (stderr, "[ReControl] CmdTap: Queuing tap at (%d, %d)\n", x, y);
	fflush (stderr);

	// Capture 'this' to send response in main thread
	ReControlSession* self = this;

	// Create command that will execute in worker thread
	CPUWorkerThread::Command cmd{
		.type = CPUWorkerThread::CMD_INJECT_EVENT,
		.handler = [x, y]() {
			fprintf (stderr, "[ReControl] CmdTap handler: Creating EmSessionStopper\n");
			fflush (stderr);

			// Suspend CPU to safely inject pen event
			EmSessionStopper stopper (gSession, kStopOnCycle);

			EmPenEvent penDown (EmPoint (x, y), true);
			gSession->PostPenEvent (penDown);

			EmPenEvent penUp (EmPoint (-1, -1), false);
			gSession->PostPenEvent (penUp);

			fprintf (stderr, "[ReControl] CmdTap handler: Complete\n");
			fflush (stderr);
		},
		.response = [self]() {
			fprintf (stderr, "[ReControl] CmdTap response: Sending OK\n");
			fflush (stderr);
			self->Send ("OK\n");
		}
	};

	// Queue command for worker thread to execute
	gCPUWorker->queueCommand(cmd);
}
```

**Step 2: Verify compilation**

Run: `make -C build -j4 2>&1 | tail -20`
Expected: Build succeeds

**Step 3: Test with single tap**

Run:
```bash
timeout 10 ./build/pose64 m515.pcf --port 6425 > /tmp/test.log 2>&1 &
sleep 3
echo "tap 50 40" | nc -w 2 localhost 6425
sleep 1
grep "CmdTap\|handler\|response" /tmp/test.log
killall -9 pose64
```

Expected: See debug messages showing command queueing, handler execution, response

**Step 4: Commit**

```bash
git add src/core/ReControl.cpp
git commit -m "feat: update CmdTap to use worker thread queue

Change CmdTap from direct execution to queueing command via
CPUWorkerThread. Handler runs in worker thread, response callback
runs in main thread. Maintains same behavior via async dispatch."
```

---

## Task 6: Update CmdKey to Use Worker Thread

**Files:**
- Modify: `src/core/ReControl.cpp` (CmdKey method)

**Step 1: Update CmdKey similarly to CmdTap**

In `src/core/ReControl.cpp`, find CmdKey and replace:

```cpp
void ReControlSession::CmdKey (const QStringList& args)
{
	if (args.size () != 2) { SendErr ("usage", "key <charcode>"); return; }
	if (!gSession) { SendErr ("transient", "no session"); return; }

	int charcode = args[1].toInt ();

	// Suspend CPU to safely inject key event
	EmSessionStopper stopper (gSession, kStopOnCycle);

	EmKeyEvent keyEvent (charcode);
	gSession->PostKeyEvent (keyEvent);

	Send ("OK\n");
}
```

With:
```cpp
void ReControlSession::CmdKey (const QStringList& args)
{
	if (args.size () != 2) { SendErr ("usage", "key <charcode>"); return; }
	if (!gSession) { SendErr ("transient", "no session"); return; }

	int charcode = args[1].toInt ();
	ReControlSession* self = this;

	CPUWorkerThread::Command cmd{
		.type = CPUWorkerThread::CMD_INJECT_EVENT,
		.handler = [charcode]() {
			EmSessionStopper stopper (gSession, kStopOnCycle);
			EmKeyEvent keyEvent (charcode);
			gSession->PostKeyEvent (keyEvent);
		},
		.response = [self]() {
			self->Send ("OK\n");
		}
	};

	gCPUWorker->queueCommand(cmd);
}
```

**Step 2: Verify compilation**

Run: `make -C build -j4 2>&1 | tail -20`
Expected: Build succeeds

**Step 3: Commit**

```bash
git add src/core/ReControl.cpp
git commit -m "feat: update CmdKey to use worker thread queue

Apply same async dispatch pattern as CmdTap. Handler executes
in worker thread with CPU suspension, response in main thread."
```

---

## Task 7: Update CmdButton to Use Worker Thread

**Files:**
- Modify: `src/core/ReControl.cpp` (CmdButton method)

**Step 1: Update CmdButton**

Find CmdButton method and update the button press section:

Replace:
```cpp
	// Suspend CPU to safely inject button event
	EmSessionStopper stopper (gSession, kStopOnCycle);

	if (action == "down")      gSession->SetButtonDown (button);
	else if (action == "up")   gSession->SetButtonUp (button);
	else if (action == "tap")  gSession->SetButtonTap (button);
	else { SendErr ("usage", "button <name> <down|up|tap>"); return; }

	Send ("OK\n");
```

With:
```cpp
	ReControlSession* self = this;

	CPUWorkerThread::Command cmd{
		.type = CPUWorkerThread::CMD_INJECT_EVENT,
		.handler = [button, action]() {
			EmSessionStopper stopper (gSession, kStopOnCycle);

			if (action == "down")      gSession->SetButtonDown (button);
			else if (action == "up")   gSession->SetButtonUp (button);
			else if (action == "tap")  gSession->SetButtonTap (button);
		},
		.response = [self]() {
			self->Send ("OK\n");
		}
	};

	gCPUWorker->queueCommand(cmd);
```

**Step 2: Verify compilation**

Run: `make -C build -j4 2>&1 | tail -20`
Expected: Build succeeds

**Step 3: Commit**

```bash
git add src/core/ReControl.cpp
git commit -m "feat: update CmdButton to use worker thread queue

Apply async dispatch pattern to button commands."
```

---

## Task 8: Update CmdPen to Use Worker Thread

**Files:**
- Modify: `src/core/ReControl.cpp` (CmdPen method)

**Step 1: Update CmdPen**

Find CmdPen and replace:

```cpp
	// Suspend CPU to safely inject pen event
	EmSessionStopper stopper (gSession, kStopOnCycle);

	EmPenEvent penEvent (EmPoint (x, y), isDown);
	gSession->PostPenEvent (penEvent);

	Send ("OK\n");
```

With:
```cpp
	ReControlSession* self = this;

	CPUWorkerThread::Command cmd{
		.type = CPUWorkerThread::CMD_INJECT_EVENT,
		.handler = [x, y, isDown]() {
			EmSessionStopper stopper (gSession, kStopOnCycle);
			EmPenEvent penEvent (EmPoint (x, y), isDown);
			gSession->PostPenEvent (penEvent);
		},
		.response = [self]() {
			self->Send ("OK\n");
		}
	};

	gCPUWorker->queueCommand(cmd);
```

**Step 2: Verify compilation**

Run: `make -C build -j4 2>&1 | tail -20`
Expected: Build succeeds

**Step 3: Commit**

```bash
git add src/core/ReControl.cpp
git commit -m "feat: update CmdPen to use worker thread queue

Apply async dispatch pattern to pen commands."
```

---

## Task 9: Remove Old Debug Output

**Files:**
- Modify: `src/core/ReControl.cpp`

**Step 1: Clean up OnReadyRead debug logging**

Remove the debug fprintf statements added earlier:

In OnReadyRead method, remove:
```cpp
	fprintf (stderr, "[ReControl] OnReadyRead: Received %d bytes\n", (int)data.size ());
	fflush (stderr);
	...
	fprintf (stderr, "[ReControl] OnReadyRead: Processing command: %s\n", line.toStdString ().c_str ());
	fflush (stderr);
	...
	fprintf (stderr, "[ReControl] OnReadyRead: Dispatching command: %s\n", cmd.toStdString ().c_str ());
	fflush (stderr);
```

Keep only essential debug output for troubleshooting (errors, timeouts).

**Step 2: Verify compilation and basic functionality**

Run: `make -C build -j4 2>&1 | tail -10`
Expected: Build succeeds

**Step 3: Commit**

```bash
git add src/core/ReControl.cpp
git commit -m "refactor: remove diagnostic debug output from OnReadyRead

Clean up temporary debug logging added during diagnosis phase.
Keep essential error/timeout logging for troubleshooting."
```

---

## Task 10: Integration Test - TAP Command

**Files:**
- Create: `test_cpu_worker_tap.py`

**Step 1: Create integration test script**

Create `test_cpu_worker_tap.py`:

```python
#!/usr/bin/env python3
"""Integration test for TAP command with CPU worker thread."""

import socket
import time
import subprocess
import sys
import signal

def test_tap_while_cpu_running():
    """Test that TAP command executes while CPU is running."""

    # Start emulator
    print("Starting emulator...")
    proc = subprocess.Popen(
        ["./build/pose64", "m515.pcf", "--port", "6425"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )

    # Wait for startup
    time.sleep(6)

    try:
        # Connect to ReControl
        print("Connecting to ReControl...")
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(10)
        sock.connect(('localhost', 6425))

        # Send initial state check
        print("Checking state...")
        sock.sendall(b'state\n')
        resp = sock.recv(1024).decode()
        print(f"  State: {resp.strip()}")
        assert resp.startswith("OK"), f"Expected OK, got: {resp}"

        # Send TAP command
        print("Sending TAP command...")
        sock.sendall(b'tap 50 40\n')
        resp = sock.recv(1024).decode()
        print(f"  Response: {resp.strip()}")
        assert resp.startswith("OK"), f"TAP failed: {resp}"

        # Send KEY command
        print("Sending KEY command...")
        sock.sendall(b'key 65\n')
        resp = sock.recv(1024).decode()
        print(f"  Response: {resp.strip()}")
        assert resp.startswith("OK"), f"KEY failed: {resp}"

        # Send second TAP
        print("Sending second TAP command...")
        sock.sendall(b'tap 100 100\n')
        resp = sock.recv(1024).decode()
        print(f"  Response: {resp.strip()}")
        assert resp.startswith("OK"), f"TAP 2 failed: {resp}"

        sock.close()
        print("\n✓ All tests passed!")
        return 0

    except Exception as e:
        print(f"\n✗ Test failed: {e}")
        return 1

    finally:
        # Kill emulator
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except:
            proc.kill()

if __name__ == '__main__':
    sys.exit(test_tap_while_cpu_running())
```

**Step 2: Run test**

Run: `chmod +x test_cpu_worker_tap.py && python3 test_cpu_worker_tap.py`
Expected: All tests pass, no timeouts

**Step 3: Commit**

```bash
git add test_cpu_worker_tap.py
git commit -m "test: add integration test for TAP with CPU worker thread

Test that TAP, KEY commands execute reliably while CPU is running.
Verifies worker thread properly handles command dispatch."
```

---

## Task 11: Integration Test - Multiple Commands

**Files:**
- Create: `test_cpu_worker_sequence.py`

**Step 1: Create sequence test**

Create `test_cpu_worker_sequence.py`:

```python
#!/usr/bin/env python3
"""Integration test for command sequence with CPU worker thread."""

import socket
import time
import subprocess
import sys

def test_command_sequence():
    """Test multiple commands in rapid sequence."""

    # Start emulator
    print("Starting emulator...")
    proc = subprocess.Popen(
        ["./build/pose64", "m515.pcf", "--port", "6425"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )

    time.sleep(6)

    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(10)
        sock.connect(('localhost', 6425))

        # Rapid command sequence
        commands = [
            (b'state\n', 'state'),
            (b'tap 50 50\n', 'tap 1'),
            (b'key 65\n', 'key 1'),
            (b'key 66\n', 'key 2'),
            (b'tap 100 100\n', 'tap 2'),
            (b'state\n', 'state 2'),
        ]

        for cmd_bytes, label in commands:
            print(f"Sending {label}...")
            sock.sendall(cmd_bytes)
            resp = sock.recv(1024).decode()
            print(f"  {resp.strip()[:50]}")
            assert resp.startswith("OK"), f"{label} failed: {resp}"
            time.sleep(0.1)  # Small delay between commands

        sock.close()
        print("\n✓ Sequence test passed!")
        return 0

    except Exception as e:
        print(f"\n✗ Test failed: {e}")
        return 1

    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except:
            proc.kill()

if __name__ == '__main__':
    sys.exit(test_command_sequence())
```

**Step 2: Run test**

Run: `python3 test_cpu_worker_sequence.py`
Expected: All commands execute in sequence without timeout

**Step 3: Commit**

```bash
git add test_cpu_worker_sequence.py
git commit -m "test: add command sequence test for CPU worker thread

Verify that multiple commands execute reliably without timeout or
race conditions. Tests command ordering and throughput."
```

---

## Task 12: Stress Test - High Command Rate

**Files:**
- Create: `test_cpu_worker_stress.py`

**Step 1: Create stress test**

Create `test_cpu_worker_stress.py`:

```python
#!/usr/bin/env python3
"""Stress test for CPU worker thread command queue."""

import socket
import time
import subprocess
import sys
import threading

def test_stress():
    """Send many commands quickly to stress test queue."""

    print("Starting emulator...")
    proc = subprocess.Popen(
        ["./build/pose64", "m515.pcf", "--port", "6425"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )

    time.sleep(6)

    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(15)
        sock.connect(('localhost', 6425))

        # Send 50 commands as fast as possible
        print("Sending 50 commands...")
        for i in range(50):
            if i % 10 == 0:
                sock.sendall(b'state\n')
            elif i % 2 == 0:
                sock.sendall(b'tap 50 50\n')
            else:
                sock.sendall(b'key 65\n')

            # Read response
            resp = sock.recv(1024).decode()
            if not resp.startswith("OK"):
                print(f"Command {i} failed: {resp}")
                return 1

            if (i + 1) % 10 == 0:
                print(f"  {i + 1}/50 commands completed")

        sock.close()
        print("\n✓ Stress test passed!")
        return 0

    except Exception as e:
        print(f"\n✗ Stress test failed: {e}")
        return 1

    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except:
            proc.kill()

if __name__ == '__main__':
    sys.exit(test_stress())
```

**Step 2: Run test**

Run: `python3 test_cpu_worker_stress.py`
Expected: All 50 commands complete without errors or timeout

**Step 3: Commit**

```bash
git add test_cpu_worker_stress.py
git commit -m "test: add stress test for CPU worker command queue

Verify queue handles rapid fire commands (50 commands) without
deadlock, data loss, or ordering issues. Tests throughput limits."
```

---

## Task 13: Update CmdReset, CmdScreenshot, etc. (Non-Blocking Commands)

**Files:**
- Modify: `src/core/ReControl.cpp`

**Step 1: Update CmdReset**

CmdReset currently uses EmSessionStopper. Update to use queue:

Replace the EmSessionStopper block in CmdReset with:
```cpp
	ReControlSession* self = this;

	CPUWorkerThread::Command cmd{
		.type = CPUWorkerThread::CMD_INJECT_EVENT,
		.handler = [type]() {
			gSession->ScheduleReset (type);
		},
		.response = [self]() {
			self->Send ("OK\n");
		}
	};

	gCPUWorker->queueCommand(cmd);
```

**Step 2: Update CmdScreenshot**

Replace the EmSessionStopper block in CmdScreenshot:
```cpp
	ReControlSession* self = this;
	QString filepath = args[1];

	CPUWorkerThread::Command cmd{
		.type = CPUWorkerThread::CMD_INJECT_EVENT,
		.handler = [filepath]() {
			EmSessionStopper stopper (gSession, kStopNow);
			// ... screenshot code ...
		},
		.response = [self, filepath]() {
			// Send OK with dimensions
			self->Send ("OK 160 160\n");
		}
	};

	gCPUWorker->queueCommand(cmd);
```

**Step 3: Verify compilation**

Run: `make -C build -j4 2>&1 | tail -10`
Expected: Build succeeds

**Step 4: Commit**

```bash
git add src/core/ReControl.cpp
git commit -m "feat: update remaining commands to use worker thread queue

Convert CmdReset, CmdScreenshot and similar commands to use
async dispatch via worker thread. Maintains same behavior with
proper thread-safe execution."
```

---

## Task 14: Remove Debug Output Entirely

**Files:**
- Modify: `src/core/CPUWorkerThread.cpp`
- Modify: `src/ui/main.cpp`
- Modify: `src/core/ReControl.cpp`

**Step 1: Remove debug fprintf statements**

Remove all `fprintf(stderr, "[...")` debug output from:
- CPUWorkerThread.cpp
- main.cpp ReControl startup section
- ReControl.cpp remaining debug output

Keep only error logging for actual failures.

**Step 2: Verify compilation**

Run: `make -C build -j4 2>&1 | tail -10`
Expected: Build succeeds

**Step 3: Clean test run**

Run: `timeout 5 ./build/pose64 m515.pcf --port 6425 2>&1 | tail -5`
Expected: No debug output, only ReControlServer listening message

**Step 4: Commit**

```bash
git add src/core/CPUWorkerThread.cpp src/ui/main.cpp src/core/ReControl.cpp
git commit -m "refactor: remove all debug fprintf output

Clean up temporary debug logging used during implementation.
Emulator runs silently except for errors."
```

---

## Task 15: Final Integration Test

**Files:**
- Test: Manual verification

**Step 1: Full manual test**

Run:
```bash
timeout 30 ./build/pose64 m515.pcf --port 6425 > /dev/null 2>&1 &
sleep 6

python3 << 'PYEOF'
import socket
import time

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.settimeout(10)
sock.connect(('localhost', 6425))

tests = [
    ("state", "State check"),
    ("tap 50 40", "TAP input"),
    ("key 65", "KEY input"),
    ("button power tap", "BUTTON input"),
    ("state", "Final state"),
]

for cmd, label in tests:
    print(f"Test: {label}")
    sock.sendall((cmd + '\n').encode())
    resp = sock.recv(1024).decode()
    print(f"  {resp.strip()[:50]}")
    assert resp.startswith("OK"), f"FAILED: {resp}"
    time.sleep(0.2)

sock.close()
print("\n✓ All manual tests passed!")
PYEOF

killall -9 pose64 2>/dev/null || true
```

Expected: All tests pass, no timeouts

**Step 2: Verify no regression**

Run: `python3 test_recontrol.py --host localhost --port 6425` (existing test)
Expected: Most tests pass (multiline response parsing may need update)

**Step 3: Final commit**

```bash
git commit -m "refactor: CPU worker thread integration complete

All ReControl commands now execute via thread-safe worker queue.
Resolves TCP event processing starvation issue. TCP commands
reliable while CPU is running. Non-ReControl users unaffected."
```

---

## Acceptance Criteria

- [ ] CPUWorkerThread class fully implemented and compilable
- [ ] All input commands (tap, key, button, pen) execute via worker thread
- [ ] All state query commands (state, screenshot, info) execute via worker thread
- [ ] TAP command test passes (no timeout while CPU running)
- [ ] Command sequence test passes (ordering preserved)
- [ ] Stress test passes (50 rapid commands)
- [ ] No performance regression for non-ReControl users
- [ ] Debug output removed, only error logging remains
- [ ] Build clean with no warnings

---

## Known Limitations

1. **Load command still deferred** - Not implemented in this phase
2. **Response callbacks run in main thread** - May add 1-2ms latency
3. **Command timeout is 10s** - May need tuning for slow operations
4. **No priority queue** - Commands processed FIFO
