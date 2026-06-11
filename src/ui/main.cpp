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
#include "DebugMgr.h"
#include "PreferenceMgr.h"
#include "ReControl.h"
#include "CPUWorkerThread.h"

// Undefine Palm OS macros that conflict with Qt
#undef daysInYear
#undef monthsInYear

#include <QApplication>
#include <QTimer>
#include <QSurfaceFormat>

#include <cstdio>
#include <cstring>
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
	qtApp.setApplicationVersion ("0.9.1");

	// Parse --port and --no-recontrol before passing argv to Qt/POSE
	int recontrolPort = 6416;  // default
	for (int i = 1; i < argc; i++)
	{
		if (strcmp (argv[i], "--port") == 0 && i + 1 < argc)
		{
			recontrolPort = atoi (argv[i + 1]);
			// Remove --port and value from argv
			for (int j = i; j < argc - 2; j++)
				argv[j] = argv[j + 2];
			argc -= 2;
			i--;
		}
		else if (strcmp (argv[i], "--no-recontrol") == 0)
		{
			recontrolPort = 0;
			for (int j = i; j < argc - 1; j++)
				argv[j] = argv[j + 1];
			argc -= 1;
			i--;
		}
		else if (strcmp (argv[i], "--slp-debugger") == 0)
		{
			Debug::ForceSocketsThisRun ();
			for (int j = i; j < argc - 1; j++)
				argv[j] = argv[j + 1];
			argc -= 1;
			i--;
		}
	}

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

			// Start CPU worker thread BEFORE event loop
			extern CPUWorkerThread* gCPUWorker;
			gCPUWorker = new CPUWorkerThread();
			gCPUWorker->start();
			fprintf(stderr, "[main] CPU worker thread started\n");
			fflush(stderr);

			// Defer ReControl startup until event loop is running
			if (recontrolPort > 0)
			{
				QTimer::singleShot (0, [recontrolPort]() {
					ReControl_Startup (recontrolPort);
				});
			}

			// Enter the Qt event loop
			int exitCode = qtApp.exec ();

			// Shut down CPU worker thread
			if (gCPUWorker) {
				fprintf(stderr, "[main] Shutting down CPU worker thread\n");
				fflush(stderr);
				gCPUWorker->shutdown();
				delete gCPUWorker;
				gCPUWorker = nullptr;
			}

			// Shut down ReControl server
			ReControl_Shutdown ();

			return exitCode;
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
