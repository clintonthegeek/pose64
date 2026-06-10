/* -*- mode: C++; tab-width: 4 -*- */
/* ReControl: TCP control interface for POSE64.
 *
 * Implements ReControlServer and ReControlSession classes for
 * TCP-based command dispatch. All I/O happens on the Qt event loop
 * (UI thread), eliminating race conditions with SuspendThread.
 *
 * Command handlers live in ReControlCmds_*.cpp files.
 * This file contains infrastructure: server, session, dispatch table,
 * dispatch loop, QueueWork/QueueWorkResult, and DoMenuLookup.
 */

#include <QTcpServer>
#include <QTcpSocket>
#include <QPointer>
#include <QTimer>
#include <QApplication>

#include <string>
#include <cstring>

#include "EmCommon.h"

// Undefine Palm OS macros that conflict with Qt
#undef daysInYear
#undef monthsInYear

#include "ReControl.h"
#include "EmSession.h"
#include "EmLowMem.h"
#include "Hardware/EmMemory.h"
#include "ROMStubs.h"
#include "PalmFormReader.h"
#include "CPUWorkerThread.h"

// Global CPU worker thread instance
CPUWorkerThread* gCPUWorker = nullptr;

static ReControlServer* gReControlServer = nullptr;

// ============================================================================
// Command handler forward declarations
// (Defined in ReControlCmds_*.cpp files)
// ============================================================================

// Session commands (ReControlCmds_Session.cpp)
std::string RcCmd_State (const QStringList& args);
std::string RcCmd_Quit (const QStringList& args);
void        RcCmd_Reset (ReControlSession* s, const QStringList& args);
void        RcCmd_Sleep (ReControlSession* s, const QStringList& args);
std::string RcCmd_Install (const QStringList& args);
std::string RcCmd_Export (const QStringList& args);
std::string RcCmd_Launch (const QStringList& args);
std::string RcCmd_Save (const QStringList& args);
void        RcCmd_Load (ReControlSession* s, const QStringList& args);
void        RcCmd_Info (ReControlSession* s, const QStringList& args);
std::string RcCmd_Apps (const QStringList& args);
void        RcCmd_Dialog (ReControlSession* s, const QStringList& args);
std::string RcCmd_Delete (const QStringList& args);

// Input commands (ReControlCmds_Input.cpp)
std::string RcCmd_Tap (const QStringList& args);
std::string RcCmd_TapId (const QStringList& args);
std::string RcCmd_Pen (const QStringList& args);
std::string RcCmd_Key (const QStringList& args);
std::string RcCmd_Type (const QStringList& args);
std::string RcCmd_Button (const QStringList& args);
void        RcCmd_Menu (ReControlSession* s, const QStringList& args);
void        RcCmd_Run (ReControlSession* s, const QStringList& args);

// Main-thread argument validators for WorkerDirect commands (ReControlCmds_Input.cpp)
std::string RcValidate_Tap (const QStringList& args);
std::string RcValidate_Pen (const QStringList& args);
std::string RcValidate_Key (const QStringList& args);
std::string RcValidate_Type (const QStringList& args);
std::string RcValidate_Button (const QStringList& args);

// Query commands (ReControlCmds_Query.cpp)
std::string RcCmd_Screenshot (const QStringList& args);
std::string RcCmd_ScreenHash (const QStringList& args);
std::string RcCmd_UI (const QStringList& args);
std::string RcCmd_Peek (const QStringList& args);
std::string RcCmd_Poke (const QStringList& args);
std::string RcCmd_Regs (const QStringList& args);
std::string RcCmd_Backtrace (const QStringList& args);

// Debug commands (ReControlCmds_Debug.cpp)
std::string RcCmd_Break (const QStringList& args);
std::string RcCmd_Watch (const QStringList& args);
std::string RcCmd_Spy (const QStringList& args);
std::string RcCmd_Log (const QStringList& args);
std::string RcCmd_Gremlin (const QStringList& args);
std::string RcCmd_Check (const QStringList& args);
std::string RcCmd_ErrorHandling (const QStringList& args);

