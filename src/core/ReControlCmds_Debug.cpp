/* -*- mode: C++; tab-width: 4 -*- */
/* ReControl: debug command handlers (break, watch, spy, log, gremlin, check, errorhandling). */

#include "EmCommon.h"

// Undefine Palm OS macros that conflict with Qt
#undef daysInYear
#undef monthsInYear

#include "ReControl.h"
#include "EmSession.h"
#include "DebugMgr.h"
#include "Hordes.h"
#include "CGremlins.h"
#include "Logging.h"
#include "PreferenceMgr.h"
#include "ErrorHandling.h"
#include "EmLowMem.h"
#include "Hardware/EmMemory.h"
#include "UAE.h"

#include <string>
#include <cstring>

// ParseAddress is defined in ReControlCmds_Query.cpp
extern bool ParseAddress (const std::string& addrStr, emuptr& outAddr);

// ============================================================================
// RcCmd_Break — Adaptive (runs directly while blocked_on_ui, else under the
// dispatch cycle stopper).  The CPU thread is frozen on the dialog in the
// blocked case, so mutating gDebuggerGlobals.bp[] + the meta-memory
// instruction-break bits is safe; the dialog's resume path holds its
// (index, pc) by value and never re-reads the table (GATE 3 Gap 1).
// ============================================================================

std::string RcCmd_Break (const QStringList& args)
{
	if (args.size () < 2)
		return "ERR usage: break <list|set|clear|clearall|enable|disable> [args...]\n";

	QString sub = args[1].toLower ();

	if (sub == "list")
	{
		std::string result = "OK break list\n";
		for (int i = 0; i < dbgTotalBreakpoints; i++)
		{
			char buf[256];
			const char* condStr = "";
			if (gDebuggerGlobals.bpCondition[i] && gDebuggerGlobals.bpCondition[i]->source)
				condStr = gDebuggerGlobals.bpCondition[i]->source;

			snprintf (buf, sizeof (buf), " [%d] %s addr=%08X%s%s",
				i,
				gDebuggerGlobals.bp[i].enabled ? "enabled " : "disabled",
				(unsigned)(uintptr_t) gDebuggerGlobals.bp[i].addr,
				condStr[0] ? " condition=\"" : "",
				condStr[0] ? condStr : "");
			std::string line (buf);
			if (condStr[0])
				line += "\"";
			line += "\n";
			result += line;
		}
		result += ".\n";
		return result;
	}

	if (sub == "set")
	{
		if (args.size () < 4)
			return "ERR usage: break set <index> <addr> [condition]\n";

		int index = args[2].toInt ();
		if (index < 0 || index >= dbgTotalBreakpoints)
			return "ERR usage: index must be 0-" + std::to_string (dbgTotalBreakpoints - 1) + "\n";

		std::string addrStr = args[3].toStdString ();
		emuptr addr;
		if (!ParseAddress (addrStr, addr))
			return "ERR usage: invalid address '" + addrStr + "'\n";

		// Optional condition string (remaining args joined)
		std::string condStr;
		for (int i = 4; i < args.size (); i++)
		{
			if (!condStr.empty ()) condStr += ' ';
			condStr += args[i].toStdString ();
		}

		BreakpointCondition* cond = nullptr;
		if (!condStr.empty ())
		{
			cond = Debug::NewBreakpointCondition (condStr.c_str ());
			if (!cond)
				return "ERR usage: invalid condition '" + condStr + "'\n";
		}

		Debug::SetBreakpoint (index, addr, cond);
		return "OK\n";
	}

	if (sub == "clear")
	{
		if (args.size () < 3)
			return "ERR usage: break clear <index>\n";
		int index = args[2].toInt ();
		if (index < 0 || index >= dbgTotalBreakpoints)
			return "ERR usage: index must be 0-" + std::to_string (dbgTotalBreakpoints - 1) + "\n";

		Debug::ClearBreakpoint (index);
		return "OK\n";
	}

	if (sub == "enable" || sub == "disable")
	{
		if (args.size () < 3)
			return "ERR usage: break " + sub.toStdString () + " <index>\n";
		int index = args[2].toInt ();
		if (index < 0 || index >= dbgTotalBreakpoints)
			return "ERR usage: index must be 0-" + std::to_string (dbgTotalBreakpoints - 1) + "\n";

		gDebuggerGlobals.bp[index].enabled = (sub == "enable");
		return "OK\n";
	}

	if (sub == "clearall")
	{
		for (int i = 0; i < dbgTotalBreakpoints; i++)
			Debug::ClearBreakpoint (i);
		return "OK\n";
	}

	return "ERR usage: break <list|set|clear|clearall|enable|disable> [args...]\n";
}

