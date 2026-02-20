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
#include <QStringList>
#include <QTimer>
#include <QImage>
#include <QFileInfo>

#include <string>
#include <memory>
#include <cstring>

#include "EmCommon.h"
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

// Forward declarations
class ReControlServer;
class ReControlSession;

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
	void ProcessBufferedCommands (void);

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
			Send ("OK suspended\n");
			break;
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
	// args: ["tap", "80", "80"]
	if (args.size () != 3) { SendErr ("usage", "tap <x> <y>"); return; }
	if (!gSession) { SendErr ("transient", "no session"); return; }

	int x = args[1].toInt ();
	int y = args[2].toInt ();

	EmPenEvent penDown (EmPoint (x, y), true);
	gSession->PostPenEvent (penDown);

	EmPenEvent penUp (EmPoint (-1, -1), false);
	gSession->PostPenEvent (penUp);

	Send ("OK\n");
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

	EmPenEvent penEvent (EmPoint (x, y), isDown);
	gSession->PostPenEvent (penEvent);

	Send ("OK\n");
}

void ReControlSession::CmdKey (const QStringList& args)
{
	if (args.size () != 2) { SendErr ("usage", "key <charcode>"); return; }
	if (!gSession) { SendErr ("transient", "no session"); return; }

	int charcode = args[1].toInt ();
	EmKeyEvent keyEvent (charcode);
	gSession->PostKeyEvent (keyEvent);

	Send ("OK\n");
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

	if (action == "down")      gSession->SetButtonDown (button);
	else if (action == "up")   gSession->SetButtonUp (button);
	else if (action == "tap")  gSession->SetButtonTap (button);
	else { SendErr ("usage", "button <name> <down|up|tap>"); return; }

	Send ("OK\n");
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

void ReControlSession::CmdScreenshot (const QStringList& args)
{
	if (args.size () != 2) { SendErr ("usage", "screenshot <filepath>"); return; }
	if (!gSession) { SendErr ("transient", "no session"); return; }

	EmSessionStopper stopper (gSession, kStopNow);
	if (!stopper.Stopped ())
	{
		SendErr ("transient", "could not stop session");
		return;
	}

	// Capture screen
	EmScreen::InvalidateAll ();

	EmScreenUpdateInfo info;
	info.fScreenLow  = 0;
	info.fScreenHigh = 0xFFFFFFFF;
	if (!EmScreen::GetBits (info))
	{
		SendErr ("transient", "could not capture screen");
		return;
	}

	// Convert EmPixMap to QImage
	EmPoint size = info.fImage.GetSize ();
	int w = size.fX;
	int h = size.fY;

	info.fImage.ConvertToFormat (kPixMapFormat24RGB);

	QImage img (w, h, QImage::Format_RGB888);
	const uint8_t* src = (const uint8_t*) info.fImage.GetBits ();
	EmPixMapRowBytes srcRowBytes = info.fImage.GetRowBytes ();
	for (int y = 0; y < h; y++)
	{
		memcpy (img.scanLine (y), src + y * srcRowBytes, w * 3);
	}

	// Save as PNG
	QString path = args[1];
	if (!img.save (path, "PNG"))
	{
		SendErr ("transient", "could not write " + path.toStdString ());
		return;
	}

	Send ("OK " + std::to_string (w) + " " + std::to_string (h) + "\n");
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

	EmSessionStopper stopper (gSession, kStopOnSysCall, 5000);
	if (!stopper.Stopped ())
	{
		SendErr ("timeout", "CPU did not reach syscall boundary within 5000ms");
		return;
	}

	try
	{
		EmStreamFile stream (EmFileRef (path.toStdString ()), kOpenExistingForRead);
		EmFileImport importer (stream, kMethodBest);

		// Call Continue() in a loop until it's done or returns an error
		while (!importer.Done ())
		{
			ErrCode err = importer.Continue ();
			if (err != errNone)
			{
				SendErr ("fatal", "install failed - EmFileImport returned error");
				return;
			}
		}

		Send ("OK\n");
	}
	catch (...)
	{
		SendErr ("fatal", "install failed - emulator exception during ROM call (recommend reset)");
	}
}

void ReControlSession::CmdLaunch (const QStringList& args)
{
	if (args.size () != 2) { SendErr ("usage", "launch <dbname>"); return; }
	if (!gSession) { SendErr ("transient", "no session"); return; }

	EmSessionStopper stopper (gSession, kStopOnSysCall, 5000);
	if (!stopper.Stopped () || !stopper.CanCall ())
	{
		SendErr ("timeout", "CPU did not reach syscall boundary within 5000ms");
		return;
	}

	try
	{
		std::string name = args[1].toStdString ();
		LocalID dbID = DmFindDatabase (0, name.c_str ());
		if (dbID == 0)
		{
			SendErr ("usage", "database not found: " + name);
			return;
		}

		Err err = SysUIAppSwitch (0, dbID, sysAppLaunchCmdNormalLaunch, NULL);
		if (err != errNone)
		{
			SendErr ("fatal", "SysUIAppSwitch failed");
			return;
		}

		Send ("OK\n");
	}
	catch (...)
	{
		SendErr ("fatal", "launch failed - emulator exception during ROM call (recommend reset)");
	}
}

void ReControlSession::CmdSave (const QStringList& args)
{
	if (args.size () != 2) { SendErr ("usage", "save <filepath>"); return; }
	if (!gSession) { SendErr ("transient", "no session"); return; }

	EmSessionStopper stopper (gSession, kStopNow);
	if (!stopper.Stopped ())
	{
		SendErr ("transient", "could not stop session");
		return;
	}

	EmFileRef ref (args[1].toStdString ());
	gSession->Save (ref, true);

	Send ("OK\n");
}

void ReControlSession::CmdLoad (const QStringList& args)
{
	// Phase 1.5 todo: Full implementation requires EmDocument cooperation
	SendErr ("usage", "load not yet implemented");
}

void ReControlSession::CmdInfo (const QStringList& args)
{
	// First line: OK with POSE64 version
	Send ("OK POSE64 0.9.0\n");

	if (gSession)
	{
		Configuration cfg = gSession->GetConfiguration ();

		// Device information
		Send (" device=" + cfg.fDevice.GetIDString () + "\n");

		// RAM size (in bytes, convert to MB for readability)
		int ramSizeMB = cfg.fRAMSize / (1024 * 1024);
		if (ramSizeMB > 0)
		{
			Send (" ram=" + std::to_string (ramSizeMB) + "MB\n");
		}

		// ROM filename
		if (cfg.fROMFile.IsSpecified ())
		{
			Send (" rom=" + cfg.fROMFile.GetName () + "\n");
		}

		// Screen dimensions - stop the session and get screen info
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
				Send (" screen=" + std::to_string (size.fX) + "x" + std::to_string (size.fY) + "\n");
			}
		}
	}

	// Session file path
	if (gDocument)
	{
		EmFileRef ref = gDocument->GetFileRef ();
		if (ref.IsSpecified ())
		{
			Send (" session=" + ref.GetFullPath () + "\n");
		}
	}

	// Terminator
	Send (".\n");
}