// Profile commands (ReControlCmds_Profile.cpp)
#if HAS_PROFILING
std::string RcCmd_Profile (const QStringList& args);
#endif

// ============================================================================
// Dispatch table
// ============================================================================
// {name, category, timeoutMs, handler, customHandler}

static const CommandEntry sCommandTable[] = {
	// Session
	{"state",          kCmdImmediate,     0,    RcCmd_State,      nullptr},
	{"quit",           kCmdImmediate,     0,    RcCmd_Quit,       nullptr},
	{"reset",          kCmdCustom,        0,    nullptr,          RcCmd_Reset},
	{"sleep",          kCmdCustom,        0,    nullptr,          RcCmd_Sleep},
	{"install",        kCmdWorkerRaw,     0,    RcCmd_Install,    nullptr},
	{"export",         kCmdWorkerSysCall, 5000, RcCmd_Export,     nullptr},
	{"launch",         kCmdWorkerSysCall, 5000, RcCmd_Launch,     nullptr},
	{"save",           kCmdWorkerRaw,     0,    RcCmd_Save,       nullptr},
	{"load",           kCmdCustom,        0,    nullptr,          RcCmd_Load},
	{"info",           kCmdCustom,        0,    nullptr,          RcCmd_Info},
	{"apps",           kCmdWorkerSysCall, 5000, RcCmd_Apps,       nullptr},
	{"dialog",         kCmdCustom,        0,    nullptr,          RcCmd_Dialog},
	{"delete",         kCmdWorkerSysCall, 5000, RcCmd_Delete,     nullptr},

	// Input
	{"tap",            kCmdWorkerDirect,  0,    RcCmd_Tap,        nullptr, RcValidate_Tap},
	{"tap-id",         kCmdWorkerCycle,   0,    RcCmd_TapId,      nullptr},
	{"pen",            kCmdWorkerDirect,  0,    RcCmd_Pen,        nullptr, RcValidate_Pen},
	{"key",            kCmdWorkerDirect,  0,    RcCmd_Key,        nullptr, RcValidate_Key},
	{"type",           kCmdWorkerDirect,  0,    RcCmd_Type,       nullptr, RcValidate_Type},
	{"button",         kCmdWorkerDirect,  0,    RcCmd_Button,     nullptr, RcValidate_Button},
	{"menu",           kCmdCustom,        0,    nullptr,          RcCmd_Menu},
	{"run",            kCmdCustom,        0,    nullptr,          RcCmd_Run},

	// Query
	{"screenshot",     kCmdWorkerRaw,     0,    RcCmd_Screenshot, nullptr},
	{"screen-hash",    kCmdWorkerRaw,     0,    RcCmd_ScreenHash, nullptr},
	{"ui",             kCmdWorkerCycle,   0,    RcCmd_UI,         nullptr},
	{"peek",           kCmdAdaptive,      0,    RcCmd_Peek,       nullptr},
	{"poke",           kCmdAdaptive,      0,    RcCmd_Poke,       nullptr},
	{"regs",           kCmdAdaptive,      0,    RcCmd_Regs,       nullptr},
	{"backtrace",      kCmdAdaptive,      0,    RcCmd_Backtrace,  nullptr},
	{"bt",             kCmdAdaptive,      0,    RcCmd_Backtrace,  nullptr},

	// Debug
	{"break",          kCmdWorkerCycle,   0,    RcCmd_Break,      nullptr},
	{"watch",          kCmdWorkerCycle,   0,    RcCmd_Watch,      nullptr},
	{"spy",            kCmdWorkerCycle,   0,    RcCmd_Spy,        nullptr},
	{"log",            kCmdImmediate,     0,    RcCmd_Log,        nullptr},
	{"gremlin",        kCmdWorkerRaw,     0,    RcCmd_Gremlin,    nullptr},
	{"check",          kCmdImmediate,     0,    RcCmd_Check,      nullptr},
	{"errorhandling",  kCmdImmediate,     0,    RcCmd_ErrorHandling, nullptr},

	// Profile
#if HAS_PROFILING
	{"profile",        kCmdWorkerRaw,     0,    RcCmd_Profile,    nullptr},
#endif

	{nullptr, kCmdImmediate, 0, nullptr, nullptr}  // sentinel
};

