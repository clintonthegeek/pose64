/* -*- mode: C++; tab-width: 4 -*- */
/* Qt PORT: Qt-based EmApplication implementation.
 *
 * Follows EmApplicationFltk.cpp as the reference implementation.
 * Uses QTimer for idle processing instead of FLTK's Fl::wait().
 */

#include "EmCommon.h"
#include "EmApplicationQt.h"

#include "EmDlgQt.h"			// HandleDialogs
#include "EmDocument.h"			// gDocument
#include "EmMenus.h"			// MenuInitialize
#include "EmWindowQt.h"

// Undefine Palm OS macros that conflict with Qt
#undef daysInYear
#undef monthsInYear

#include <QApplication>
#include <QClipboard>

EmApplicationQt*	gHostApplication;

// These variables are defined in Platform_Unix.cpp.

const double			kClipboardFreq = 0.1;
extern ByteList			gClipboardDataPalm;
extern ByteList			gClipboardDataHost;
extern omni_mutex		gClipboardMutex;
extern omni_condition	gClipboardCondition;
extern Bool				gClipboardHaveOutgoingData;
extern Bool				gClipboardNeedIncomingData;
extern Bool				gClipboardPendingIncomingData;
extern Bool				gClipboardHaveIncomingData;


/***********************************************************************
 *
 * FUNCTION:	EmApplicationQt::EmApplicationQt
 *
 * DESCRIPTION:	Constructor.  Sets the global host application variable
 *				to point to us.
 *
 ***********************************************************************/

EmApplicationQt::EmApplicationQt (void) :
	EmApplication (),
	fAppWindow (NULL)
{
	EmAssert (gHostApplication == NULL);
	gHostApplication = this;
}


/***********************************************************************
 *
 * FUNCTION:	EmApplicationQt::~EmApplicationQt
 *
 * DESCRIPTION:	Destructor.  Closes our window and sets the host
 *				application variable to NULL.
 *
 ***********************************************************************/

EmApplicationQt::~EmApplicationQt (void)
{
	delete fAppWindow;

	EmAssert (gHostApplication == this);
	gHostApplication = NULL;
}


/***********************************************************************
 *
 * FUNCTION:	EmApplicationQt::Startup
 *
 * DESCRIPTION:	Performs one-time startup initialization.
 *
 ***********************************************************************/

Bool EmApplicationQt::Startup (int argc, char** argv)
{
	// Initialize the base system.
	// This calls gPrefs->Load(), CSocket::Startup(), Debug::Startup(),
	// RPC::Startup(), LogStartup(), and parses command-line args.

	if (!EmApplication::Startup (argc, argv))
		return false;

	// Create our window.

	this->PrvCreateWindow (argc, argv);

	// Start up the sub-systems.

	::MenuInitialize (false);

	return true;
}


/***********************************************************************
 *
 * FUNCTION:	EmApplicationQt::Run
 *
 * DESCRIPTION:	Run the application.  Called from main() after Startup.
 *				Uses QTimer for idle processing instead of FLTK's
 *				while(1) { Fl::wait(0.1); HandleIdle(); } loop.
 *
 ***********************************************************************/

void EmApplicationQt::Run (void)
{
	this->HandleStartupActions ();

	// The QTimer-based idle loop is set up in main() via QApplication::exec().
	// This function is called before entering the Qt event loop.
}


/***********************************************************************
 *
 * FUNCTION:	EmApplicationQt::Shutdown
 *
 * DESCRIPTION:	Performs one-time shutdown operations.
 *
 ***********************************************************************/

void EmApplicationQt::Shutdown (void)
{
	// Delete our window now, so that its position will be recorded
	// in the preferences before EmApplication::Shutdown saves them.

	delete fAppWindow;
	fAppWindow = NULL;

	// Perform common shutdown.

	EmApplication::Shutdown ();
}


/***********************************************************************
 *
 * FUNCTION:	EmApplicationQt::HandleIdle
 *
 * DESCRIPTION:	Perform idle-time operations.  Called from QTimer at ~10Hz.
 *
 ***********************************************************************/

void EmApplicationQt::HandleIdle (void)
{
	// Idle the clipboard.  Do this first, in case the CPU
	// thread is blocked waiting for the data.

	if (!this->PrvIdleClipboard ())
		return;	// CPU thread is still blocked.

	// Handle any modeless dialogs

	::HandleDialogs ();

	EmApplication::HandleIdle ();
}


/***********************************************************************
 *
 * FUNCTION:	EmApplicationQt::PrvCreateWindow
 *
 * DESCRIPTION:	Create the window that displays the LCD/skin stuff.
 *
 ***********************************************************************/

void EmApplicationQt::PrvCreateWindow (int argc, char** argv)
{
	fAppWindow = new EmWindowQt;
	fAppWindow->WindowInit ();
	fAppWindow->show ();
}


/***********************************************************************
 *
 * FUNCTION:	EmApplicationQt::PrvIdleClipboard
 *
 * DESCRIPTION:	Check to see if there is any incoming or outgoing
 *				clipboard data and handle it if there is.
 *
 * RETURNED:	TRUE if there is no pending incoming data.
 *
 ***********************************************************************/

Bool EmApplicationQt::PrvIdleClipboard (void)
{
	omni_mutex_lock lock (gClipboardMutex);

	// See if there's anything outgoing.

	if (gClipboardHaveOutgoingData)
	{
		// Copy the data to the system clipboard via Qt.
		fClipboardData = gClipboardDataHost;

		QClipboard* clipboard = QApplication::clipboard ();
		if (clipboard)
		{
			QByteArray data ((const char*) &fClipboardData[0],
							  (int) fClipboardData.size ());
			clipboard->setText (QString::fromLatin1 (data));
		}

		gClipboardHaveOutgoingData = false;
	}

	// See if there's a request for incoming data.

	if (gClipboardNeedIncomingData)
	{
		gClipboardNeedIncomingData = false;

		QClipboard* clipboard = QApplication::clipboard ();
		if (clipboard && !clipboard->text ().isEmpty ())
		{
			QByteArray data = clipboard->text ().toLatin1 ();

			gClipboardDataPalm.clear ();
			gClipboardDataHost.clear ();

			copy (data.constData (),
				  data.constData () + data.size (),
				  back_inserter (gClipboardDataHost));
		}

		gClipboardHaveIncomingData = true;
		gClipboardCondition.broadcast ();
	}

	return !gClipboardPendingIncomingData;
}
