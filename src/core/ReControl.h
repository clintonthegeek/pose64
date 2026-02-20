/* -*- mode: C++; tab-width: 4 -*- */
/* ReControl: TCP control interface for POSE64.
 * Design: docs/plans/2026-02-19-recontrol-v2-design.md
 */

#ifndef ReControl_h
#define ReControl_h

// Forward declaration
class CPUWorkerThread;

// Global instance - initialized in main.cpp
extern CPUWorkerThread* gCPUWorker;

// Start the ReControl TCP server on the given port.
// Call after theApp.Startup() and before qtApp.exec().
// Pass 0 to disable ReControl.
void ReControl_Startup (int port);

// Shut down the ReControl server.
// Call after qtApp.exec() returns and before theApp.Shutdown().
void ReControl_Shutdown (void);

#endif /* ReControl_h */