// ============================================================================
// ReControlSession — Implementation
// ============================================================================

ReControlSession::ReControlSession (QTcpSocket* socket, ReControlServer* server)
	: fSocket (socket),
	  fServer (server),
	  fReadBuffer (),
	  fProcessingPaused (false),
	  fCommandBuffer ()
{
	fSocket->setParent (this);

	connect (fSocket, &QTcpSocket::readyRead,
			 this, &ReControlSession::OnReadyRead);
	connect (fSocket, &QTcpSocket::disconnected,
			 this, &ReControlSession::OnDisconnected);
}

ReControlSession::~ReControlSession ()
{
}

void ReControlSession::Send (const std::string& msg)
{
	if (fSocket)
	{
		fSocket->write (msg.c_str ());
		fSocket->flush ();
	}
}

void ReControlSession::SendErr (const std::string& category, const std::string& msg)
{
	Send ("ERR " + category + ": " + msg + "\n");
}

void ReControlSession::PauseProcessing (int ms)
{
	fProcessingPaused = true;
	QTimer::singleShot (ms, this, &ReControlSession::OnSleepDone);
}

void ReControlSession::QueueWork (std::function<void()> handler)
{
	if (!gCPUWorker) { SendErr ("transient", "no CPU worker"); return; }

	QPointer<ReControlSession> safeRef (this);

	CPUWorkerThread::Command cmd{
		.type = CPUWorkerThread::CMD_INJECT_EVENT,
		.handler = [handler = std::move (handler)]() {
			try {
				handler ();
			} catch (...) {
				// Swallow to prevent worker thread death.
			}
		},
		.response = [safeRef]() {
			if (safeRef) safeRef->Send ("OK\n");
		}
	};

	gCPUWorker->queueCommand (cmd);
}

void ReControlSession::QueueWorkResult (std::function<std::string()> handler)
{
	if (!gCPUWorker) { SendErr ("transient", "no CPU worker"); return; }

	QPointer<ReControlSession> safeRef (this);
	auto result = std::make_shared<std::string>();

	CPUWorkerThread::Command cmd{
		.type = CPUWorkerThread::CMD_INJECT_EVENT,
		.handler = [result, handler = std::move (handler)]() {
			try {
				*result = handler ();
			} catch (const std::exception& e) {
				*result = "ERR internal: " + std::string (e.what ()) + "\n";
			} catch (...) {
				*result = "ERR internal: unhandled exception in worker\n";
			}
		},
		.response = [safeRef, result]() {
			if (safeRef) safeRef->Send (*result);
		}
	};

	gCPUWorker->queueCommand (cmd);
}

// ============================================================================
// DoMenuLookup — async helper for the 'menu' command
// ============================================================================
// Kept as a method because it uses QTimer::singleShot for retry and captures this.

