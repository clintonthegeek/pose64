/* -*- mode: C++; tab-width: 4 -*- */
/* ReControl: session command handlers (state, quit, reset, sleep, install, export,
 * launch, save, load, info, apps, dialog, delete). */

#include "EmCommon.h"

#include "PalmMacroUndefs.h"	// Phase 5: daysInYear/monthsInYear undef

#include "ReControl.h"
#include "EmSession.h"
#include "EmApplication.h"
#include "PreferenceMgr.h"
#include "EmTransportSerial.h"	// EmTransportSerial, GetPtySlaveName (Phase 4)
#include "EmDocument.h"
#include "EmFileImport.h"
#include "EmStreamFile.h"
#include "EmScreen.h"
#include "EmLowMem.h"
#include "EmTypes.h"
#include "ROMStubs.h"
#include "Patches/EmPatchState.h"
#include "CPUWorkerThread.h"
#include "LoadApplication.h"
#include "PalmFormReader.h"
#include "UAE.h"

#include <QApplication>
#include <QFileInfo>
#include <QTimer>
#include <QPointer>

#include <string>

// From EmDlgQt.cpp — remote dialog handling
extern std::string EmDlgQt_GetPendingDialog (void);
extern bool EmDlgQt_RespondToDialog (const std::string& buttonName);
extern void EmDlgQt_DismissIfPending (void);

// From DebugMgr.h — watchpoint state (clear before session teardown)
#include "DebugMgr.h"

// ============================================================================
// RcCmd_State — Immediate (no stopper, handles null gSession)
// ============================================================================

std::string RcCmd_State (const QStringList& args)
{
	if (!gSession)
		return "OK stopped\n";

	EmSessionState state = gSession->GetSessionState ();
	switch (state)
	{
		case kRunning:
			return "OK running\n";
		case kSuspended:
		{
			EmSuspendState suspendState = gSession->GetSuspendState ();
			EmSuspendCounters counters = suspendState.fCounters;

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
			return std::string (buffer);
		}
		case kStopped:
			return "OK stopped\n";
		case kBlockedOnUI:
			return "OK blocked_on_ui\n";
	}
	return "OK unknown\n";
}

// ============================================================================
// RcCmd_Quit — Immediate
// ============================================================================

std::string RcCmd_Quit (const QStringList& args)
{
	gApplication->SetTimeToQuit (true);
	return "OK\n";
}

// ============================================================================
// RcCmd_Reset — Custom (needs special dialog dismiss logic)
// ============================================================================

void RcCmd_Reset (ReControlSession* session, const QStringList& args)
{
	if (!gSession) { session->SendErr ("transient", "no session"); return; }

	EmResetType type = kResetSoft;
	if (args.size () > 1)
	{
		QString t = args[1].toLower ();
		if (t == "hard")  type = kResetHard;
		else if (t == "debug") type = kResetDebug;
		else if (t != "soft") { session->SendErr ("usage", "reset [soft|hard|debug]"); return; }
	}

	bool wasBlocked = (gSession->GetSessionState () == kBlockedOnUI);

	if (wasBlocked)
		EmDlgQt_DismissIfPending ();

	gSession->ForceReset (type);

	if (wasBlocked)
		session->Send ("OK reset (was blocked_on_ui, dialog dismissed)\n");
	else
		session->Send ("OK\n");
}

// ============================================================================
// RcCmd_Sleep — Custom (pauses command processing via QTimer)
// ============================================================================

void RcCmd_Sleep (ReControlSession* session, const QStringList& args)
{
	if (args.size () != 2) { session->SendErr ("usage", "sleep <ms>"); return; }
	int ms = args[1].toInt ();
	if (ms < 1 || ms > 30000) { session->SendErr ("usage", "sleep <1-30000>"); return; }
	session->PauseProcessing (ms);
}

// ============================================================================
// RcCmd_Install — WorkerRaw (dynamic timeout based on file size)
// ============================================================================

