/* -*- mode: C++; tab-width: 4 -*- */
/* ReControl: TCP control interface for POSE64.
 *
 * Implements ReControlServer and ReControlSession classes for
 * TCP-based command dispatch. All I/O happens on the Qt event loop
 * (UI thread), eliminating race conditions with SuspendThread.
 */

#include <QTcpServer>
#include <QTcpSocket>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QImage>
#include <QFileInfo>
#include <QApplication>

#include <string>
#include <memory>
#include <cstring>
#include <zlib.h>

#include "EmCommon.h"

// Undefine Palm OS macros that conflict with Qt
#undef daysInYear
#undef monthsInYear

#include "ReControl.h"
#include "EmSession.h"
#include "EmApplication.h"
#include "EmDocument.h"
#include "Skins.h"
#include "EmTypes.h"
#include "EmScreen.h"
#include "EmFileImport.h"
#include "EmStreamFile.h"
#include "ROMStubs.h"
#include "PalmFormReader.h"
#include "Hardware/EmMemory.h"
#include "EmLowMem.h"
#include "CPUWorkerThread.h"

// Forward declarations
class ReControlServer;
class ReControlSession;

// Global CPU worker thread instance
CPUWorkerThread* gCPUWorker = nullptr;

static ReControlServer* gReControlServer = nullptr;

// ============================================================================
// ReControlSession - One per connected client
// ============================================================================

class ReControlSession : public QObject
{
	Q_OBJECT

public:
	ReControlSession (QTcpSocket* socket, ReControlServer* server);
	~ReControlSession ();

private slots:
	void OnReadyRead ();
	void OnDisconnected ();
	void OnSleepDone ();

private:
	void Send (const std::string& msg);
	void SendErr (const std::string& category, const std::string& msg);

	void CmdState (const QStringList& args);
	void CmdQuit (const QStringList& args);
	void CmdTap (const QStringList& args);
	void CmdPen (const QStringList& args);
	void CmdKey (const QStringList& args);
	void CmdButton (const QStringList& args);
	void CmdReset (const QStringList& args);
	void CmdSleep (const QStringList& args);
	void CmdScreenshot (const QStringList& args);
	void CmdInstall (const QStringList& args);
	void CmdLaunch (const QStringList& args);
	void CmdSave (const QStringList& args);
	void CmdLoad (const QStringList& args);
	void CmdInfo (const QStringList& args);
	void CmdUI (const QStringList& args);
	void CmdScreenHash (const QStringList& args);
	void CmdType (const QStringList& args);
	void CmdTapId (const QStringList& args);
	void CmdApps (const QStringList& args);
	void ProcessBufferedCommands (void);
	void DispatchCommand (const QStringList& parts);

	void QueueWork (std::function<void()> handler);
	void QueueWorkResult (std::function<std::string()> handler);

	QTcpSocket* fSocket;
	ReControlServer* fServer;
	QByteArray fReadBuffer;
	bool fProcessingPaused;
	QStringList fCommandBuffer;
};

// ============================================================================
// ReControlServer - TCP server, manages sessions
// ============================================================================

class ReControlServer : public QObject
{
	Q_OBJECT

public:
	ReControlServer (int port, QObject* parent = nullptr);
	~ReControlServer ();

	bool IsListening () const { return fServer && fServer->isListening (); }
	void NotifySessionGone (ReControlSession* session);

private slots:
	void OnNewConnection ();

private:
	QTcpServer* fServer;
	ReControlSession* fActiveSession;
};

// ============================================================================
// ReControlSession - Implementation
// ============================================================================

ReControlSession::ReControlSession (QTcpSocket* socket, ReControlServer* server)
	: fSocket (socket),
	  fServer (server),
	  fReadBuffer (),
	  fProcessingPaused (false),
	  fCommandBuffer ()
{
	// Move socket to this object (not strictly necessary on single-threaded,
	// but good practice for Qt)
	fSocket->setParent (this);

	// Connect signals
	connect (fSocket, &QTcpSocket::readyRead,
			 this, &ReControlSession::OnReadyRead);
	connect (fSocket, &QTcpSocket::disconnected,
			 this, &ReControlSession::OnDisconnected);
}