// ============================================================================
// RcCmd_Watch — WorkerCycle (all sub-commands run under dispatch stopper)
// ============================================================================

std::string RcCmd_Watch (const QStringList& args)
{
	if (args.size () < 2)
		return "ERR usage: watch <set|clear|status>\n";

	QString sub = args[1].toLower ();

	if (sub == "status")
	{
		char buf[128];
		if (gDebuggerGlobals.watchEnabled)
			snprintf (buf, sizeof (buf), "OK watch enabled addr=%08X nbytes=%u\n",
				(unsigned) gDebuggerGlobals.watchAddr,
				(unsigned) gDebuggerGlobals.watchBytes);
		else
			snprintf (buf, sizeof (buf), "OK watch disabled\n");
		return std::string (buf);
	}

	if (sub == "clear")
	{
		gDebuggerGlobals.watchEnabled = false;
		gDebuggerGlobals.watchAddr = 0;
		gDebuggerGlobals.watchBytes = 0;
		return "OK\n";
	}

	if (sub == "set")
	{
		if (args.size () < 4)
			return "ERR usage: watch set <addr> <nbytes>\n";

		std::string addrStr = args[2].toStdString ();
		int nbytes = args[3].toInt ();
		if (nbytes < 1 || nbytes > 65536)
			return "ERR usage: nbytes must be 1-65536\n";

		emuptr addr;
		if (!ParseAddress (addrStr, addr))
			return "ERR usage: invalid address '" + addrStr + "'\n";

		gDebuggerGlobals.watchEnabled = true;
		gDebuggerGlobals.watchAddr = addr;
		gDebuggerGlobals.watchBytes = nbytes;
		return "OK\n";
	}

	return "ERR usage: watch <set|clear|status>\n";
}

// ============================================================================
// RcCmd_Spy — WorkerCycle (all sub-commands run under dispatch stopper)
// ============================================================================

std::string RcCmd_Spy (const QStringList& args)
{
	if (args.size () < 2)
		return "ERR usage: spy <set|clear|status>\n";

	QString sub = args[1].toLower ();

	if (sub == "status")
	{
		char buf[128];
		if (gDebuggerGlobals.stepSpy)
			snprintf (buf, sizeof (buf), "OK spy enabled addr=%08X value=%08X\n",
				(unsigned) gDebuggerGlobals.ssAddr,
				(unsigned) gDebuggerGlobals.ssValue);
		else
			snprintf (buf, sizeof (buf), "OK spy disabled\n");
		return std::string (buf);
	}

	if (sub == "clear")
	{
		gDebuggerGlobals.stepSpy = false;
		gDebuggerGlobals.ssAddr = 0;
		gDebuggerGlobals.ssValue = 0;
		return "OK\n";
	}

	if (sub == "set")
	{
		if (args.size () < 3)
			return "ERR usage: spy set <addr>\n";
		std::string addrStr = args[2].toStdString ();

		emuptr addr;
		if (!ParseAddress (addrStr, addr))
			return "ERR usage: invalid address '" + addrStr + "'\n";

		CEnableFullAccess munge;
		gDebuggerGlobals.stepSpy = true;
		gDebuggerGlobals.ssAddr = addr;
		gDebuggerGlobals.ssValue = EmMemGet32 (addr);
		return "OK\n";
	}

	return "ERR usage: spy <set|clear|status>\n";
}

// ============================================================================
// RcCmd_Log — Immediate (preferences only, no CPU interaction)
// ============================================================================