std::string RcCmd_Install (const QStringList& args)
{
	if (args.size () != 2)
		return "ERR usage: install <filepath>\n";

	QString path = args[1];

	QFileInfo fileInfo (path);
	if (!fileInfo.exists ())
		return "ERR usage: file not found: " + path.toStdString () + "\n";

	qint64 fileSize = fileInfo.size ();
	if (fileSize == 0)
		return "ERR usage: file is empty\n";
	if (fileSize > 4 * 1024 * 1024)
		return "ERR usage: file is too large (max 4MB)\n";

	std::string pathStr = path.toStdString ();
	int timeoutMs = 5000 + (int) (fileSize / 100);

	EmSessionStopper stopper (gSession, kStopOnSysCall, timeoutMs);
	if (!stopper.Stopped ())
	{
		char msg[512];
		snprintf (msg, sizeof (msg),
			"ERR timeout: CPU did not reach syscall boundary within %dms. "
			"File: %s (%lldKB). "
			"Recovery: palm_reset type=soft, then retry install.\n",
			timeoutMs, pathStr.c_str (), (long long) fileSize / 1024);
		return msg;
	}

	try
	{
		EmStreamFile stream (EmFileRef (pathStr), kOpenExistingForRead);
		EmFileImport importer (stream, kMethodBest);

		while (!importer.Done ())
		{
			ErrCode err = importer.Continue ();
			if (err != errNone)
			{
				char msg[512];
				snprintf (msg, sizeof (msg),
					"ERR fatal: install failed - ROM import error (code %ld). "
					"File: %s (%lldKB). "
					"Recovery: palm_reset type=hard recommended before retry.\n",
					(long) err, pathStr.c_str (), (long long) fileSize / 1024);
				return std::string (msg);
			}
		}

		return "OK\n";
	}
	catch (...)
	{
		char msg[512];
		snprintf (msg, sizeof (msg),
			"ERR fatal: install failed - emulator exception during ROM call. "
			"File: %s (%lldKB). Session may be corrupted. "
			"Recovery: palm_reset type=hard recommended before retry.\n",
			pathStr.c_str (), (long long) fileSize / 1024);
		return std::string (msg);
	}
}

// ============================================================================
// RcCmd_Export — WorkerSysCall (stopper created by dispatch loop)
// ============================================================================

std::string RcCmd_Export (const QStringList& args)
{
	if (args.size () < 3)
		return "ERR usage: export <dbname> <filepath>\n";

	QString path = args.last ();
	QString nameQ;
	for (int i = 1; i < args.size () - 1; i++)
	{
		if (i > 1) nameQ += ' ';
		nameQ += args[i];
	}
	std::string name = nameQ.toStdString ();
	std::string pathStr = path.toStdString ();

	try
	{
		LocalID dbID = DmFindDatabase (0, name.c_str ());
		if (dbID == 0)
			return "ERR usage: database not found: " + name + "\n";

		EmStreamFile stream (EmFileRef (pathStr), kCreateOrEraseForWrite);
		SavePalmFile (stream, 0, name.c_str ());

		return "OK\n";
	}
	catch (...)
	{
		return "ERR fatal: export failed - emulator exception during ROM call (recommend reset)\n";
	}
}

// ============================================================================
// RcCmd_Launch — WorkerSysCall (stopper created by dispatch loop)
// Fixes main-thread deadlock: now runs on worker thread.
// ============================================================================

std::string RcCmd_Launch (const QStringList& args)
{
	if (args.size () < 2)
		return "ERR usage: launch <dbname>\n";

	QString nameQ;
	for (int i = 1; i < args.size (); i++)
	{
		if (i > 1) nameQ += ' ';
		nameQ += args[i];
	}
	std::string name = nameQ.toStdString ();

	try
	{
		LocalID dbID = DmFindDatabase (0, name.c_str ());
		if (dbID == 0)
			return "ERR usage: database not found: " + name + "\n";

		// Use PuppetString's app-switch mechanism.
		EmPatchState::SetSwitchApp (0, dbID);

		// EvtWakeup signals the event group at the kernel level.
		// PuppetString fires on the next natural SysEvGroupWait cycle.
		EvtWakeup ();

		return "OK\n";
	}
	catch (...)
	{
		return "ERR fatal: launch failed - emulator exception during ROM call (recommend reset)\n";
	}
}