ReControlSession::~ReControlSession ()
{
	// Socket will be deleted as a child
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

void ReControlSession::QueueWork (std::function<void()> handler)
{
	if (!gCPUWorker) { SendErr ("transient", "no CPU worker"); return; }

	QPointer<ReControlSession> safeRef (this);

	CPUWorkerThread::Command cmd{
		.type = CPUWorkerThread::CMD_INJECT_EVENT,
		.handler = std::move (handler),
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
			*result = handler ();
		},
		.response = [safeRef, result]() {
			if (safeRef) safeRef->Send (*result);
		}
	};

	gCPUWorker->queueCommand (cmd);
}

void ReControlSession::CmdState (const QStringList& args)
{
	if (!gSession)
	{
		Send ("OK stopped\n");
		return;
	}

	EmSessionState state = gSession->GetSessionState ();
	switch (state)
	{
		case kRunning:
			Send ("OK running\n");
			break;
		case kSuspended:
		{
			// Include suspend counter diagnostics
			EmSuspendState suspendState = gSession->GetSuspendState ();
			EmSuspendCounters counters = suspendState.fCounters;

			// Determine primary reason for suspension
			// Priority: debugger > external > ui > timeout > syscall > subreturn
			std::string reason = "unknown";
			if (counters.fSuspendByDebugger)
				reason = "debugger";
			else if (counters.fSuspendByExternal)
				reason = "external";
			else if (counters.fSuspendByUIThread)
				reason = "ui";
			else if (counters.fSuspendByTimeout)
				reason = "timeout";
			else if (counters.fSuspendBySysCall)
				reason = "syscall";
			else if (counters.fSuspendBySubroutineReturn)
				reason = "subreturn";

			char buffer[256];
			snprintf (buffer, sizeof (buffer),
				"OK suspended:%s ui=%d dbg=%d ext=%d timeout=%d syscall=%d subret=%d\n",
				reason.c_str (),
				counters.fSuspendByUIThread,
				counters.fSuspendByDebugger,
				counters.fSuspendByExternal,
				counters.fSuspendByTimeout,
				counters.fSuspendBySysCall,
				counters.fSuspendBySubroutineReturn);
			Send (buffer);
			break;
		}
		case kStopped:
			Send ("OK stopped\n");
			break;
		case kBlockedOnUI:
			Send ("OK blocked_on_ui\n");
			break;
	}
}

void ReControlSession::CmdQuit (const QStringList& args)
{
	Send ("OK\n");
	gApplication->SetTimeToQuit (true);
}

void ReControlSession::CmdTap (const QStringList& args)
{
	if (args.size () != 3) { SendErr ("usage", "tap <x> <y>"); return; }
	if (!gSession) { SendErr ("transient", "no session"); return; }

	int x = args[1].toInt ();
	int y = args[2].toInt ();

	QueueWork ([x, y]() {
		if (!gSession) return;
		EmPenEvent penDown (EmPoint (x, y), true);
		gSession->PostPenEvent (penDown);

		EmPenEvent penUp (EmPoint (-1, -1), false);
		gSession->PostPenEvent (penUp);
	});
}

void ReControlSession::CmdPen (const QStringList& args)
{
	if (args.size () != 4) { SendErr ("usage", "pen <down|up> <x> <y>"); return; }
	if (!gSession) { SendErr ("transient", "no session"); return; }

	QString state = args[1].toLower ();
	bool isDown = (state == "down");
	if (state != "down" && state != "up") { SendErr ("usage", "pen <down|up> <x> <y>"); return; }

	int x = args[2].toInt ();
	int y = args[3].toInt ();

	QueueWork ([x, y, isDown]() {
		if (!gSession) return;
		EmPenEvent penEvent (EmPoint (x, y), isDown);
		gSession->PostPenEvent (penEvent);
	});
}

void ReControlSession::CmdKey (const QStringList& args)
{
	if (args.size () != 2) { SendErr ("usage", "key <charcode>"); return; }
	if (!gSession) { SendErr ("transient", "no session"); return; }

	int charcode = args[1].toInt ();

	QueueWork ([charcode]() {
		if (!gSession) return;
		EmKeyEvent keyEvent (charcode);
		gSession->PostKeyEvent (keyEvent);
	});
}

