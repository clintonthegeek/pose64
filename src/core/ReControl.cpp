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
#include <QPainter>
#include <QFont>
#include <QFileInfo>
#include <QApplication>

#include <string>
#include <memory>
#include <cstring>
#include <vector>
#include <zlib.h>
#include <QThread>

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
#include "LoadApplication.h"
#include "ROMStubs.h"
#include "PalmFormReader.h"
#include "Hardware/EmMemory.h"
#include "EmLowMem.h"
#include "CPUWorkerThread.h"
#include "Patches/EmPatchState.h"
#include "UAE.h"

// Forward declarations
class ReControlServer;
class ReControlSession;

// From EmDlgQt.cpp — remote dialog handling
extern std::string EmDlgQt_GetPendingDialog (void);
extern bool EmDlgQt_RespondToDialog (const std::string& buttonName);

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
	void CmdExport (const QStringList& args);
	void CmdLaunch (const QStringList& args);
	void CmdSave (const QStringList& args);
	void CmdLoad (const QStringList& args);
	void CmdInfo (const QStringList& args);
	void CmdUI (const QStringList& args);
	void CmdScreenHash (const QStringList& args);
	void CmdType (const QStringList& args);
	void CmdTapId (const QStringList& args);
	void CmdApps (const QStringList& args);
	void CmdDialog (const QStringList& args);
	void CmdRun (const QStringList& args);
	void CmdPeek (const QStringList& args);
	void CmdPoke (const QStringList& args);
	void CmdRegs (const QStringList& args);
	void CmdMenu (const QStringList& args);
	void DoMenuLookup (std::string menuTitle, std::string itemTitle, bool activated);
	void CmdDelete (const QStringList& args);
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
		.handler = [handler = std::move (handler)]() {
			try {
				handler ();
			} catch (...) {
				// RAII cleanup (EmSessionStopper) happens via stack unwinding.
				// Swallow — the response will still send "OK\n" which isn't
				// ideal, but preventing thread death is more important.
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

	// Use ForceReset so reset works even when the CPU is stuck
	// in a suspended state (debugger break, stale lock, etc.).
	gSession->ForceReset (type);
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
	if (args.size () < 2) { SendErr ("usage", "screenshot <filepath> [scale=N] [grid] [annotate] [crosshair=X,Y]"); return; }
	if (!gSession) { SendErr ("transient", "no session"); return; }

	QString path = args[1];

	// Parse optional flags
	int  scaleFactor  = 1;
	bool drawGrid     = false;
	bool drawAnnotate = false;
	bool drawCrosshair = false;
	int  crossX = 0, crossY = 0;

	for (int i = 2; i < args.size (); i++)
	{
		QString flag = args[i].toLower ();
		if (flag.startsWith ("scale="))
		{
			scaleFactor = flag.mid (6).toInt ();
			if (scaleFactor < 1) scaleFactor = 1;
			if (scaleFactor > 16) scaleFactor = 16;
		}
		else if (flag == "grid")
		{
			drawGrid = true;
		}
		else if (flag == "annotate")
		{
			drawAnnotate = true;
		}
		else if (flag.startsWith ("crosshair="))
		{
			drawCrosshair = true;
			QString coords = flag.mid (10);
			QStringList xy = coords.split (',');
			if (xy.size () == 2)
			{
				crossX = xy[0].toInt ();
				crossY = xy[1].toInt ();
			}
		}
	}

	QueueWorkResult ([path, scaleFactor, drawGrid, drawAnnotate, drawCrosshair, crossX, crossY]() -> std::string {
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

		// Build raw QImage from LCD framebuffer
		QImage raw (w, h, QImage::Format_RGB888);
		const uint8_t* src = (const uint8_t*) info.fImage.GetBits ();
		EmPixMapRowBytes srcRowBytes = info.fImage.GetRowBytes ();
		for (int y = 0; y < h; y++)
			memcpy (raw.scanLine (y), src + y * srcRowBytes, w * 3);

		bool hasOverlays = drawGrid || drawAnnotate || drawCrosshair || (scaleFactor > 1);

		if (!hasOverlays)
		{
			// Raw mode — save directly, same as before
			if (!raw.save (path, "PNG"))
				return "ERR transient: could not write " + path.toStdString () + "\n";
			return "OK " + hash + " " + std::to_string (w) + " " + std::to_string (h) + "\n";
		}

		// ── Scale ──────────────────────────────────────────────────
		int sw = w * scaleFactor;
		int sh = h * scaleFactor;
		int s  = scaleFactor;  // shorthand

		QImage img = raw.scaled (sw, sh, Qt::IgnoreAspectRatio, Qt::FastTransformation);

		QPainter painter (&img);
		painter.setRenderHint (QPainter::Antialiasing, false);

		// ── Grid ───────────────────────────────────────────────────
		if (drawGrid)
		{
			// Light gridlines every 10 Palm pixels
			QPen gridPen (QColor (255, 255, 255, 60), 1);
			painter.setPen (gridPen);
			for (int px = 10; px < w; px += 10)
				painter.drawLine (px * s, 0, px * s, sh);
			for (int py = 10; py < h; py += 10)
				painter.drawLine (0, py * s, sw, py * s);

			// Coordinate labels every 20 Palm pixels
			QFont font;
			font.setPixelSize (std::max (8, s * 3));
			painter.setFont (font);

			for (int px = 0; px <= w; px += 20)
			{
				QString num = QString::number (px);
				int tx = px * s + 2;

				// Outline: draw dark text offset in 4 directions
				painter.setPen (QColor (0, 0, 0, 200));
				painter.drawText (tx - 1, s * 3, num);
				painter.drawText (tx + 1, s * 3, num);
				painter.drawText (tx, s * 3 - 1, num);
				painter.drawText (tx, s * 3 + 1, num);
				// Foreground
				painter.setPen (QColor (255, 255, 0, 220));
				painter.drawText (tx, s * 3, num);
			}
			for (int py = 20; py <= h; py += 20)
			{
				QString num = QString::number (py);
				int ty = py * s + s * 2;

				painter.setPen (QColor (0, 0, 0, 200));
				painter.drawText (1, ty, num);
				painter.drawText (3, ty, num);
				painter.drawText (2, ty - 1, num);
				painter.drawText (2, ty + 1, num);
				painter.setPen (QColor (255, 255, 0, 220));
				painter.drawText (2, ty, num);
			}

			// Tick marks every 10 Palm pixels along edges
			QPen tickPen (QColor (255, 255, 0, 200), 1);
			painter.setPen (tickPen);
			for (int px = 10; px < w; px += 10)
			{
				int tickLen = (px % 20 == 0) ? s * 2 : s;
				painter.drawLine (px * s, 0, px * s, tickLen);
			}
			for (int py = 10; py < h; py += 10)
			{
				int tickLen = (py % 20 == 0) ? s * 2 : s;
				painter.drawLine (0, py * s, tickLen, py * s);
			}
		}

		// ── Annotate ───────────────────────────────────────────────
		if (drawAnnotate)
		{
			CEnableFullAccess munge;
			std::vector<PalmObjInfo> objs = PalmFormReader_GetObjectBounds ();

			QFont labelFont;
			labelFont.setPixelSize (std::max (8, s * 3));
			painter.setFont (labelFont);

			for (const auto& obj : objs)
			{
				// Color by type
				QColor color;
				switch (obj.type)
				{
					case kFrmControlObj:   color = QColor (80, 140, 255, 140); break; // blue
					case kFrmFieldObj:     color = QColor (80, 220, 80, 140);  break; // green
					case kFrmListObj:      color = QColor (255, 160, 40, 140); break; // orange
					case kFrmGadgetObj:    color = QColor (180, 80, 220, 140); break; // purple
					case kFrmScrollBarObj: color = QColor (220, 220, 40, 140); break; // yellow
					case kFrmTitleObj:     color = QColor (40, 200, 200, 140); break; // cyan
					case kFrmLabelObj:     color = QColor (200, 200, 200, 100); break; // gray
					default:               color = QColor (255, 255, 255, 100); break;
				}

				int rx = obj.screenX * s;
				int ry = obj.screenY * s;
				int rw = obj.w * s;
				int rh = obj.h * s;

				if (rw > 0 && rh > 0)
				{
					// Fill
					painter.fillRect (rx, ry, rw, rh, color);
					// Border
					QPen borderPen (color.darker (150), std::max (1, s / 2));
					painter.setPen (borderPen);
					painter.drawRect (rx, ry, rw, rh);
				}

				// ID label
				QString idText = QString::number (obj.id);
				int labelX = rx + 2;
				int labelY = ry - 2;
				if (labelY < labelFont.pixelSize ())
					labelY = ry + labelFont.pixelSize () + 2;  // below if too close to top

				// Outline
				painter.setPen (QColor (0, 0, 0, 220));
				painter.drawText (labelX - 1, labelY, idText);
				painter.drawText (labelX + 1, labelY, idText);
				painter.drawText (labelX, labelY - 1, idText);
				painter.drawText (labelX, labelY + 1, idText);
				// Foreground — use same hue as box but bright
				painter.setPen (color.lighter (200));
				painter.drawText (labelX, labelY, idText);
			}
		}

		// ── Crosshair ──────────────────────────────────────────────
		if (drawCrosshair)
		{
			int cx = crossX * s;
			int cy = crossY * s;

			QPen crossPen (QColor (255, 0, 0, 200), std::max (1, s / 2));
			painter.setPen (crossPen);
			painter.drawLine (cx, 0, cx, sh);    // vertical
			painter.drawLine (0, cy, sw, cy);     // horizontal

			// Coordinate label
			QFont crossFont;
			crossFont.setPixelSize (std::max (10, s * 3));
			crossFont.setBold (true);
			painter.setFont (crossFont);

			QString coordText = QString ("(%1,%2)").arg (crossX).arg (crossY);
			int tx = cx + s;
			int ty = cy - s;
			if (tx + s * 20 > sw) tx = cx - s * 20;  // flip side if near right edge
			if (ty < crossFont.pixelSize ()) ty = cy + crossFont.pixelSize () + s;

			painter.setPen (QColor (0, 0, 0, 220));
			painter.drawText (tx - 1, ty, coordText);
			painter.drawText (tx + 1, ty, coordText);
			painter.drawText (tx, ty - 1, coordText);
			painter.drawText (tx, ty + 1, coordText);
			painter.setPen (QColor (255, 50, 50, 255));
			painter.drawText (tx, ty, coordText);
		}

		painter.end ();

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

void ReControlSession::CmdExport (const QStringList& args)
{
	if (args.size () < 3) { SendErr ("usage", "export <dbname> <filepath>"); return; }
	if (!gSession) { SendErr ("transient", "no session"); return; }

	// Rejoin args[1..N-1] as dbname (may contain spaces), last arg is filepath
	QString path = args.last ();
	QString nameQ;
	for (int i = 1; i < args.size () - 1; i++)
	{
		if (i > 1) nameQ += ' ';
		nameQ += args[i];
	}
	std::string name = nameQ.toStdString ();
	std::string pathStr = path.toStdString ();

	QueueWorkResult ([name, pathStr]() -> std::string {
		if (!gSession) return "ERR transient: no session\n";
		EmSessionStopper stopper (gSession, kStopOnSysCall, 5000);
		if (!stopper.Stopped () || !stopper.CanCall ())
			return "ERR timeout: CPU did not reach syscall boundary within 5000ms\n";

		try
		{
			// Verify the database exists
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

	bool appsOnly = true;
	if (args.size () >= 2 && args[1].toLower () == "all")
		appsOnly = false;

	QueueWorkResult ([appsOnly]() -> std::string {
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
				UInt16 attributes = 0;
				UInt32 type = 0;
				UInt32 creator = 0;

				Err err = DmDatabaseInfo (0, dbID, name,
					&attributes, NULL, NULL, NULL, NULL, NULL,
					NULL, NULL, &type, &creator);
				if (err != errNone) continue;

				// Filter to applications only unless "all" requested
				if (appsOnly && type != sysFileTApplication) continue;

				// Format type and creator as 4-char codes
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
	});
}

void ReControlSession::CmdDialog (const QStringList& args)
{
	if (args.size () >= 3 && args[1].toLower () == "respond")
	{
		std::string buttonName = args[2].toLower ().toStdString ();
		if (EmDlgQt_RespondToDialog (buttonName))
			Send ("OK\n");
		else
			SendErr ("usage", "no pending dialog or unknown button: " + buttonName);
		return;
	}

	std::string info = EmDlgQt_GetPendingDialog ();
	if (info.empty ())
	{
		Send ("OK none\n");
		return;
	}

	Send (info);
}

// ============================================================================
// CmdRun — batch/script command
// ============================================================================
// Executes multiple sub-commands in a single worker thread invocation,
// eliminating TCP round-trip overhead.  Only input commands are allowed:
// tap, pen, key, type, button, sleep, repeat.
//
// Syntax:  run <cmd1>; <cmd2>; sleep <ms>; repeat <N> { <cmd>; <cmd> }

// Helper: execute one sub-command within the worker thread.
// Returns empty string on success, or error message on failure.
static std::string RunOneSub (const QString& cmdLine)
{
	QStringList parts = cmdLine.trimmed ().split (' ', Qt::SkipEmptyParts);
	if (parts.isEmpty ()) return "";

	QString cmd = parts[0].toLower ();

	if (cmd == "tap")
	{
		if (parts.size () != 3) return "tap requires 2 arguments";
		if (!gSession) return "no session";
		int x = parts[1].toInt ();
		int y = parts[2].toInt ();
		EmPenEvent penDown (EmPoint (x, y), true);
		gSession->PostPenEvent (penDown);
		EmPenEvent penUp (EmPoint (-1, -1), false);
		gSession->PostPenEvent (penUp);
		return "";
	}

	if (cmd == "pen")
	{
		if (parts.size () != 4) return "pen requires 3 arguments";
		if (!gSession) return "no session";
		QString state = parts[1].toLower ();
		if (state != "down" && state != "up") return "pen action must be down or up";
		int x = parts[2].toInt ();
		int y = parts[3].toInt ();
		EmPenEvent penEvent (EmPoint (x, y), state == "down");
		gSession->PostPenEvent (penEvent);
		return "";
	}

	if (cmd == "key")
	{
		if (parts.size () != 2) return "key requires 1 argument";
		if (!gSession) return "no session";
		int charcode = parts[1].toInt ();
		EmKeyEvent keyEvent (charcode);
		gSession->PostKeyEvent (keyEvent);
		return "";
	}

	if (cmd == "type")
	{
		if (parts.size () < 2) return "type requires text";
		if (!gSession) return "no session";
		QString text;
		for (int i = 1; i < parts.size (); i++)
		{
			if (i > 1) text += ' ';
			text += parts[i];
		}
		QByteArray latin1 = text.toLatin1 ();
		for (int i = 0; i < latin1.size (); i++)
		{
			EmKeyEvent keyEvent ((unsigned char) latin1[i]);
			gSession->PostKeyEvent (keyEvent);
		}
		return "";
	}

	if (cmd == "button")
	{
		if (parts.size () != 3) return "button requires 2 arguments";
		if (!gSession) return "no session";
		QString name = parts[1].toLower ();
		QString action = parts[2].toLower ();

		SkinElementType button = kElement_None;
		if (name == "power")        button = kElement_PowerButton;
		else if (name == "up")      button = kElement_UpButton;
		else if (name == "down")    button = kElement_DownButton;
		else if (name == "app1")    button = kElement_App1Button;
		else if (name == "app2")    button = kElement_App2Button;
		else if (name == "app3")    button = kElement_App3Button;
		else if (name == "app4")    button = kElement_App4Button;
		else if (name == "cradle")  button = kElement_CradleButton;
		else if (name == "contrast") button = kElement_ContrastButton;
		else return "unknown button '" + name.toStdString () + "'";

		if (action == "down")      gSession->SetButtonDown (button);
		else if (action == "up")   gSession->SetButtonUp (button);
		else if (action == "tap")  gSession->SetButtonTap (button);
		else return "button action must be down, up, or tap";
		return "";
	}

	if (cmd == "sleep")
	{
		if (parts.size () != 2) return "sleep requires 1 argument";
		int ms = parts[1].toInt ();
		if (ms < 1 || ms > 30000) return "sleep ms must be 1-30000";
		QThread::msleep (ms);
		return "";
	}

	return "unknown command '" + cmd.toStdString () + "' (only tap/pen/key/type/button/sleep/repeat allowed in run)";
}

// Helper: execute a list of sub-commands (semicolon-delimited tokens already split).
// Handles "repeat N { ... }" by recursion.
// Returns empty string on success, or "command N '<cmd>': <reason>" on failure.
static std::string RunSubCommands (const QStringList& subcmds)
{
	int cmdNum = 0;

	for (int i = 0; i < subcmds.size (); i++)
	{
		QString sub = subcmds[i].trimmed ();
		if (sub.isEmpty ()) continue;
		cmdNum++;

		// Check for "repeat N { ... }"
		QStringList words = sub.split (' ', Qt::SkipEmptyParts);
		if (!words.isEmpty () && words[0].toLower () == "repeat")
		{
			if (words.size () < 2)
				return "command " + std::to_string (cmdNum) + " 'repeat': requires count";

			int count = words[1].toInt ();
			if (count < 1 || count > 10000)
				return "command " + std::to_string (cmdNum) + " 'repeat': count must be 1-10000";

			// Collect the brace-enclosed body from remaining subcmds
			// The opening brace should be the next token or at the end of this subcmd
			// Format: repeat N { cmd1; cmd2 }
			// After semicolon split, we look for a subcmd starting with { and ending with }

			// First, check if there's a { in the rest of this word list
			QString body;
			bool foundOpen = false;
			bool foundClose = false;

			// Check if brace is in the remaining words of this subcmd
			for (int w = 2; w < words.size (); w++)
			{
				QString word = words[w];
				if (word.startsWith ('{'))
				{
					foundOpen = true;
					word = word.mid (1);  // strip leading brace
				}
				if (word.endsWith ('}'))
				{
					foundClose = true;
					word.chop (1);  // strip trailing brace
				}
				if (!body.isEmpty ()) body += ' ';
				body += word;
			}

			// If we didn't find close brace, scan following subcmds
			if (foundOpen && !foundClose)
			{
				for (i++; i < subcmds.size (); i++)
				{
					QString next = subcmds[i].trimmed ();
					if (next.endsWith ('}'))
					{
						next.chop (1);
						if (!body.isEmpty ()) body += ';';
						body += next;
						foundClose = true;
						break;
					}
					if (!body.isEmpty ()) body += ';';
					body += next;
				}
			}

			if (!foundOpen || !foundClose)
				return "command " + std::to_string (cmdNum) + " 'repeat': missing { } body";

			// Split body on semicolons and execute N times
			QStringList bodyParts = body.split (';');
			for (int r = 0; r < count; r++)
			{
				std::string err = RunSubCommands (bodyParts);
				if (!err.empty ())
					return "command " + std::to_string (cmdNum) + " repeat iteration " + std::to_string (r + 1) + ": " + err;
			}
			continue;
		}

		// Normal sub-command
		std::string err = RunOneSub (sub);
		if (!err.empty ())
			return "command " + std::to_string (cmdNum) + " '" + sub.toStdString () + "': " + err;
	}

	return "";
}

void ReControlSession::CmdRun (const QStringList& args)
{
	if (args.size () < 2) { SendErr ("usage", "run <cmd1>; <cmd2>; ..."); return; }
	if (!gSession) { SendErr ("transient", "no session"); return; }

	// Rejoin everything after "run" and split on semicolons
	QString script;
	for (int i = 1; i < args.size (); i++)
	{
		if (i > 1) script += ' ';
		script += args[i];
	}

	QStringList subcmds = script.split (';');

	QueueWorkResult ([subcmds]() -> std::string {
		if (!gSession) return "ERR transient: no session\n";

		std::string err = RunSubCommands (subcmds);
		if (!err.empty ())
			return "ERR transient: " + err + "\n";

		// Count non-empty commands
		int count = 0;
		for (const QString& s : subcmds)
			if (!s.trimmed ().isEmpty ()) count++;
		return "OK " + std::to_string (count) + " commands\n";
	});
}

// ============================================================================
// CmdPeek — read emulated memory
// ============================================================================
// Address formats:
//   0x00012345    — absolute hex
//   a5@-6423      — A5-relative (signed decimal offset)
//   global.<name> — named low-memory global pointer

static bool ParseAddress (const std::string& addrStr, emuptr& outAddr)
{
	// Absolute hex: 0x...
	if (addrStr.size () > 2 && addrStr[0] == '0' && (addrStr[1] == 'x' || addrStr[1] == 'X'))
	{
		unsigned long val = strtoul (addrStr.c_str () + 2, nullptr, 16);
		outAddr = (emuptr) val;
		return true;
	}

	// A5-relative: a5@<offset> or a5@-<offset>
	if (addrStr.size () > 3 &&
		(addrStr[0] == 'a' || addrStr[0] == 'A') &&
		addrStr[1] == '5' && addrStr[2] == '@')
	{
		int offset = atoi (addrStr.c_str () + 3);
		uint32 a5 = m68k_areg (regs, 5);
		outAddr = (emuptr) ((int32) a5 + offset);
		return true;
	}

	// Named global: global.<name>
	if (addrStr.size () > 7 && addrStr.substr (0, 7) == "global.")
	{
		std::string name = addrStr.substr (7);
		CEnableFullAccess munge;
		if (name == "uiCurrentMenu")
			outAddr = EmLowMem_GetGlobal (uiGlobalsCommon.uiCurrentMenu);
		else if (name == "currentForm")
			outAddr = EmLowMem_GetGlobal (uiGlobalsCommon.currentForm);
		else
			return false;
		return true;
	}

	// Plain decimal
	unsigned long val = strtoul (addrStr.c_str (), nullptr, 10);
	if (val > 0)
	{
		outAddr = (emuptr) val;
		return true;
	}

	return false;
}

void ReControlSession::CmdPeek (const QStringList& args)
{
	if (args.size () != 3) { SendErr ("usage", "peek <addr> <nbytes>"); return; }
	if (!gSession) { SendErr ("transient", "no session"); return; }

	std::string addrStr = args[1].toStdString ();
	int nbytes = args[2].toInt ();
	if (nbytes < 1 || nbytes > 256) { SendErr ("usage", "nbytes must be 1-256"); return; }

	QueueWorkResult ([addrStr, nbytes]() -> std::string {
		if (!gSession) return "ERR transient: no session\n";
		EmSessionStopper stopper (gSession, kStopOnCycle);
		if (!stopper.Stopped ()) return "ERR transient: could not stop session\n";

		CEnableFullAccess munge;

		emuptr addr;
		if (!ParseAddress (addrStr, addr))
			return "ERR usage: invalid address '" + addrStr + "'\n";

		// Read bytes and format as hex
		std::string hex;
		hex.reserve (nbytes * 2);
		for (int i = 0; i < nbytes; i++)
		{
			uint8 b = EmMemGet8 (addr + i);
			char buf[4];
			snprintf (buf, sizeof (buf), "%02X", b);
			hex += buf;
		}

		return "OK " + hex + "\n";
	});
}

// ============================================================================
// CmdPoke — write emulated memory
// ============================================================================

void ReControlSession::CmdPoke (const QStringList& args)
{
	if (args.size () != 4) { SendErr ("usage", "poke <addr> <nbytes> <hexdata>"); return; }
	if (!gSession) { SendErr ("transient", "no session"); return; }

	std::string addrStr = args[1].toStdString ();
	int nbytes = args[2].toInt ();
	std::string hexdata = args[3].toStdString ();

	if (nbytes < 1 || nbytes > 256) { SendErr ("usage", "nbytes must be 1-256"); return; }
	if ((int) hexdata.size () != nbytes * 2) { SendErr ("usage", "hexdata length must be nbytes*2"); return; }

	// Parse hex data
	std::vector<uint8> data;
	data.reserve (nbytes);
	for (int i = 0; i < nbytes; i++)
	{
		char hi = hexdata[i * 2];
		char lo = hexdata[i * 2 + 1];
		auto hexVal = [](char c) -> int {
			if (c >= '0' && c <= '9') return c - '0';
			if (c >= 'a' && c <= 'f') return c - 'a' + 10;
			if (c >= 'A' && c <= 'F') return c - 'A' + 10;
			return -1;
		};
		int h = hexVal (hi);
		int l = hexVal (lo);
		if (h < 0 || l < 0) { SendErr ("usage", "invalid hex in data"); return; }
		data.push_back ((uint8) ((h << 4) | l));
	}

	QueueWorkResult ([addrStr, data]() -> std::string {
		if (!gSession) return "ERR transient: no session\n";
		EmSessionStopper stopper (gSession, kStopOnCycle);
		if (!stopper.Stopped ()) return "ERR transient: could not stop session\n";

		CEnableFullAccess munge;

		emuptr addr;
		if (!ParseAddress (addrStr, addr))
			return "ERR usage: invalid address '" + addrStr + "'\n";

		for (size_t i = 0; i < data.size (); i++)
			EmMemPut8 (addr + i, data[i]);

		return "OK\n";
	});
}

// ============================================================================
// CmdRegs — dump m68k registers
// ============================================================================

void ReControlSession::CmdRegs (const QStringList& args)
{
	if (!gSession) { SendErr ("transient", "no session"); return; }

	QueueWorkResult ([]() -> std::string {
		if (!gSession) return "ERR transient: no session\n";
		EmSessionStopper stopper (gSession, kStopOnCycle);
		if (!stopper.Stopped ()) return "ERR transient: could not stop session\n";

		char buf[512];
		snprintf (buf, sizeof (buf),
			"OK D0=%08X D1=%08X D2=%08X D3=%08X D4=%08X D5=%08X D6=%08X D7=%08X"
			" A0=%08X A1=%08X A2=%08X A3=%08X A4=%08X A5=%08X A6=%08X A7=%08X"
			" PC=%08X SR=%04X\n",
			(unsigned) m68k_dreg (regs, 0), (unsigned) m68k_dreg (regs, 1),
			(unsigned) m68k_dreg (regs, 2), (unsigned) m68k_dreg (regs, 3),
			(unsigned) m68k_dreg (regs, 4), (unsigned) m68k_dreg (regs, 5),
			(unsigned) m68k_dreg (regs, 6), (unsigned) m68k_dreg (regs, 7),
			(unsigned) m68k_areg (regs, 0), (unsigned) m68k_areg (regs, 1),
			(unsigned) m68k_areg (regs, 2), (unsigned) m68k_areg (regs, 3),
			(unsigned) m68k_areg (regs, 4), (unsigned) m68k_areg (regs, 5),
			(unsigned) m68k_areg (regs, 6), (unsigned) m68k_areg (regs, 7),
			(unsigned) regs.pc, (unsigned) regs.sr);

		return std::string (buf);
	});
}

// ============================================================================
// CmdMenu — trigger menu item by name
// ============================================================================
// Syntax:  menu <"Menu Title"> <"Item Title">
// Looks up the menu bar from uiCurrentMenu, finds the matching item,
// and posts a menuEvent with the item's ID.
//
// If the menu bar is not active, auto-activates it by posting a vchrMenu
// key event, letting the CPU process it, then retrying the lookup.

void ReControlSession::CmdMenu (const QStringList& args)
{
	if (args.size () < 3) { SendErr ("usage", "menu <menutitle> <itemtitle>"); return; }
	if (!gSession) { SendErr ("transient", "no session"); return; }

	// Rejoin args after "menu" to parse quoted strings
	QString fullArgs;
	for (int i = 1; i < args.size (); i++)
	{
		if (i > 1) fullArgs += ' ';
		fullArgs += args[i];
	}

	// Parse two arguments (possibly quoted)
	QStringList parsed;
	QString current;
	bool inQuotes = false;
	for (int i = 0; i < fullArgs.size (); i++)
	{
		QChar ch = fullArgs[i];
		if (ch == '"')
		{
			inQuotes = !inQuotes;
			continue;
		}
		if (ch == ' ' && !inQuotes)
		{
			if (!current.isEmpty ())
			{
				parsed.append (current);
				current.clear ();
			}
			continue;
		}
		current += ch;
	}
	if (!current.isEmpty ())
		parsed.append (current);

	if (parsed.size () < 2) { SendErr ("usage", "menu <menutitle> <itemtitle>"); return; }

	std::string menuTitle = parsed[0].toStdString ();
	std::string itemTitle = parsed[1].toStdString ();

	DoMenuLookup (menuTitle, itemTitle, false);
}

// ----------------------------------------------------------------------------
// DoMenuLookup — internal helper for CmdMenu
// If the menu bar is not active and `activated` is false, posts vchrMenu key
// to the emulator's key queue, waits 300ms for the CPU to process it, then
// retries with activated=true.

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
					// Check that the form has a menu before attempting activation
					emuptr formPtr = EmLowMem_GetGlobal (uiGlobalsCommon.currentForm);
					if (formPtr == 0) { *result = "ERR transient: no active form\n"; return; }

					uint16 menuRscId = EmMemGet16 (formPtr + kFormType_menuRscId);
					if (menuRscId == 0)
					{
						*result = "ERR transient: current form has no menu bar\n";
						return;
					}

					// Post vchrMenu to the emulator key queue.  When the CPU
					// resumes it will process the key, which causes Palm OS to
					// call MenuHandleEvent -> MenuInit -> set uiCurrentMenu.
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

				// Found the menu — search items
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

					// Case-insensitive: exact match first, then substring
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

					// Post menuEvent
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
				// Menu bar was not active — we posted vchrMenu, now wait
				// for the CPU to process it and retry.
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
// CmdDelete — delete a database
// ============================================================================

void ReControlSession::CmdDelete (const QStringList& args)
{
	if (args.size () < 2) { SendErr ("usage", "delete <dbname>"); return; }
	if (!gSession) { SendErr ("transient", "no session"); return; }

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

			Err err = DmDeleteDatabase (0, dbID);
			if (err != errNone)
				return "ERR fatal: DmDeleteDatabase failed (err=" + std::to_string (err) + ")\n";

			return "OK\n";
		}
		catch (...)
		{
			return "ERR fatal: delete failed - emulator exception during ROM call (recommend reset)\n";
		}
	});
}

// ============================================================================

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
	else if (cmd == "export")      CmdExport (parts);
	else if (cmd == "launch")      CmdLaunch (parts);
	else if (cmd == "save")        CmdSave (parts);
	else if (cmd == "load")        CmdLoad (parts);
	else if (cmd == "info")        CmdInfo (parts);
	else if (cmd == "ui")          CmdUI (parts);
	else if (cmd == "apps")        CmdApps (parts);
	else if (cmd == "dialog")      CmdDialog (parts);
	else if (cmd == "run")         CmdRun (parts);
	else if (cmd == "peek")        CmdPeek (parts);
	else if (cmd == "poke")        CmdPoke (parts);
	else if (cmd == "regs")        CmdRegs (parts);
	else if (cmd == "menu")        CmdMenu (parts);
	else if (cmd == "delete")      CmdDelete (parts);
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