// ============================================================================
// RcCmd_Save — WorkerRaw (creates own kStopNow stopper)
// ============================================================================

std::string RcCmd_Save (const QStringList& args)
{
	if (args.size () != 2)
		return "ERR usage: save <filepath>\n";

	std::string pathStr = args[1].toStdString ();

	EmSessionStopper stopper (gSession, kStopNow);
	if (!stopper.Stopped ())
		return "ERR transient: could not stop session\n";

	EmFileRef ref (pathStr);
	gSession->Save (ref, true);

	return "OK\n";
}

// ============================================================================
// RcCmd_Load — Custom (tears down and rebuilds session, deferred execution)
// ============================================================================

void RcCmd_Load (ReControlSession* session, const QStringList& args)
{
	if (args.size () != 2) { session->SendErr ("usage", "load <filepath>"); return; }

	QString path = args[1];
	QFileInfo fi (path);
	if (!fi.exists () || !fi.isFile ())
	{
		session->SendErr ("usage", "file not found: " + path.toStdString ());
		return;
	}

	QPointer<ReControlSession> safeRef (session);
	std::string pathStr = path.toStdString ();

	// doTeardown: executes the actual session teardown + rebuild.
	auto doTeardown = [safeRef, pathStr]() {
		auto safeSend = [&safeRef](const std::string& msg) {
			if (safeRef)
				safeRef->Send (msg);
		};

		bool wasQuitOnLastWindow = qApp->quitOnLastWindowClosed ();
		qApp->setQuitOnLastWindowClosed (false);

		try
		{
			if (gCPUWorker)
			{
				gCPUWorker->shutdown ();
				delete gCPUWorker;
				gCPUWorker = nullptr;
			}

			if (gDocument)
			{
				gDocument->HandleClose (kSaveNever, false);
			}

			if (gDocument != NULL)
			{
				qApp->setQuitOnLastWindowClosed (wasQuitOnLastWindow);
				safeSend ("ERR transient: could not close current session\n");
				return;
			}

			EmFileRef ref (pathStr);
			EmDocument::DoOpen (ref);

			qApp->setQuitOnLastWindowClosed (wasQuitOnLastWindow);

			gCPUWorker = new CPUWorkerThread ();
			gCPUWorker->start ();

			if (gDocument && gSession)
				safeSend ("OK\n");
			else
				safeSend ("ERR fatal: failed to open session\n");
		}
		catch (ErrCode errCode)
		{
			qApp->setQuitOnLastWindowClosed (wasQuitOnLastWindow);
			if (!gCPUWorker && gSession)
			{
				gCPUWorker = new CPUWorkerThread ();
				gCPUWorker->start ();
			}
			safeSend ("ERR fatal: load failed (error " + std::to_string (errCode) + ")\n");
		}
		catch (...)
		{
			qApp->setQuitOnLastWindowClosed (wasQuitOnLastWindow);
			if (!gCPUWorker && gSession)
			{
				gCPUWorker = new CPUWorkerThread ();
				gCPUWorker->start ();
			}
			safeSend ("ERR fatal: load failed (unknown exception)\n");
		}
	};

	// The timer decouples teardown from the socket handler's call stack.
	// If the session is blocked_on_ui when it fires, REFUSE instead of
	// loading (GATE 3 Gap 3, 2026-06-12).  The old dismiss-and-defer path
	// assumed dismissing the dialog left the CPU running — true when only
	// watchpoints raised dialogs and watchEnabled was cleared first, but
	// false since Phase 3b: breakpoint and crash dialogs re-raise on
	// hot/faulting PCs, the CPU re-blocks before the deferred teardown
	// runs, and HandleClose then waits forever on a CPU thread parked on a
	// dialog this (stuck) thread can never service.
	QTimer::singleShot (0, qApp, [safeRef, pathStr, doTeardown = std::move (doTeardown)]() mutable {
		if (gSession && gSession->GetSessionState () == kBlockedOnUI)
		{
			if (safeRef)
				safeRef->SendErr ("blocked",
					"dismiss dialog first with 'dialog respond' "
					"(if it re-raises, 'break clearall' works while blocked)");
			return;
		}

		// No dialog running: safe to tear down immediately.
		gDebuggerGlobals.watchEnabled = false;
		doTeardown ();
	});
}

