/* -*- mode: C++; tab-width: 4 -*- */
/* ReControl: TCP control interface for POSE64. */

#ifndef ReControl_h
#define ReControl_h

#include "EmTypes.h"		// emuptr — for the ParseAddress declaration below
							// (AUTOMOC TUs include this header without EmCommon.h)

#include <string>
#include <functional>
#include <QObject>
#include <QStringList>
#include <QByteArray>

// Forward declarations
class CPUWorkerThread;
class QTcpSocket;
class QTcpServer;

// Global instance - initialized in main.cpp
extern CPUWorkerThread* gCPUWorker;

// ---------------------------------------------------------------------------
// Command dispatch types
// ---------------------------------------------------------------------------

enum CommandCategory {
	kCmdImmediate,       // main thread, no stopper
	kCmdWorkerDirect,    // worker thread, no stopper, always sends OK (hardware-path input only)
	kCmdWorkerCycle,     // worker thread, kStopOnCycle, returns result
	kCmdWorkerSysCall,   // worker thread, kStopOnSysCall + timeout
	kCmdWorkerRaw,       // worker thread, no stopper (handler creates own); runs main-thread validate if present
	kCmdAdaptive,        // direct if blocked_on_ui, else kStopOnCycle
	kCmdCustom           // handler manages own threading
};

// Standard handler: returns result string ("OK\n", "ERR ...\n", multi-line).
// Never calls Send/SendErr/QueueWork directly.
using CmdHandler = std::string (*)(const QStringList& args);

// Custom handler: receives session for direct I/O control.
class ReControlSession;
using CustomCmdHandler = void (*)(ReControlSession* session,
                                  const QStringList& args);

struct CommandEntry {
	const char*      name;
	CommandCategory  category;
	int              timeoutMs;      // for kCmdWorkerSysCall (0 = default 5000)
	CmdHandler       handler;        // non-null for standard categories
	CustomCmdHandler customHandler;  // non-null for kCmdCustom
	CmdHandler       validate = nullptr;  // optional main-thread arg validator
	                                      // (kCmdWorkerDirect/kCmdWorkerRaw); returns "" if OK
};

// ---------------------------------------------------------------------------
// ReControlServer — TCP server, manages sessions
// ---------------------------------------------------------------------------

class ReControlServer : public QObject
{
	Q_OBJECT

public:
	ReControlServer (int port, QObject* parent = nullptr);
	~ReControlServer ();

	bool IsListening () const;
	void NotifySessionGone (ReControlSession* session);

private slots:
	void OnNewConnection ();

private:
	QTcpServer* fServer;
	ReControlSession* fActiveSession;
};

// ---------------------------------------------------------------------------
// ReControlSession — One per connected client
// ---------------------------------------------------------------------------

class ReControlSession : public QObject
{
	Q_OBJECT

public:
	ReControlSession (QTcpSocket* socket, ReControlServer* server);
	~ReControlSession ();

	// Public for custom command handlers
	void Send (const std::string& msg);
	void SendErr (const std::string& category, const std::string& msg);
	void PauseProcessing (int ms);
	void DoMenuLookup (std::string menuTitle, std::string itemTitle, bool activated);
	void QueueWork (std::function<void()> handler);
	void QueueWorkResult (std::function<std::string()> handler);

private slots:
	void OnReadyRead ();
	void OnDisconnected ();
	void OnSleepDone ();

private:
	void ProcessBufferedCommands (void);
	void DispatchCommand (const QStringList& parts);

	QTcpSocket* fSocket;
	ReControlServer* fServer;
	QByteArray fReadBuffer;
	bool fProcessingPaused;
	QStringList fCommandBuffer;
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void ReControl_Startup (int port);
void ReControl_Shutdown (void);

// ---------------------------------------------------------------------------
// Shared command helpers
// ---------------------------------------------------------------------------
// Parses a hex/decimal address string into emuptr.  Defined in
// ReControlCmds_Query.cpp.  (emuptr comes from EmCommon.h, which every
// ReControl consumer includes before ReControl.h.)
bool ParseAddress (const std::string& addrStr, emuptr& outAddr);

#endif /* ReControl_h */