std::string RcCmd_Log (const QStringList& args)
{
	if (args.size () < 2)
		return "ERR usage: log <list|set|dump|clear>\n";

	static const struct {
		const char* name;
		PrefKeyType key;
	} kLogCategories[] = {
		{ "ErrorMessages",     kPrefKeyLogErrorMessages     },
		{ "WarningMessages",   kPrefKeyLogWarningMessages   },
		{ "Gremlins",          kPrefKeyLogGremlins           },
		{ "CPUOpcodes",        kPrefKeyLogCPUOpcodes         },
		{ "EnqueuedEvents",    kPrefKeyLogEnqueuedEvents     },
		{ "DequeuedEvents",    kPrefKeyLogDequeuedEvents     },
		{ "SystemCalls",       kPrefKeyLogSystemCalls        },
		{ "ApplicationCalls",  kPrefKeyLogApplicationCalls   },
		{ "Serial",            kPrefKeyLogSerial             },
		{ "SerialData",        kPrefKeyLogSerialData         },
		{ "NetLib",            kPrefKeyLogNetLib             },
		{ "NetLibData",        kPrefKeyLogNetLibData         },
		{ "ExgMgr",            kPrefKeyLogExgMgr             },
		{ "ExgMgrData",        kPrefKeyLogExgMgrData         },
		{ "HLDebugger",        kPrefKeyLogHLDebugger         },
		{ "HLDebuggerData",    kPrefKeyLogHLDebuggerData     },
		{ "LLDebugger",        kPrefKeyLogLLDebugger         },
		{ "LLDebuggerData",    kPrefKeyLogLLDebuggerData     },
		{ "RPC",               kPrefKeyLogRPC                },
		{ "RPCData",           kPrefKeyLogRPCData            },
	};
	const int kNumCategories = sizeof (kLogCategories) / sizeof (kLogCategories[0]);

	QString sub = args[1].toLower ();

	if (sub == "list")
	{
		std::string result = "OK log list\n";
		for (int i = 0; i < kNumCategories; i++)
		{
			Preference<uint8> pref (kLogCategories[i].key);
			char buf[64];
			snprintf (buf, sizeof (buf), " %s=%d\n", kLogCategories[i].name, (int) *pref);
			result += buf;
		}
		result += ".\n";
		return result;
	}

	if (sub == "set")
	{
		if (args.size () < 4)
			return "ERR usage: log set <category> <0|1|2>\n";

		std::string catName = args[2].toStdString ();
		int value = args[3].toInt ();
		if (value < 0 || value > 2)
			return "ERR usage: value must be 0, 1, or 2\n";

		bool found = false;
		for (int i = 0; i < kNumCategories; i++)
		{
			if (strcasecmp (catName.c_str (), kLogCategories[i].name) == 0)
			{
				Preference<uint8> pref (kLogCategories[i].key);
				pref = (uint8) value;
				found = true;
				break;
			}
		}

		if (!found)
			return "ERR usage: unknown category '" + catName + "'\n";
		return "OK\n";
	}

	if (sub == "dump")
	{
		LogDump ();
		return "OK\n";
	}

	if (sub == "clear")
	{
		LogClear ();
		return "OK\n";
	}

	return "ERR usage: log <list|set|dump|clear>\n";
}

// ============================================================================
// RcCmd_Gremlin — WorkerRaw (handler creates own stoppers per sub-command)
// ============================================================================