void ReControlSession::CmdButton (const QStringList& args)
{
	if (args.size () != 3) { SendErr ("usage", "button <name> <down|up|tap>"); return; }
	if (!gSession) { SendErr ("transient", "no session"); return; }

	QString name = args[1].toLower ();
	QString action = args[2].toLower ();

	SkinElementType button = kElement_None;
	if (name == "power")      button = kElement_PowerButton;
	else if (name == "up")    button = kElement_UpButton;
	else if (name == "down")  button = kElement_DownButton;
	else if (name == "app1")  button = kElement_App1Button;
	else if (name == "app2")  button = kElement_App2Button;
	else if (name == "app3")  button = kElement_App3Button;
	else if (name == "app4")  button = kElement_App4Button;
	else if (name == "cradle") button = kElement_CradleButton;
	else if (name == "contrast") button = kElement_ContrastButton;
	else { SendErr ("usage", "unknown button '" + name.toStdString () + "'"); return; }

	if (action != "down" && action != "up" && action != "tap")
	{
		SendErr ("usage", "button <name> <down|up|tap>");
		return;
	}

	QueueWork ([button, action]() {
		if (!gSession) return;
		if (action == "down")      gSession->SetButtonDown (button);
		else if (action == "up")   gSession->SetButtonUp (button);
		else if (action == "tap")  gSession->SetButtonTap (button);
	});
}

void ReControlSession::CmdReset (const QStringList& args)
{
	if (!gSession) { SendErr ("transient", "no session"); return; }

	EmResetType type = kResetSoft;
	if (args.size () > 1)
	{
		QString t = args[1].toLower ();
		if (t == "hard")  type = kResetHard;
		else if (t == "debug") type = kResetDebug;
		else if (t != "soft") { SendErr ("usage", "reset [soft|hard|debug]"); return; }
	}

	gSession->ScheduleReset (type);
	Send ("OK\n");
}

void ReControlSession::CmdSleep (const QStringList& args)
{
	if (args.size () != 2) { SendErr ("usage", "sleep <ms>"); return; }
	int ms = args[1].toInt ();
	if (ms < 1 || ms > 30000) { SendErr ("usage", "sleep <1-30000>"); return; }

	fProcessingPaused = true;
	QTimer::singleShot (ms, this, &ReControlSession::OnSleepDone);
	// Don't send response yet — OnSleepDone will send it
}

void ReControlSession::OnSleepDone ()
{
	Send ("OK\n");
	fProcessingPaused = false;
	ProcessBufferedCommands ();
}

// ---------------------------------------------------------------------------
// Helper: compute CRC32 over screen pixel data (RGB, row by row)
// ---------------------------------------------------------------------------

static std::string ComputeScreenHash (EmScreenUpdateInfo& info, int w, int h)
{
	const uint8_t* src = (const uint8_t*) info.fImage.GetBits ();
	EmPixMapRowBytes srcRowBytes = info.fImage.GetRowBytes ();

	uLong crc = ::crc32 (0L, Z_NULL, 0);
	for (int y = 0; y < h; y++)
		crc = ::crc32 (crc, src + y * srcRowBytes, w * 3);

	char hex[16];
	snprintf (hex, sizeof (hex), "%08lx", (unsigned long) crc);
	return std::string (hex);
}

void ReControlSession::CmdScreenshot (const QStringList& args)
{
	if (args.size () != 2) { SendErr ("usage", "screenshot <filepath>"); return; }
	if (!gSession) { SendErr ("transient", "no session"); return; }

	QString path = args[1];

	QueueWorkResult ([path]() -> std::string {
		if (!gSession) return "ERR transient: no session\n";
		EmSessionStopper stopper (gSession, kStopNow);
		if (!stopper.Stopped ()) return "ERR transient: could not stop session\n";

		EmScreen::InvalidateAll ();

		EmScreenUpdateInfo info;
		info.fScreenLow  = 0;
		info.fScreenHigh = 0xFFFFFFFF;
		if (!EmScreen::GetBits (info)) return "ERR transient: could not capture screen\n";

		EmPoint size = info.fImage.GetSize ();
		int w = size.fX;
		int h = size.fY;

		info.fImage.ConvertToFormat (kPixMapFormat24RGB);

		std::string hash = ComputeScreenHash (info, w, h);

		QImage img (w, h, QImage::Format_RGB888);
		const uint8_t* src = (const uint8_t*) info.fImage.GetBits ();
		EmPixMapRowBytes srcRowBytes = info.fImage.GetRowBytes ();
		for (int y = 0; y < h; y++)
		{
			memcpy (img.scanLine (y), src + y * srcRowBytes, w * 3);
		}

		if (!img.save (path, "PNG"))
			return "ERR transient: could not write " + path.toStdString () + "\n";

		return "OK " + hash + " " + std::to_string (w) + " " + std::to_string (h) + "\n";
	});
}