void ReControlSession::DoMenuLookup (std::string menuTitle, std::string itemTitle, bool activated)
{
	if (!gCPUWorker) { SendErr ("transient", "no CPU worker"); return; }

	QPointer<ReControlSession> safeRef (this);
	auto result = std::make_shared<std::string>();

	CPUWorkerThread::Command cmd{
		.type = CPUWorkerThread::CMD_INJECT_EVENT,
		.handler = [result, menuTitle, itemTitle, activated]() {
			if (!gSession) { *result = "ERR transient: no session\n"; return; }
			EmSessionStopper stopper (gSession, kStopOnSysCall, 5000);
			if (!stopper.Stopped () || !stopper.CanCall ())
			{
				*result = "ERR timeout: CPU did not reach syscall boundary within 5000ms\n";
				return;
			}

			CEnableFullAccess munge;

			emuptr menuBarPtr = EmLowMem_GetGlobal (uiGlobalsCommon.uiCurrentMenu);
			if (menuBarPtr == 0)
			{
				if (!activated)
				{
					emuptr formPtr = EmLowMem_GetGlobal (uiGlobalsCommon.currentForm);
					if (formPtr == 0) { *result = "ERR transient: no active form\n"; return; }

					uint16 menuRscId = EmMemGet16 (formPtr + kFormType_menuRscId);
					if (menuRscId == 0)
					{
						*result = "ERR transient: current form has no menu bar\n";
						return;
					}

					gSession->PostKeyEvent (EmKeyEvent (0x0105));
					*result = "";  // empty = signal to retry
					return;
				}
				*result = "ERR transient: no menu bar active\n";
				return;
			}

			// Menu bar is active — walk the structure
			int16 numMenus = (int16) EmMemGet16 (menuBarPtr + kMenuBarType_numMenus);
			emuptr menusPtr = EmMemGet32 (menuBarPtr + kMenuBarType_menus);
			if (menusPtr == 0 || numMenus <= 0 || numMenus > 20)
			{
				*result = "ERR transient: invalid menu bar structure\n";
				return;
			}

			for (int m = 0; m < numMenus; m++)
			{
				emuptr pullDown = menusPtr + (m * kMenuPullDownType_size);
				emuptr titlePtr = EmMemGet32 (pullDown + kMenuPullDownType_title);
				if (titlePtr == 0) continue;

				char titleBuf[256];
				int ti = 0;
				for (; ti < 255; ti++)
				{
					uint8 ch = EmMemGet8 (titlePtr + ti);
					if (ch == 0) break;
					titleBuf[ti] = (char) ch;
				}
				titleBuf[ti] = '\0';

				if (strcasecmp (titleBuf, menuTitle.c_str ()) != 0)
					continue;

				uint16 hiddenNumItems = EmMemGet16 (pullDown + kMenuPullDownType_hiddenNumItems);
				int numItems = hiddenNumItems & 0x7FFF;
				emuptr itemsPtr = EmMemGet32 (pullDown + kMenuPullDownType_items);
				if (itemsPtr == 0 || numItems <= 0) continue;

				int maxItems = (numItems > 30) ? 30 : numItems;
				for (int i = 0; i < maxItems; i++)
				{
					emuptr item = itemsPtr + (i * kMenuItemType_size);
					uint16 id = EmMemGet16 (item + kMenuItemType_id);
					uint8 hiddenByte = EmMemGet8 (item + kMenuItemType_hidden);
					emuptr itemStr = EmMemGet32 (item + kMenuItemType_itemStr);

					if (hiddenByte & 0x80) continue;
					if (itemStr == 0) continue;

					char itemBuf[256];
					int ii = 0;
					for (; ii < 255; ii++)
					{
						uint8 ch = EmMemGet8 (itemStr + ii);
						if (ch == 0) break;
						itemBuf[ii] = (char) ch;
					}
					itemBuf[ii] = '\0';

					bool match = false;
					if (strcasecmp (itemBuf, itemTitle.c_str ()) == 0)
						match = true;
					else
					{
						std::string lowerItem (itemBuf);
						std::string lowerSearch (itemTitle);
						for (char& c : lowerItem) c = tolower (c);
						for (char& c : lowerSearch) c = tolower (c);
						if (lowerItem.find (lowerSearch) != std::string::npos)
							match = true;
					}

					if (!match) continue;

					try
					{
						EventType event;
						memset (&event, 0, sizeof (event));
						event.eType = menuEvent;
						event.data.menu.itemID = id;
						EvtAddEventToQueue (&event);
					}
					catch (...)
					{
						*result = "ERR fatal: exception posting menuEvent\n";
						return;
					}

					*result = "OK id=" + std::to_string (id) + "\n";
					return;
				}

				*result = "ERR usage: item '" + itemTitle + "' not found in menu '" + menuTitle + "'\n";
				return;
			}

			*result = "ERR usage: menu '" + menuTitle + "' not found\n";
		},
		.response = [safeRef, result, menuTitle, itemTitle]() {
			if (!safeRef) return;
			if (result->empty ())
			{
				QTimer::singleShot (300, safeRef, [safeRef, menuTitle, itemTitle]() {
					if (safeRef)
						safeRef->DoMenuLookup (menuTitle, itemTitle, true);
				});
			}
			else
			{
				safeRef->Send (*result);
			}
		}
	};

	gCPUWorker->queueCommand (cmd);
}