std::string RcCmd_Gremlin (const QStringList& args)
{
	if (args.size () < 2)
		return "ERR usage: gremlin <new|status|suspend|step|resume|stop>\n";

	QString sub = args[1].toLower ();

	if (sub == "status")
	{
		if (!Hordes::IsOn ())
			return "OK gremlin off\n";

		unsigned short number;
		UInt32 step, until;
		Hordes::Status (&number, &step, &until);

		char buf[128];
		snprintf (buf, sizeof (buf), "OK gremlin running number=%d step=%u until=%u\n",
			(int) number, (unsigned) step, (unsigned) until);
		return std::string (buf);
	}

	if (sub == "new")
	{
		if (args.size () < 4)
			return "ERR usage: gremlin new <seed> <events>\n";
		if (Hordes::IsOn ())
			return "ERR transient: gremlin already running\n";

		int seed = args[2].toInt ();
		int events = args[3].toInt ();
		if (events < 1)
			return "ERR usage: events must be > 0\n";

		EmSessionStopper stopper (gSession, kStopOnSysCall, 10000);
		if (!stopper.Stopped ())
			return "ERR transient: could not stop session for gremlin\n";

		GremlinInfo info;
		info.fNumber = seed;
		info.fSteps = events;
		info.fFinal = events;
		info.fSaveFrequency = 10000;
		info.fAppList = gGremlinAppList;

		Hordes::NewGremlin (info);

		char buf[80];
		snprintf (buf, sizeof (buf), "OK gremlin started seed=%d events=%d\n", seed, events);
		return std::string (buf);
	}

	if (sub == "suspend")
	{
		if (!Hordes::IsOn ())
			return "ERR transient: no gremlin running\n";
		if (!Hordes::CanSuspend ())
			return "ERR transient: cannot suspend now\n";
		Hordes::Suspend ();
		return "OK\n";
	}

	if (sub == "step")
	{
		if (!Hordes::IsOn ())
			return "ERR transient: no gremlin running\n";
		if (!Hordes::CanStep ())
			return "ERR transient: cannot step now\n";
		Hordes::Step ();
		return "OK\n";
	}

	if (sub == "resume")
	{
		if (!Hordes::IsOn ())
			return "ERR transient: no gremlin running\n";
		if (!Hordes::CanResume ())
			return "ERR transient: cannot resume now\n";
		Hordes::Resume ();
		return "OK\n";
	}

	if (sub == "stop")
	{
		if (!Hordes::IsOn ())
			return "ERR transient: no gremlin running\n";
		EmSessionStopper stopper (gSession, kStopNow);
		Hordes::Stop ();
		return "OK\n";
	}

	return "ERR usage: gremlin <new|status|suspend|step|resume|stop>\n";
}

// ============================================================================
// RcCmd_Check — Immediate (preferences only)
// ============================================================================

std::string RcCmd_Check (const QStringList& args)
{
	if (args.size () < 2)
		return "ERR usage: check <list|set|set-all|clearall>\n";

	static const struct {
		const char* name;
		PrefKeyType key;
	} kReportFlags[] = {
		{ "FreeChunkAccess",         kPrefKeyReportFreeChunkAccess         },
		{ "HardwareRegisterAccess",  kPrefKeyReportHardwareRegisterAccess  },
		{ "LowMemoryAccess",         kPrefKeyReportLowMemoryAccess         },
		{ "LowStackAccess",          kPrefKeyReportLowStackAccess          },
		{ "MemMgrDataAccess",        kPrefKeyReportMemMgrDataAccess        },
		{ "MemMgrLeaks",             kPrefKeyReportMemMgrLeaks             },
		{ "MemMgrSemaphore",         kPrefKeyReportMemMgrSemaphore         },
		{ "OffscreenObject",         kPrefKeyReportOffscreenObject         },
		{ "OverlayErrors",           kPrefKeyReportOverlayErrors           },
		{ "ProscribedFunction",      kPrefKeyReportProscribedFunction      },
		{ "ROMAccess",               kPrefKeyReportROMAccess               },
		{ "ScreenAccess",            kPrefKeyReportScreenAccess            },
		{ "SizelessObject",          kPrefKeyReportSizelessObject          },
		{ "StackAlmostOverflow",     kPrefKeyReportStackAlmostOverflow     },
		{ "StrictIntlChecks",        kPrefKeyReportStrictIntlChecks        },
		{ "SystemGlobalAccess",      kPrefKeyReportSystemGlobalAccess      },
		{ "UIMgrDataAccess",         kPrefKeyReportUIMgrDataAccess         },
		{ "UnlockedChunkAccess",     kPrefKeyReportUnlockedChunkAccess     },
	};
	const int kNumFlags = sizeof (kReportFlags) / sizeof (kReportFlags[0]);

	QString sub = args[1].toLower ();

	if (sub == "list")
	{
		std::string result = "OK check list\n";
		for (int i = 0; i < kNumFlags; i++)
		{
			Preference<bool> pref (kReportFlags[i].key);
			char buf[64];
			snprintf (buf, sizeof (buf), " %s=%s\n",
				kReportFlags[i].name, *pref ? "on" : "off");
			result += buf;
		}
		result += ".\n";
		return result;
	}

	if (sub == "set-all")
	{
		if (args.size () < 3)
			return "ERR usage: check set-all <on|off>\n";
		QString val = args[2].toLower ();
		if (val != "on" && val != "off")
			return "ERR usage: check set-all <on|off>\n";

		bool enable = (val == "on");
		for (int i = 0; i < kNumFlags; i++)
		{
			Preference<bool> pref (kReportFlags[i].key);
			pref = enable;
		}
		return "OK\n";
	}

	if (sub == "set")
	{
		if (args.size () < 4)
			return "ERR usage: check set <flag> <on|off>\n";

		std::string flagName = args[2].toStdString ();
		QString val = args[3].toLower ();
		if (val != "on" && val != "off")
			return "ERR usage: check set <flag> <on|off>\n";

		bool enable = (val == "on");
		bool found = false;
		for (int i = 0; i < kNumFlags; i++)
		{
			if (strcasecmp (flagName.c_str (), kReportFlags[i].name) == 0)
			{
				Preference<bool> pref (kReportFlags[i].key);
				pref = enable;
				found = true;
				break;
			}
		}

		if (!found)
			return "ERR usage: unknown flag '" + flagName + "'\n";
		return "OK\n";
	}

	if (sub == "clearall")
	{
		for (int i = 0; i < kNumFlags; i++)
		{
			Preference<bool> pref (kReportFlags[i].key);
			pref = false;
		}
		return "OK\n";
	}

	return "ERR usage: check <list|set|set-all|clearall>\n";
}