void ReControlSession::ProcessBufferedCommands ()
{
	while (!fCommandBuffer.isEmpty ())
	{
		QString line = fCommandBuffer.takeFirst ();
		// Parse and dispatch the command
		QStringList parts = line.split (' ', Qt::SkipEmptyParts);
		QString cmd = parts[0].toLower ();

		// Dispatch command
		if (cmd == "state")
		{
			CmdState (parts);
		}
		else if (cmd == "quit")
		{
			CmdQuit (parts);
		}
		else if (cmd == "tap")
		{
			CmdTap (parts);
		}
		else if (cmd == "pen")
		{
			CmdPen (parts);
		}
		else if (cmd == "key")
		{
			CmdKey (parts);
		}
		else if (cmd == "button")
		{
			CmdButton (parts);
		}
		else if (cmd == "reset")
		{
			CmdReset (parts);
		}
		else if (cmd == "screenshot")
		{
			CmdScreenshot (parts);
		}
		else if (cmd == "sleep")
		{
			CmdSleep (parts);
		}
		else if (cmd == "install")
		{
			CmdInstall (parts);
		}
		else if (cmd == "launch")
		{
			CmdLaunch (parts);
		}
		else if (cmd == "save")
		{
			CmdSave (parts);
		}
		else if (cmd == "load")
		{
			CmdLoad (parts);
		}
		else if (cmd == "info")
		{
			CmdInfo (parts);
		}
		else
		{
			SendErr ("usage", "unknown command '" + cmd.toStdString () + "'");
		}
	}
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
		QString cmd = parts[0].toLower ();

		// Dispatch command
		if (cmd == "state")
		{
			CmdState (parts);
		}
		else if (cmd == "quit")
		{
			CmdQuit (parts);
		}
		else if (cmd == "tap")
		{
			CmdTap (parts);
		}
		else if (cmd == "pen")
		{
			CmdPen (parts);
		}
		else if (cmd == "key")
		{
			CmdKey (parts);
		}
		else if (cmd == "button")
		{
			CmdButton (parts);
		}
		else if (cmd == "reset")
		{
			CmdReset (parts);
		}
		else if (cmd == "screenshot")
		{
			CmdScreenshot (parts);
		}
		else if (cmd == "sleep")
		{
			CmdSleep (parts);
		}
		else if (cmd == "install")
		{
			CmdInstall (parts);
		}
		else if (cmd == "launch")
		{
			CmdLaunch (parts);
		}
		else if (cmd == "save")
		{
			CmdSave (parts);
		}
		else if (cmd == "load")
		{
			CmdLoad (parts);
		}
		else if (cmd == "info")
		{
			CmdInfo (parts);
		}
		else
		{
			SendErr ("usage", "unknown command '" + cmd.toStdString () + "'");
		}
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