void ReControlSession::CmdInstall (const QStringList& args)
{
	if (args.size () != 2) { SendErr ("usage", "install <filepath>"); return; }
	if (!gSession) { SendErr ("transient", "no session"); return; }

	QString path = args[1];

	// Validate file exists
	QFileInfo fileInfo (path);
	if (!fileInfo.exists ())
	{
		SendErr ("usage", "file not found: " + path.toStdString ());
		return;
	}

	// Check file is not empty
	qint64 fileSize = fileInfo.size ();
	if (fileSize == 0)
	{
		SendErr ("usage", "file is empty");
		return;
	}

	// Check file is not too large (4MB limit)
	if (fileSize > 4 * 1024 * 1024)
	{
		SendErr ("usage", "file is too large (max 4MB)");
		return;
	}

	std::string pathStr = path.toStdString ();

	QueueWorkResult ([pathStr]() -> std::string {
		if (!gSession) return "ERR transient: no session\n";
		EmSessionStopper stopper (gSession, kStopOnSysCall, 5000);
		if (!stopper.Stopped ())
			return "ERR timeout: CPU did not reach syscall boundary within 5000ms\n";

		try
		{
			EmStreamFile stream (EmFileRef (pathStr), kOpenExistingForRead);
			EmFileImport importer (stream, kMethodBest);

			while (!importer.Done ())
			{
				ErrCode err = importer.Continue ();
				if (err != errNone)
					return "ERR fatal: install failed - EmFileImport returned error\n";
			}

			return "OK\n";
		}
		catch (...)
		{
			return "ERR fatal: install failed - emulator exception during ROM call (recommend reset)\n";
		}
	});
}

void ReControlSession::CmdLaunch (const QStringList& args)
{
	if (args.size () < 2) { SendErr ("usage", "launch <dbname>"); return; }
	if (!gSession) { SendErr ("transient", "no session"); return; }

	// Rejoin args after "launch" to support database names with spaces
	QString nameQ;
	for (int i = 1; i < args.size (); i++)
	{
		if (i > 1) nameQ += ' ';
		nameQ += args[i];
	}
	std::string name = nameQ.toStdString ();

	QueueWorkResult ([name]() -> std::string {
		if (!gSession) return "ERR transient: no session\n";
		EmSessionStopper stopper (gSession, kStopOnSysCall, 5000);
		if (!stopper.Stopped () || !stopper.CanCall ())
			return "ERR timeout: CPU did not reach syscall boundary within 5000ms\n";

		try
		{
			LocalID dbID = DmFindDatabase (0, name.c_str ());
			if (dbID == 0)
				return "ERR usage: database not found: " + name + "\n";

			Err err = SysUIAppSwitch (0, dbID, sysAppLaunchCmdNormalLaunch, NULL);
			if (err != errNone)
				return "ERR fatal: SysUIAppSwitch failed\n";

			return "OK\n";
		}
		catch (...)
		{
			return "ERR fatal: launch failed - emulator exception during ROM call (recommend reset)\n";
		}
	});
}

void ReControlSession::CmdSave (const QStringList& args)
{
	if (args.size () != 2) { SendErr ("usage", "save <filepath>"); return; }
	if (!gSession) { SendErr ("transient", "no session"); return; }

	std::string pathStr = args[1].toStdString ();

	QueueWorkResult ([pathStr]() -> std::string {
		if (!gSession) return "ERR transient: no session\n";
		EmSessionStopper stopper (gSession, kStopNow);
		if (!stopper.Stopped ()) return "ERR transient: could not stop session\n";

		EmFileRef ref (pathStr);
		gSession->Save (ref, true);

		return "OK\n";
	});
}