// ============================================================================
// RcCmd_ErrorHandling — Immediate (preferences only)
// ============================================================================

std::string RcCmd_ErrorHandling (const QStringList& args)
{
	if (args.size () < 2)
		return "ERR usage: errorhandling <get|list|set>\n";

	static const struct {
		const char* name;
		PrefKeyType key;
	} kSettings[] = {
		{ "WarningOff", kPrefKeyWarningOff },
		{ "ErrorOff",   kPrefKeyErrorOff   },
		{ "WarningOn",  kPrefKeyWarningOn  },
		{ "ErrorOn",    kPrefKeyErrorOn    },
	};
	const int kNumSettings = sizeof (kSettings) / sizeof (kSettings[0]);

	static const struct {
		const char* name;
		EmErrorHandlingOption value;
	} kOptions[] = {
		{ "show",     kShow     },
		{ "continue", kContinue },
		{ "quit",     kQuit     },
		{ "switch",   kSwitch   },
	};
	const int kNumOptions = sizeof (kOptions) / sizeof (kOptions[0]);

	auto optionName = [&](EmErrorHandlingOption opt) -> const char* {
		for (int i = 0; i < kNumOptions; i++)
			if (kOptions[i].value == opt) return kOptions[i].name;
		return "unknown";
	};

	QString sub = args[1].toLower ();

	if (sub == "get" || sub == "list")
	{
		std::string result = "OK errorhandling\n";
		for (int i = 0; i < kNumSettings; i++)
		{
			Preference<EmErrorHandlingOption> pref (kSettings[i].key);
			char buf[64];
			snprintf (buf, sizeof (buf), " %s=%s\n",
				kSettings[i].name, optionName (*pref));
			result += buf;
		}
		result += ".\n";
		return result;
	}

	if (sub == "set")
	{
		if (args.size () < 4)
			return "ERR usage: errorhandling set <setting> <show|continue|quit|switch>\n";

		std::string settingName = args[2].toStdString ();
		std::string optName = args[3].toLower ().toStdString ();

		bool foundSetting = false;
		PrefKeyType key = kPrefKeyWarningOff;
		for (int i = 0; i < kNumSettings; i++)
		{
			if (strcasecmp (settingName.c_str (), kSettings[i].name) == 0)
			{
				key = kSettings[i].key;
				foundSetting = true;
				break;
			}
		}
		if (!foundSetting)
			return "ERR usage: unknown setting '" + settingName + "'\n";

		bool foundOption = false;
		EmErrorHandlingOption opt = kShow;
		for (int i = 0; i < kNumOptions; i++)
		{
			if (optName == kOptions[i].name)
			{
				opt = kOptions[i].value;
				foundOption = true;
				break;
			}
		}
		if (!foundOption)
			return "ERR usage: unknown option '" + optName + "' (use show/continue/quit/switch)\n";

		Preference<EmErrorHandlingOption> pref (key);
		pref = opt;
		return "OK\n";
	}

	return "ERR usage: errorhandling <get|list|set>\n";
}
