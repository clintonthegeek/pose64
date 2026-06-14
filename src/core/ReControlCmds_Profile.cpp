/* -*- mode: C++; tab-width: 4 -*- */
/* ReControl: profile command handlers. */

#include "EmCommon.h"

#include "PalmMacroUndefs.h"	// Phase 5: daysInYear/monthsInYear undef

#include "ReControl.h"
#include "EmSession.h"
#include "Profiling.h"

#if HAS_PROFILING

std::string RcCmd_Profile (const QStringList& args)
{
	if (args.size () < 2)
		return "ERR usage: profile <init|start|stop|dump|print|cleanup|cycles>\n";

	QString sub = args[1].toLower ();

	// dump/print validate prerequisites and may return an error before doing
	// any work; those returns happen before the stopper does anything costly.
	std::string dumpPath;
	if (sub == "dump" || sub == "print")
	{
		if (args.size () < 3)
			return "ERR usage: profile " + sub.toStdString () + " <path>\n";
		dumpPath = args[2].toStdString ();
		if (!gProfilingEnabled)
			return "ERR transient: profiling not enabled (call profile init + start first)\n";
		if (gProfilingOn)
			return "ERR transient: profiling still running (call profile stop first)\n";
		if (gClockCycles == 0)
			return "ERR transient: no profiling data collected (run CPU with profiling enabled first)\n";
	}

	// One stopper for all subcommand work (every branch needs kStopNow).
	EmSessionStopper stopper (gSession, kStopNow);

	if (sub == "init")
	{
		int maxCalls = MAXFNCALLS;
		int maxDepth = 200;
		if (args.size () >= 3) maxCalls = args[2].toInt ();
		if (args.size () >= 4) maxDepth = args[3].toInt ();
		if (maxCalls < 1) maxCalls = MAXFNCALLS;
		if (maxDepth < 1) maxDepth = 200;
		ProfileInit (maxCalls, maxDepth);
		return "OK\n";
	}

	if (sub == "start")
	{
		ProfileStart ();
		return "OK\n";
	}

	if (sub == "stop")
	{
		ProfileStop ();
		return "OK\n";
	}

	if (sub == "dump")
	{
		ProfileDump (dumpPath.c_str ());
		return "OK\n";
	}

	if (sub == "print")
	{
		ProfilePrint (dumpPath.c_str ());
		return "OK\n";
	}

	if (sub == "cleanup")
	{
		ProfileCleanup ();
		return "OK\n";
	}

	if (sub == "cycles")
	{
		char buf[128];
		snprintf (buf, sizeof (buf), "OK clock=%lld read=%lld write=%lld\n",
			(long long) gClockCycles, (long long) gReadCycles, (long long) gWriteCycles);
		return std::string (buf);
	}

	return "ERR usage: profile <init|start|stop|dump|print|cleanup|cycles>\n";
}

#endif // HAS_PROFILING