void ReControlSession::CmdLoad (const QStringList& args)
{
	if (args.size () != 2) { SendErr ("usage", "load <filepath>"); return; }
	if (!gDocument) { SendErr ("transient", "no document"); return; }

	QString path = args[1];
	QFileInfo fi (path);
	if (!fi.exists () || !fi.isFile ())
	{
		SendErr ("usage", "file not found: " + path.toStdString ());
		return;
	}

	// Load must run entirely on the main thread because it destroys
	// the current session/document and creates new ones (involving
	// Qt window operations and CPU thread management).
	//
	// Defer via QTimer::singleShot to avoid re-entrancy issues with
	// the socket read handler.
	//
	// Use QPointer instead of raw pointer — the socket could
	// disconnect (destroying this ReControlSession) before the
	// deferred lambda fires, and a raw pointer would dangle.
	QPointer<ReControlSession> safeRef (this);
	std::string pathStr = path.toStdString ();

	QTimer::singleShot (0, qApp, [safeRef, pathStr]() {
		// Helper: send a message only if the session still exists.
		auto safeSend = [&safeRef](const std::string& msg) {
			if (safeRef)
				safeRef->Send (msg);
		};

		try
		{
			// Shut down the CPU worker thread before destroying the session.
			// Commands in the queue reference gSession which is about to die.
			if (gCPUWorker)
			{
				gCPUWorker->shutdown ();
				delete gCPUWorker;
				gCPUWorker = nullptr;
			}

			// Close the current document without prompting to save.
			// HandleClose(kSaveNever, false) calls `delete this` on
			// the document, which destroys the EmSession and CPU thread.
			if (gDocument)
			{
				gDocument->HandleClose (kSaveNever, false);
			}

			if (gDocument != NULL)
			{
				safeSend ("ERR transient: could not close current session\n");
				return;
			}

			// Open the new session (creates EmDocument, EmSession, CPU thread)
			EmFileRef ref (pathStr);
			EmDocument::DoOpen (ref);

			// Restart the CPU worker thread for the new session
			gCPUWorker = new CPUWorkerThread ();
			gCPUWorker->start ();

			if (gDocument && gSession)
			{
				safeSend ("OK\n");
			}
			else
			{
				safeSend ("ERR fatal: failed to open session\n");
			}
		}
		catch (ErrCode errCode)
		{
			// Ensure worker thread is restarted even on error
			if (!gCPUWorker && gSession)
			{
				gCPUWorker = new CPUWorkerThread ();
				gCPUWorker->start ();
			}
			safeSend ("ERR fatal: load failed (error " + std::to_string (errCode) + ")\n");
		}
		catch (...)
		{
			if (!gCPUWorker && gSession)
			{
				gCPUWorker = new CPUWorkerThread ();
				gCPUWorker->start ();
			}
			safeSend ("ERR fatal: load failed (unknown exception)\n");
		}
	});
}

void ReControlSession::CmdInfo (const QStringList& args)
{
	if (!gSession) {
		Send ("OK POSE64 " + std::string (qApp->applicationVersion ().toStdString ()) + "\n");
		Send (".\n");
		return;
	}

	// Gather non-CPU-dependent info on main thread first
	std::string version = qApp->applicationVersion ().toStdString ();
	Configuration cfg = gSession->GetConfiguration ();
	std::string deviceId = cfg.fDevice.GetIDString ();
	int ramSizeMB = cfg.fRAMSize / (1024 * 1024);
	std::string romName;
	if (cfg.fROMFile.IsSpecified ())
		romName = cfg.fROMFile.GetName ();
	std::string sessionPath;
	if (gDocument)
	{
		EmFileRef ref = gDocument->GetFileRef ();
		if (ref.IsSpecified ())
			sessionPath = ref.GetFullPath ();
	}

	QueueWorkResult ([version, deviceId, ramSizeMB, romName, sessionPath]() -> std::string {
		if (!gSession) return "ERR transient: no session\n";
		std::string out = "OK POSE64 " + version + "\n";
		out += " device=" + deviceId + "\n";
		if (ramSizeMB > 0)
			out += " ram=" + std::to_string (ramSizeMB) + "MB\n";
		if (!romName.empty ())
			out += " rom=" + romName + "\n";

		EmSessionStopper stopper (gSession, kStopNow);
		if (stopper.Stopped ())
		{
			EmScreen::InvalidateAll ();

			EmScreenUpdateInfo info;
			info.fScreenLow  = 0;
			info.fScreenHigh = 0xFFFFFFFF;
			if (EmScreen::GetBits (info))
			{
				EmPoint size = info.fImage.GetSize ();
				out += " screen=" + std::to_string (size.fX) + "x" + std::to_string (size.fY) + "\n";
			}
		}

		if (!sessionPath.empty ())
			out += " session=" + sessionPath + "\n";
		out += ".\n";
		return out;
	});
}

