/* -*- mode: C++; tab-width: 4 -*- */
/* ReControl: TCP control interface for POSE64. */

#ifndef ReControl_h
#define ReControl_h

#include <string>
#include <QStringList>

// Forward declarations
class CPUWorkerThread;
class ReControlSession;

// Global instance - initialized in main.cpp
extern CPUWorkerThread* gCPUWorker;

// ---------------------------------------------------------------------------
// Command dispatch types
// ---------------------------------------------------------------------------

enum CommandCategory {
	kCmdImmediate,       // main thread, no stopper
	kCmdWorkerDirect,    // worker thread, no stopper, always sends OK
	kCmdWorkerCycle,     // worker thread, kStopOnCycle, returns result
	kCmdWorkerSysCall,   // worker thread, kStopOnSysCall + timeout
	kCmdWorkerRaw,       // worker thread, no stopper (handler creates own)
	kCmdAdaptive,        // direct if blocked_on_ui, else kStopOnCycle
	kCmdCustom           // handler manages own threading
};

// Standard handler: returns result string ("OK\n", "ERR ...\n", multi-line).
// Never calls Send/SendErr/QueueWork directly.
using CmdHandler = std::string (*)(const QStringList& args);

// Custom handler: receives session for direct I/O control.
using CustomCmdHandler = void (*)(ReControlSession* session,
                                  const QStringList& args);

struct CommandEntry {
	const char*      name;
	CommandCategory  category;
	int              timeoutMs;      // for kCmdWorkerSysCall (0 = default 5000)
	CmdHandler       handler;        // non-null for standard categories
	CustomCmdHandler customHandler;  // non-null for kCmdCustom
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void ReControl_Startup (int port);
void ReControl_Shutdown (void);

#endif /* ReControl_h */