// ============================================================================
// RcCmd_Info — Custom (null-session returns version only, otherwise WorkerRaw)
// ============================================================================

void RcCmd_Info (ReControlSession* session, const QStringList& args)
{
	if (!gSession)
	{
		session->Send ("OK POSE64 " + std::string (qApp->applicationVersion ().toStdString ()) + "\n");
		session->Send (".\n");
		return;
	}

	// Gather non-CPU-dependent info on main thread
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

	// Serial transport: configured descriptor + live PTY slave path
	// (Phase 4 — lets hosts script HotSync end-to-end; the PTY persists
	// for the process once the guest first opens the port).
	std::string serialInfo;
	{
		Preference<EmTransportDescriptor> pref (kPrefKeyPortSerial);
		EmTransportType type = pref->GetType ();
		if (type != kTransportNull && type != kTransportUnknown)
		{
			serialInfo = pref->GetDescriptor ();
			EmTransportSerial* serial = dynamic_cast<EmTransportSerial*> (
				gEmuPrefs->GetTransportForDevice (kUARTSerial));
			if (serial)
			{
				std::string pty = serial->GetPtySlaveName ();
				if (!pty.empty ())
					serialInfo += " pty=" + pty;
			}
		}
	}

	session->QueueWorkResult ([version, deviceId, ramSizeMB, romName, sessionPath, serialInfo]() -> std::string {
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

		// OS version from Palm ROM
		UInt32 osVer = EmPatchState::OSMajorMinorVersion ();
		if (osVer > 0)
			out += " os=" + std::to_string (osVer / 10) + "." + std::to_string (osVer % 10) + "\n";

		// Current app name
		EmuAppInfo appInfo = EmPatchState::GetCurrentAppInfo ();
		if (appInfo.fName[0] != '\0')
			out += " app=" + std::string (appInfo.fName) + "\n";

		// Active form ID
		{
			CEnableFullAccess munge;
			emuptr formPtr = EmLowMem_GetGlobal (uiGlobalsCommon.currentForm);
			if (formPtr != 0)
			{
				uint16 formId = EmMemGet16 (formPtr + kFormType_formId);
				out += " form=" + std::to_string (formId) + "\n";
			}
		}

		if (!sessionPath.empty ())
			out += " session=" + sessionPath + "\n";
		if (!serialInfo.empty ())
			out += " serial=" + serialInfo + "\n";
		out += ".\n";
		return out;
	});
}

// ============================================================================
// RcCmd_Apps — WorkerSysCall (stopper created by dispatch loop)
// ============================================================================

std::string RcCmd_Apps (const QStringList& args)
{
	bool appsOnly = true;
	if (args.size () >= 2 && args[1].toLower () == "all")
		appsOnly = false;

	try
	{
		std::string out = "OK\n";
		UInt16 numDBs = DmNumDatabases (0);

		for (UInt16 i = 0; i < numDBs; i++)
		{
			LocalID dbID = DmGetDatabase (0, i);
			if (dbID == 0) continue;

			Char name[dmDBNameLength];
			UInt16 attributes = 0;
			UInt32 type = 0;
			UInt32 creator = 0;

			Err err = DmDatabaseInfo (0, dbID, name,
				&attributes, NULL, NULL, NULL, NULL, NULL,
				NULL, NULL, &type, &creator);
			if (err != errNone) continue;

			if (appsOnly && type != sysFileTApplication) continue;

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

			if (!appsOnly)
			{
				bool isResource = (attributes & dmHdrAttrResDB) != 0;
				out += isResource ? " res" : " rec";
			}

			out += "\n";
		}

		out += ".\n";
		return out;
	}
	catch (...)
	{
		return "ERR fatal: apps enumeration failed - emulator exception during ROM call\n";
	}
}

// ============================================================================
// RcCmd_Dialog — Custom (reads Qt dialog state, optionally responds)
// ============================================================================