void ReControlSession::CmdUI (const QStringList& args)
{
	if (!gSession) { SendErr ("transient", "no session"); return; }

	QueueWorkResult ([]() -> std::string {
		if (!gSession) return "ERR transient: no session\n";
		EmSessionStopper stopper (gSession, kStopOnCycle);
		if (!stopper.Stopped ()) return "ERR transient: could not stop session\n";

		CEnableFullAccess munge;
		return PalmFormReader_ReadActiveForm ();
	});
}

void ReControlSession::CmdScreenHash (const QStringList& args)
{
	if (!gSession) { SendErr ("transient", "no session"); return; }

	QueueWorkResult ([]() -> std::string {
		if (!gSession) return "ERR transient: no session\n";
		EmSessionStopper stopper (gSession, kStopNow);
		if (!stopper.Stopped ()) return "ERR transient: could not stop session\n";

		EmScreen::InvalidateAll ();

		EmScreenUpdateInfo info;
		info.fScreenLow  = 0;
		info.fScreenHigh = 0xFFFFFFFF;
		if (!EmScreen::GetBits (info)) return "ERR transient: could not capture screen\n";

		EmPoint size = info.fImage.GetSize ();
		int w = size.fX;
		int h = size.fY;

		info.fImage.ConvertToFormat (kPixMapFormat24RGB);

		std::string hash = ComputeScreenHash (info, w, h);
		return "OK " + hash + " " + std::to_string (w) + " " + std::to_string (h) + "\n";
	});
}

void ReControlSession::CmdType (const QStringList& args)
{
	if (args.size () < 2) { SendErr ("usage", "type <text>"); return; }
	if (!gSession) { SendErr ("transient", "no session"); return; }

	// Rejoin args after "type" to preserve spaces, then convert to Latin-1
	QString text;
	for (int i = 1; i < args.size (); i++)
	{
		if (i > 1) text += ' ';
		text += args[i];
	}
	QByteArray latin1 = text.toLatin1 ();

	QueueWork ([latin1]() {
		if (!gSession) return;
		for (int i = 0; i < latin1.size (); i++)
		{
			unsigned char ch = (unsigned char) latin1[i];
			EmKeyEvent keyEvent (ch);
			gSession->PostKeyEvent (keyEvent);
		}
	});
}

