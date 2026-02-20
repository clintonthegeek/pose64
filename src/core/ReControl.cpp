/* -*- mode: C++; tab-width: 4 -*- */
/* ReControl: TCP control interface for POSE64.
 *
 * Implements ReControlServer and ReControlSession classes for
 * TCP-based command dispatch. All I/O happens on the Qt event loop
 * (UI thread), eliminating race conditions with SuspendThread.
 */

#include "EmCommon.h"
#include "ReControl.h"
#include "EmSession.h"
#include "EmApplication.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QObject>
#include <QStringList>

#include <string>
#include <memory>

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

private:
	void Send (const std::string& msg);
	void SendErr (const std::string& category, const std::string& msg);

	void CmdState (const QStringList& args);
	void CmdQuit (const QStringList& args);

	QTcpSocket* fSocket;
	ReControlServer* fServer;
	QByteArray fReadBuffer;
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
	  fReadBuffer ()
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