void RcCmd_Dialog (ReControlSession* session, const QStringList& args)
{
	if (args.size () >= 3 && args[1].toLower () == "respond")
	{
		std::string buttonName = args[2].toLower ().toStdString ();
		if (EmDlgQt_RespondToDialog (buttonName))
			session->Send ("OK\n");
		else
			session->SendErr ("usage", "no pending dialog or unknown button: " + buttonName);
		return;
	}

	std::string info = EmDlgQt_GetPendingDialog ();
	if (info.empty ())
	{
		session->Send ("OK none\n.\n");
		return;
	}

	// Append CPU register dump when a dialog is pending
	if (gSession && gSession->GetSessionState () == kBlockedOnUI)
	{
		char buf[512];
		snprintf (buf, sizeof (buf),
			" regs PC=%08X SR=%04X"
			" D0=%08X D1=%08X D2=%08X D3=%08X D4=%08X D5=%08X D6=%08X D7=%08X"
			" A0=%08X A1=%08X A2=%08X A3=%08X A4=%08X A5=%08X A6=%08X A7=%08X\n",
			(unsigned) regs.pc, (unsigned) regs.sr,
			(unsigned) m68k_dreg (regs, 0), (unsigned) m68k_dreg (regs, 1),
			(unsigned) m68k_dreg (regs, 2), (unsigned) m68k_dreg (regs, 3),
			(unsigned) m68k_dreg (regs, 4), (unsigned) m68k_dreg (regs, 5),
			(unsigned) m68k_dreg (regs, 6), (unsigned) m68k_dreg (regs, 7),
			(unsigned) m68k_areg (regs, 0), (unsigned) m68k_areg (regs, 1),
			(unsigned) m68k_areg (regs, 2), (unsigned) m68k_areg (regs, 3),
			(unsigned) m68k_areg (regs, 4), (unsigned) m68k_areg (regs, 5),
			(unsigned) m68k_areg (regs, 6), (unsigned) m68k_areg (regs, 7));

		size_t dotPos = info.rfind (".\n");
		if (dotPos != std::string::npos)
			info.insert (dotPos, buf);
	}

	session->Send (info);
}

// ============================================================================
// RcCmd_Delete — WorkerSysCall (stopper created by dispatch loop)
// ============================================================================

std::string RcCmd_Delete (const QStringList& args)
{
	if (args.size () < 2)
		return "ERR usage: delete <dbname>\n";

	QString nameQ;
	for (int i = 1; i < args.size (); i++)
	{
		if (i > 1) nameQ += ' ';
		nameQ += args[i];
	}
	std::string name = nameQ.toStdString ();

	try
	{
		LocalID dbID = DmFindDatabase (0, name.c_str ());
		if (dbID == 0)
			return "ERR usage: database not found: " + name + "\n";

		Err err = DmDeleteDatabase (0, dbID);
		if (err != errNone)
			return "ERR fatal: DmDeleteDatabase failed (err=" + std::to_string (err) + ")\n";

		return "OK\n";
	}
	catch (...)
	{
		return "ERR fatal: delete failed - emulator exception during ROM call (recommend reset)\n";
	}
}


// ============================================================================
// RcCmd_Speed — Immediate (main thread): set/query emulation speed.
// Percent semantics match EmApplication::DoSetSpeed: 100 = 1x, 0 = Max
// (spelled "max" on the wire so a bare 0 can't be sent by accident).
// ============================================================================

std::string RcCmd_Speed (const QStringList& args)
{
	if (!gSession)
		return "ERR transient: no session\n";

	if (args.size () == 1)
	{
		int speed = gSession->fEmulationSpeed.load (std::memory_order_relaxed);
		if (speed == 0)
			return "OK max\n";
		return "OK " + std::to_string (speed) + "\n";
	}

	if (args.size () != 2)
		return "ERR usage: speed [<percent>|max]\n";

	long speed;
	if (args[1].toLower () == "max")
	{
		speed = 0;
	}
	else
	{
		bool ok = false;
		speed = args[1].toLong (&ok);
		if (!ok || speed < 1 || speed > 10000)
			return "ERR usage: speed [<percent 1-10000>|max]\n";
	}

	Preference<long> p (kPrefKeyEmulationSpeed);
	p = speed;
	gSession->fEmulationSpeed.store ((int) speed, std::memory_order_relaxed);

	return "OK\n";
}