void ReControlSession::CmdTapId (const QStringList& args)
{
	if (args.size () != 2) { SendErr ("usage", "tap-id <object_id>"); return; }
	if (!gSession) { SendErr ("transient", "no session"); return; }

	int targetId = args[1].toInt ();

	QueueWorkResult ([targetId]() -> std::string {
		if (!gSession) return "ERR transient: no session\n";
		EmSessionStopper stopper (gSession, kStopOnCycle);
		if (!stopper.Stopped ()) return "ERR transient: could not stop session\n";

		CEnableFullAccess munge;

		// Get active form pointer
		emuptr formPtr = EmLowMem_GetGlobal (uiGlobalsCommon.currentForm);
		if (formPtr == 0) return "ERR transient: no active form\n";

		// Read the form's window origin (popup dialogs have non-zero origin)
		int16 winX = (int16) EmMemGet16 (formPtr + kWindowType_windowBounds_topLeft_x);
		int16 winY = (int16) EmMemGet16 (formPtr + kWindowType_windowBounds_topLeft_y);

		uint16 numObjects = EmMemGet16 (formPtr + kFormType_numObjects);
		emuptr objectsPtr = EmMemGet32 (formPtr + kFormType_objects);
		if (objectsPtr == 0 || numObjects == 0)
			return "ERR transient: form has no objects\n";

		for (int i = 0; i < numObjects; i++)
		{
			emuptr objEntry = objectsPtr + i * kFormObjListType_size;
			uint8 objType = EmMemGet8 (objEntry + kFormObjListType_objectType);
			emuptr dataPtr = EmMemGet32 (objEntry + kFormObjListType_object);
			if (dataPtr == 0) continue;

			// Read the object's ID and bounds based on type
			uint16 objId = 0;
			int16 bx = 0, by = 0, bw = 0, bh = 0;

			switch (objType)
			{
				case kFrmControlObj:
					objId = EmMemGet16 (dataPtr + kControlType_id);
					bx = (int16) EmMemGet16 (dataPtr + kControlType_bounds_topLeft_x);
					by = (int16) EmMemGet16 (dataPtr + kControlType_bounds_topLeft_y);
					bw = (int16) EmMemGet16 (dataPtr + kControlType_bounds_extent_x);
					bh = (int16) EmMemGet16 (dataPtr + kControlType_bounds_extent_y);
					break;

				case kFrmFieldObj:
					objId = EmMemGet16 (dataPtr + kFieldType_id);
					bx = (int16) EmMemGet16 (dataPtr + kFieldType_rect_topLeft_x);
					by = (int16) EmMemGet16 (dataPtr + kFieldType_rect_topLeft_y);
					bw = (int16) EmMemGet16 (dataPtr + kFieldType_rect_extent_x);
					bh = (int16) EmMemGet16 (dataPtr + kFieldType_rect_extent_y);
					break;

				case kFrmListObj:
					objId = EmMemGet16 (dataPtr + kListType_id);
					bx = (int16) EmMemGet16 (dataPtr + kListType_bounds_topLeft_x);
					by = (int16) EmMemGet16 (dataPtr + kListType_bounds_topLeft_y);
					bw = (int16) EmMemGet16 (dataPtr + kListType_bounds_extent_x);
					bh = (int16) EmMemGet16 (dataPtr + kListType_bounds_extent_y);
					break;

				case kFrmLabelObj:
					objId = EmMemGet16 (dataPtr + 0);  // label: id at +0
					bx = (int16) EmMemGet16 (dataPtr + 2);
					by = (int16) EmMemGet16 (dataPtr + 4);
					bw = 0;
					bh = 0;
					break;

				case kFrmGadgetObj:
					objId = EmMemGet16 (dataPtr + 0);  // gadget: id at +0
					bx = (int16) EmMemGet16 (dataPtr + 2);
					by = (int16) EmMemGet16 (dataPtr + 4);
					bw = (int16) EmMemGet16 (dataPtr + 6);
					bh = (int16) EmMemGet16 (dataPtr + 8);
					break;

				case kFrmScrollBarObj:
					objId = EmMemGet16 (dataPtr + kScrollBarType_id);
					bx = (int16) EmMemGet16 (dataPtr + kScrollBarType_bounds_topLeft_x);
					by = (int16) EmMemGet16 (dataPtr + kScrollBarType_bounds_topLeft_y);
					bw = (int16) EmMemGet16 (dataPtr + kScrollBarType_bounds_extent_x);
					bh = (int16) EmMemGet16 (dataPtr + kScrollBarType_bounds_extent_y);
					break;

				default:
					continue;  // skip non-tappable types
			}

			if ((int) objId != targetId) continue;

			// Compute center of the object in screen coordinates
			int cx = winX + bx + bw / 2;
			int cy = winY + by + bh / 2;

			// Post pen-down at center, pen-up to release
			EmPenEvent penDown (EmPoint (cx, cy), true);
			gSession->PostPenEvent (penDown);
			EmPenEvent penUp (EmPoint (-1, -1), false);
			gSession->PostPenEvent (penUp);

			return "OK " + std::to_string (cx) + " " + std::to_string (cy) + "\n";
		}

		return "ERR usage: object " + std::to_string (targetId) + " not found\n";
	});
}