// ============================================================================
// Table-driven dispatch loop
// ============================================================================

void ReControlSession::DispatchCommand (const QStringList& parts)
{
	if (parts.isEmpty ()) return;
	std::string cmdName = parts[0].toLower ().toStdString ();

	// Look up command in table
	const CommandEntry* entry = nullptr;
	for (const CommandEntry* e = sCommandTable; e->name; e++)
	{
		if (cmdName == e->name)
		{
			entry = e;
			break;
		}
	}

	if (!entry)
	{
		SendErr ("usage", "unknown command '" + cmdName + "'");
		return;
	}

	switch (entry->category)
	{
		case kCmdImmediate:
		{
			std::string result = entry->handler (parts);
			Send (result);
			break;
		}

		case kCmdWorkerDirect:
		{
			if (!gSession) { SendErr ("transient", "no session"); return; }
			// Validate args on the MAIN thread before queueing so malformed
			// input returns ERR usage immediately instead of a swallowed OK
			// (and without depending on the worker thread being responsive).
			if (entry->validate)
			{
				std::string verr = entry->validate (parts);
				if (!verr.empty ()) { Send (verr); return; }
			}
			auto handler = entry->handler;
			QueueWork ([handler, parts]() {
				if (!gSession) return;
				handler (parts);
			});
			break;
		}

		case kCmdWorkerCycle:
		{
			if (!gSession) { SendErr ("transient", "no session"); return; }
			auto handler = entry->handler;
			QueueWorkResult ([handler, parts]() -> std::string {
				if (!gSession) return "ERR transient: no session\n";
				EmSessionStopper stopper (gSession, kStopOnCycle, 5000);
				if (!stopper.Stopped ())
					return "ERR timeout: CPU did not reach a cycle boundary within 5000ms. "
					       "Recovery: dismiss any dialog (dialog respond) or palm_reset.\n";
				return handler (parts);
			});
			break;
		}

		case kCmdWorkerSysCall:
		{
			if (!gSession) { SendErr ("transient", "no session"); return; }
			auto handler = entry->handler;
			int timeout = entry->timeoutMs > 0 ? entry->timeoutMs : 5000;
			QueueWorkResult ([handler, parts, timeout]() -> std::string {
				if (!gSession) return "ERR transient: no session\n";
				EmSessionStopper stopper (gSession, kStopOnSysCall, timeout);
				if (!stopper.Stopped () || !stopper.CanCall ())
				{
					char msg[256];
					snprintf (msg, sizeof (msg),
						"ERR timeout: CPU did not reach syscall boundary within %dms\n",
						timeout);
					return msg;
				}
				return handler (parts);
			});
			break;
		}

		case kCmdWorkerRaw:
		{
			if (!gSession) { SendErr ("transient", "no session"); return; }
			auto handler = entry->handler;
			QueueWorkResult ([handler, parts]() -> std::string {
				if (!gSession) return "ERR transient: no session\n";
				return handler (parts);
			});
			break;
		}

		case kCmdAdaptive:
		{
			if (!gSession) { SendErr ("transient", "no session"); return; }
			auto handler = entry->handler;
			if (gSession->GetSessionState () == kBlockedOnUI)
			{
				// CPU is blocked on dialog — memory is stable, run directly
				std::string result = handler (parts);
				Send (result);
			}
			else
			{
				QueueWorkResult ([handler, parts]() -> std::string {
					if (!gSession) return "ERR transient: no session\n";
					EmSessionStopper stopper (gSession, kStopOnCycle, 5000);
					if (!stopper.Stopped ())
						return "ERR timeout: CPU did not reach a cycle boundary within 5000ms. "
						       "Recovery: dismiss any dialog (dialog respond) or palm_reset.\n";
					return handler (parts);
				});
			}
			break;
		}

		case kCmdCustom:
		{
			entry->customHandler (this, parts);
			break;
		}
	}
}

