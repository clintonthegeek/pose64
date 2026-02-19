/* -*- mode: C++; tab-width: 4 -*- */
/* Qt PORT v2: Application entry point.
 *
 * Follows the original FLTK main() structure:
 *   1. Create EmulatorPreferences (sets gPrefs + gEmuPrefs)
 *   2. Create EmApplicationQt (sets gApplication)
 *   3. Call Startup() — loads prefs, inits sockets/debug/RPC/logging
 *   4. HandleStartupActions() — auto-open session or show dialog
 *   5. Run event loop with QTimer-based idle processing
 *   6. Shutdown() — save prefs, close sockets
 *
 * Key difference from FLTK:
 *   FLTK: while(1) { Fl::wait(0.1); HandleIdle(); }
 *   Qt:   QApplication::exec() with QTimer firing HandleIdle()
 */

#include "EmCommon.h"
#include "EmApplicationQt.h"
#include "EmDocument.h"
#include "PreferenceMgr.h"

// Undefine Palm OS macros that conflict with Qt
#undef daysInYear
#undef monthsInYear

#include <QApplication>
#include <QTimer>
#include <QSurfaceFormat>

#include <cstdio>
#include <exception>

int main (int argc, char** argv)
{
	setvbuf (stderr, NULL, _IONBF, 0);  // unbuffered stderr for diagnostics

	// Request an alpha channel in the default surface format.  Without
	// this, WA_TranslucentBackground has no effect because the window
	// surface is created as opaque RGB (no alpha plane).
	QSurfaceFormat fmt = QSurfaceFormat::defaultFormat ();
	fmt.setAlphaBufferSize (8);
	QSurfaceFormat::setDefaultFormat (fmt);

	QApplication qtApp (argc, argv);
	qtApp.setApplicationName ("pose64");
	qtApp.setDesktopFileName ("ca.vibekoder.pose64");
	qtApp.setOrganizationDomain ("ca.vibekoder");
	qtApp.setOrganizationName ("VibeKoder");
	qtApp.setApplicationVersion ("0.9.0");

	// Create preferences and application objects on the stack,
	// exactly as in the FLTK main().
	EmulatorPreferences	prefs;
	EmApplicationQt		theApp;

	try
	{
		if (theApp.Startup (argc, argv))
		{
			// HandleStartupActions is called from Run().
			theApp.Run ();

			// Set up the idle timer.
			// This replaces FLTK's while(1) { Fl::wait(0.1); HandleIdle(); }
			QTimer idleTimer;
			QObject::connect (&idleTimer, &QTimer::timeout, [&]() {
				try
				{
					if (theApp.GetTimeToQuit ())
					{
						QApplication::quit ();
						return;
					}
					theApp.HandleIdle ();
				}
				catch (const std::exception& e)
				{
					fprintf (stderr, "POSE64: Exception in idle handler: %s\n", e.what());
					QApplication::quit ();
				}
				catch (...)
				{
					fprintf (stderr, "POSE64: Unknown exception in idle handler\n");
					QApplication::quit ();
				}
			});
			idleTimer.start (100);  // ~10 Hz, matching FLTK's Fl::wait(0.1)

			// Enter the Qt event loop
			qtApp.exec ();
		}
	}
	catch (const std::exception& e)
	{
		fprintf (stderr, "POSE64: Fatal Internal Error: %s\n", e.what());
	}
	catch (...)
	{
		fprintf (stderr, "POSE64: Fatal Internal Error (unknown exception)\n");
	}

	theApp.Shutdown ();

	return
		gErrorHappened ? 2 :
		gWarningHappened ? 1 : 0;
}