void ReControlSession::CmdApps (const QStringList& args)
{
	if (!gSession) { SendErr ("transient", "no session"); return; }

	QueueWorkResult ([]() -> std::string {
		if (!gSession) return "ERR transient: no session\n";
		EmSessionStopper stopper (gSession, kStopOnSysCall, 5000);
		if (!stopper.Stopped () || !stopper.CanCall ())
			return "ERR timeout: CPU did not reach syscall boundary within 5000ms\n";

		try
		{
			std::string out = "OK\n";
			UInt16 numDBs = DmNumDatabases (0);

			for (UInt16 i = 0; i < numDBs; i++)
			{
				LocalID dbID = DmGetDatabase (0, i);
				if (dbID == 0) continue;

				Char name[dmDBNameLength];
				UInt32 type = 0;
				UInt32 creator = 0;

				Err err = DmDatabaseInfo (0, dbID, name,
					NULL, NULL, NULL, NULL, NULL, NULL,
					NULL, NULL, &type, &creator);
				if (err != errNone) continue;

				// Only list applications
				if (type != sysFileTApplication) continue;

				// Format creator as 4-char code
				char typeStr[5], creatorStr[5];
				typeStr[0] = (char) ((type >> 24) & 0xFF);
				typeStr[1] = (char) ((type >> 16) & 0xFF);
				typeStr[2] = (char) ((type >>  8) & 0xFF);
				typeStr[3] = (char) ((type      ) & 0xFF);
				typeStr[4] = '\0';
				creatorStr[0] = (char) ((creator >> 24) & 0xFF);
				creatorStr[1] = (char) ((creator >> 16) & 0xFF);
				creatorStr[2] = (char) ((creator >>  8) & 0xFF);
				creatorStr[3] = (char) ((creator      ) & 0xFF);
				creatorStr[4] = '\0';

				out += " ";
				out += name;
				out += " type=";
				out += typeStr;
				out += " creator=";
				out += creatorStr;
				out += "\n";
			}

			out += ".\n";
			return out;
		}
		catch (...)
		{
			return "ERR fatal: apps enumeration failed - emulator exception during ROM call\n";
		}
	});
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

void ReControlSession::DispatchCommand (const QStringList& parts)
{
	QString cmd = parts[0].toLower ();

	if (cmd == "state")            CmdState (parts);
	else if (cmd == "quit")        CmdQuit (parts);
	else if (cmd == "tap")         CmdTap (parts);
	else if (cmd == "tap-id")      CmdTapId (parts);
	else if (cmd == "pen")         CmdPen (parts);
	else if (cmd == "key")         CmdKey (parts);
	else if (cmd == "type")        CmdType (parts);
	else if (cmd == "button")      CmdButton (parts);
	else if (cmd == "reset")       CmdReset (parts);
	else if (cmd == "screenshot")  CmdScreenshot (parts);
	else if (cmd == "screen-hash") CmdScreenHash (parts);
	else if (cmd == "sleep")       CmdSleep (parts);
	else if (cmd == "install")     CmdInstall (parts);
	else if (cmd == "launch")      CmdLaunch (parts);
	else if (cmd == "save")        CmdSave (parts);
	else if (cmd == "load")        CmdLoad (parts);
	else if (cmd == "info")        CmdInfo (parts);
	else if (cmd == "ui")          CmdUI (parts);
	else if (cmd == "apps")        CmdApps (parts);
	else
		SendErr ("usage", "unknown command '" + cmd.toStdString () + "'");
}

void ReControlSession::OnReadyRead ()
{
	// Read available data
	QByteArray data = fSocket->readAll ();
	fReadBuffer.append (data);

	// Process complete lines
	while (true)
	{
		int newlinePos = fReadBuffer.indexOf ('\n');
		if (newlinePos < 0)
			break;

		// Extract line (without newline)
		QString line = QString::fromUtf8 (fReadBuffer.left (newlinePos)).trimmed ();
		fReadBuffer.remove (0, newlinePos + 1);

		// Skip empty lines
		if (line.isEmpty ())
			continue;

		// If processing is paused (sleep in progress), buffer the command
		if (fProcessingPaused)
		{
			fCommandBuffer.append (line);
			continue;
		}

		// Parse command and arguments
		QStringList parts = line.split (' ', Qt::SkipEmptyParts);
		DispatchCommand (parts);
	}
}

void ReControlSession::OnDisconnected ()
{
	// Notify server that this session is gone
	if (fServer)
		fServer->NotifySessionGone (this);

	// Schedule self for deletion
	deleteLater ();
}

// ============================================================================
// ReControlServer - Implementation
// ============================================================================

ReControlServer::ReControlServer (int port, QObject* parent)
	: QObject (parent),
	  fServer (nullptr),
	  fActiveSession (nullptr)
{
	if (port <= 0)
		return;

	// Create QTcpServer
	fServer = new QTcpServer (this);

	connect (fServer, &QTcpServer::newConnection,
			 this, &ReControlServer::OnNewConnection);

	// Listen on localhost on the given port
	if (!fServer->listen (QHostAddress::LocalHost, port))
	{
		// Log error but don't crash
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
		// Will be deleted as child
	}
	// fActiveSession will be deleted as child when it disconnects
}

void ReControlServer::OnNewConnection ()
{
	// Accept the incoming connection
	QTcpSocket* socket = fServer->nextPendingConnection ();
	if (!socket)
		return;

	// If we already have an active session, reject this one
	if (fActiveSession)
	{
		// Send busy response and close
		socket->write ("ERR busy\n");
		socket->flush ();
		socket->disconnectFromHost ();
		socket->deleteLater ();
		return;
	}

	// Create a new session for this socket
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

	// Create the server (will be leaked, but that's OK for a singleton)
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

#include "ReControl.moc"