// ============================================================================
// Session I/O and command buffering
// ============================================================================

void ReControlSession::OnReadyRead ()
{
	QByteArray data = fSocket->readAll ();
	fReadBuffer.append (data);

	while (true)
	{
		int newlinePos = fReadBuffer.indexOf ('\n');
		if (newlinePos < 0)
			break;

		QString line = QString::fromUtf8 (fReadBuffer.left (newlinePos)).trimmed ();
		fReadBuffer.remove (0, newlinePos + 1);

		if (line.isEmpty ())
			continue;

		if (fProcessingPaused)
		{
			fCommandBuffer.append (line);
			continue;
		}

		QStringList parts = line.split (' ', Qt::SkipEmptyParts);
		DispatchCommand (parts);
	}
}

void ReControlSession::OnDisconnected ()
{
	if (fServer)
		fServer->NotifySessionGone (this);

	deleteLater ();
}

void ReControlSession::OnSleepDone ()
{
	Send ("OK\n");
	fProcessingPaused = false;
	ProcessBufferedCommands ();
}

void ReControlSession::ProcessBufferedCommands ()
{
	while (!fCommandBuffer.isEmpty ())
	{
		QString line = fCommandBuffer.takeFirst ();
		QStringList parts = line.split (' ', Qt::SkipEmptyParts);
		DispatchCommand (parts);
	}
}

// ============================================================================
// ReControlServer — Implementation
// ============================================================================

ReControlServer::ReControlServer (int port, QObject* parent)
	: QObject (parent),
	  fServer (nullptr),
	  fActiveSession (nullptr)
{
	if (port <= 0)
		return;

	fServer = new QTcpServer (this);

	connect (fServer, &QTcpServer::newConnection,
			 this, &ReControlServer::OnNewConnection);

	if (!fServer->listen (QHostAddress::LocalHost, port))
	{
		fprintf (stderr, "ReControlServer: Failed to listen on port %d\n", port);
		delete fServer;
		fServer = nullptr;
		return;
	}

	fprintf (stderr, "ReControlServer: Listening on localhost:%d\n", port);
}

ReControlServer::~ReControlServer ()
{
	if (fServer)
	{
		fServer->close ();
	}
}

bool ReControlServer::IsListening () const
{
	return fServer && fServer->isListening ();
}

void ReControlServer::OnNewConnection ()
{
	QTcpSocket* socket = fServer->nextPendingConnection ();
	if (!socket)
		return;

	if (fActiveSession)
	{
		socket->write ("ERR busy\n");
		socket->flush ();
		socket->disconnectFromHost ();
		socket->deleteLater ();
		return;
	}

	ReControlSession* session = new ReControlSession (socket, this);
	fActiveSession = session;
}

void ReControlServer::NotifySessionGone (ReControlSession* session)
{
	if (fActiveSession == session)
		fActiveSession = nullptr;
}

// ============================================================================
// Public API
// ============================================================================

void ReControl_Startup (int port)
{
	if (port <= 0)
		return;

	gReControlServer = new ReControlServer (port);
}

void ReControl_Shutdown (void)
{
	if (gReControlServer)
	{
		delete gReControlServer;
		gReControlServer = nullptr;
	}
}
