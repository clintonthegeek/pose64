/* -*- mode: C++; tab-width: 4 -*- */
/* ReControl: input command handlers (tap, tap-id, pen, key, type, button, menu, run). */

#include "EmCommon.h"

// Undefine Palm OS macros that conflict with Qt
#undef daysInYear
#undef monthsInYear

#include "ReControl.h"
#include "EmSession.h"
#include "Skins.h"
#include "EmLowMem.h"
#include "Hardware/EmMemory.h"
#include "PalmFormReader.h"
#include "CPUWorkerThread.h"

#include <QThread>
#include <QStringList>
#include <string>
#include <cstring>

// ============================================================================
// Argument validators — run on the MAIN thread before a WorkerDirect command
// is queued, so malformed input returns "ERR usage" immediately instead of a
// swallowed "OK".  Each returns "" when the args are well-formed.  They mirror
// the arg-count/format checks in the handlers below, and additionally reject
// non-numeric coordinates that QString::toInt() would otherwise read as 0.
// ============================================================================

static bool PrvArgIsInt (const QString& s)
{
	bool ok = false;
	s.toInt (&ok);
	return ok;
}

std::string RcValidate_Tap (const QStringList& a)
{
	if (a.size () != 3 || !PrvArgIsInt (a[1]) || !PrvArgIsInt (a[2]))
		return "ERR usage: tap <x> <y>\n";
	return "";
}

std::string RcValidate_Pen (const QStringList& a)
{
	if (a.size () != 4)
		return "ERR usage: pen <down|up> <x> <y>\n";
	QString dir = a[1].toLower ();
	if ((dir != "down" && dir != "up") || !PrvArgIsInt (a[2]) || !PrvArgIsInt (a[3]))
		return "ERR usage: pen <down|up> <x> <y>\n";
	return "";
}

std::string RcValidate_Key (const QStringList& a)
{
	if (a.size () != 2 || !PrvArgIsInt (a[1]))
		return "ERR usage: key <charcode>\n";
	return "";
}

std::string RcValidate_Type (const QStringList& a)
{
	if (a.size () < 2)
		return "ERR usage: type <text>\n";
	return "";
}

std::string RcValidate_Button (const QStringList& a)
{
	if (a.size () != 3)
		return "ERR usage: button <name> <down|up|tap>\n";

	static const char* const kNames[] = {
		"power", "up", "down", "app1", "app2", "app3", "app4",
		"cradle", "contrast"};
	QString name = a[1].toLower ();
	bool known = false;
	for (const char* n : kNames)
		if (name == n) { known = true; break; }
	if (!known)
		return "ERR usage: unknown button '" + name.toStdString () + "'\n";

	QString action = a[2].toLower ();
	if (action != "down" && action != "up" && action != "tap")
		return "ERR usage: button <name> <down|up|tap>\n";
	return "";
}

// ============================================================================
// Honest input contract (Phase 2, recovery-plan 2.4 + Q-ACK).  tap/tap-id/
// pen/key/type post toward the guest and then block up to kDeliveryTimeoutMs
// on the per-queue delivery counter (incremented by PuppetString once the
// event reaches the Palm OS event queue).  "OK delivered" means the guest
// has it; a refused post is surfaced, never swallowed as OK.
// ============================================================================

// Map a refused post to the honest protocol error (recovery-plan 2.4).
static std::string PrvInputDropError (EmPostInputResult r)
{
	switch (r)
	{
		case kInputDroppedGremlins:
			return "ERR busy: gremlin running\n";
		case kInputDroppedReplay:
			return "ERR busy: event playback active\n";
		case kInputDroppedMinimize:
			return "ERR busy: minimization active\n";
		case kInputDroppedDuplicate:
			return "ERR duplicate: pen already down at that point\n";
		default:
			return "ERR transient: event not posted\n";
	}
}

static const int kDeliveryTimeoutMs = 2000;
static const char* kPendingError =
	"ERR pending: queued, not delivered within 2000ms\n";

// ============================================================================
// RcCmd_Tap — WorkerRaw (posts pen down + up, waits for delivery)
// ============================================================================

std::string RcCmd_Tap (const QStringList& args)
{
	if (args.size () != 3)
		return "ERR usage: tap <x> <y>\n";

	int x = args[1].toInt ();
	int y = args[2].toInt ();

	EmPenEvent penDown (EmPoint (x, y), true);
	EmPostInputResult r = gSession->PostPenEvent (penDown);
	if (r != kInputPosted)
		return ::PrvInputDropError (r);

	EmPenEvent penUp (EmPoint (-1, -1), false);
	r = gSession->PostPenEvent (penUp);
	if (r != kInputPosted)
		return ::PrvInputDropError (r);

	uint64 target = gSession->PenEventsPosted ();
	if (gSession->WaitForPenDelivery (target, kDeliveryTimeoutMs))
		return "OK delivered\n";
	return kPendingError;
}

// ============================================================================
// RcCmd_TapId — WorkerRaw (handler creates its own stopper for the guest-
// memory lookup; the stopper is released before waiting for delivery)
// ============================================================================

std::string RcCmd_TapId (const QStringList& args)
{
	if (args.size () != 2)
		return "ERR usage: tap-id <object_id>\n";

	int targetId = args[1].toInt ();
	int cx = -1, cy = -1;

	// Stop the CPU only while reading guest memory.  This scope MUST close
	// (releasing the stopper) before WaitForPenDelivery below, or delivery
	// could never happen and we would always time out.
	{
	EmSessionStopper stopper (gSession, kStopOnCycle, 5000);
	if (!stopper.Stopped ())
		return "ERR timeout: CPU did not reach a cycle boundary within 5000ms. "
		       "Recovery: dismiss any dialog (dialog respond) or palm_reset.\n";

	CEnableFullAccess munge;

	emuptr formPtr = EmLowMem_GetGlobal (uiGlobalsCommon.currentForm);
	if (formPtr == 0)
		return "ERR transient: no active form\n";

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
				objId = EmMemGet16 (dataPtr + 0);
				bx = (int16) EmMemGet16 (dataPtr + 2);
				by = (int16) EmMemGet16 (dataPtr + 4);
				bw = 0;
				bh = 0;
				break;

			case kFrmGadgetObj:
				objId = EmMemGet16 (dataPtr + 0);
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
				continue;
		}

		if ((int) objId != targetId) continue;

		cx = winX + bx + bw / 2;
		cy = winY + by + bh / 2;
		break;
	}
	}  // end stopper scope — CPU resumes here, so delivery can occur

	if (cx < 0)
		return "ERR usage: object " + std::to_string (targetId) + " not found\n";

	EmPenEvent penDown (EmPoint (cx, cy), true);
	EmPostInputResult r = gSession->PostPenEvent (penDown);
	if (r != kInputPosted)
		return ::PrvInputDropError (r);
	EmPenEvent penUp (EmPoint (-1, -1), false);
	r = gSession->PostPenEvent (penUp);
	if (r != kInputPosted)
		return ::PrvInputDropError (r);

	uint64 target = gSession->PenEventsPosted ();
	if (gSession->WaitForPenDelivery (target, kDeliveryTimeoutMs))
		return "OK delivered " + std::to_string (cx) + " " + std::to_string (cy) + "\n";
	return kPendingError;
}

// ============================================================================
// RcCmd_Pen — WorkerDirect (single pen event)
// ============================================================================

std::string RcCmd_Pen (const QStringList& args)
{
	if (args.size () != 4)
		return "ERR usage: pen <down|up> <x> <y>\n";

	QString state = args[1].toLower ();
	if (state != "down" && state != "up")
		return "ERR usage: pen <down|up> <x> <y>\n";

	bool isDown = (state == "down");
	int x = args[2].toInt ();
	int y = args[3].toInt ();

	EmPenEvent penEvent (EmPoint (x, y), isDown);
	EmPostInputResult r = gSession->PostPenEvent (penEvent);
	if (r != kInputPosted)
		return ::PrvInputDropError (r);

	uint64 target = gSession->PenEventsPosted ();
	if (gSession->WaitForPenDelivery (target, kDeliveryTimeoutMs))
		return "OK delivered\n";
	return kPendingError;
}

// ============================================================================
// RcCmd_Key — WorkerRaw (single key event, waits for delivery)
// ============================================================================

std::string RcCmd_Key (const QStringList& args)
{
	if (args.size () != 2)
		return "ERR usage: key <charcode>\n";

	int charcode = args[1].toInt ();
	EmKeyEvent keyEvent (charcode);
	EmPostInputResult r = gSession->PostKeyEvent (keyEvent);
	if (r != kInputPosted)
		return ::PrvInputDropError (r);

	uint64 target = gSession->KeyEventsPosted ();
	if (gSession->WaitForKeyDelivery (target, kDeliveryTimeoutMs))
		return "OK delivered\n";
	return kPendingError;
}

// ============================================================================
// RcCmd_Type — WorkerRaw (multi-key sequence, waits for delivery)
// ============================================================================

std::string RcCmd_Type (const QStringList& args)
{
	if (args.size () < 2)
		return "ERR usage: type <text>\n";

	QString text;
	for (int i = 1; i < args.size (); i++)
	{
		if (i > 1) text += ' ';
		text += args[i];
	}
	QByteArray latin1 = text.toLatin1 ();

	for (int i = 0; i < latin1.size (); i++)
	{
		unsigned char ch = (unsigned char) latin1[i];
		EmKeyEvent keyEvent (ch);
		EmPostInputResult r = gSession->PostKeyEvent (keyEvent);
		if (r != kInputPosted)
			return ::PrvInputDropError (r);
	}

	uint64 target = gSession->KeyEventsPosted ();
	if (gSession->WaitForKeyDelivery (target, kDeliveryTimeoutMs))
		return "OK delivered\n";
	return kPendingError;
}

// ============================================================================
// RcCmd_Button — WorkerRaw (skin button press; queued, honest drop on refusal)
// ============================================================================

std::string RcCmd_Button (const QStringList& args)
{
	if (args.size () != 3)
		return "ERR usage: button <name> <down|up|tap>\n";

	QString name = args[1].toLower ();
	QString action = args[2].toLower ();

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
	else return "ERR usage: unknown button '" + name.toStdString () + "'\n";

	if (action != "down" && action != "up" && action != "tap")
		return "ERR usage: button <name> <down|up|tap>\n";

	if (action == "down")
	{
		if (!gSession->SetButtonDown (button))
			return "ERR busy: gremlin or playback active\n";
	}
	else if (action == "up")
	{
		gSession->SetButtonUp (button);
	}
	else // tap
	{
		if (!gSession->SetButtonTap (button))
			return "ERR busy: gremlin or playback active\n";
	}

	return "OK\n";
}

// ============================================================================
// RcCmd_Menu — Custom (two-phase async retry with QTimer)
// ============================================================================
// Arg parsing is here; DoMenuLookup stays as a method on ReControlSession
// because it uses QTimer::singleShot and captures this.

void RcCmd_Menu (ReControlSession* session, const QStringList& args)
{
	if (args.size () < 3)
	{
		session->SendErr ("usage", "menu <menutitle> <itemtitle>");
		return;
	}
	if (!gSession)
	{
		session->SendErr ("transient", "no session");
		return;
	}

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

	if (parsed.size () < 2)
	{
		session->SendErr ("usage", "menu <menutitle> <itemtitle>");
		return;
	}

	std::string menuTitle = parsed[0].toStdString ();
	std::string itemTitle = parsed[1].toStdString ();

	session->DoMenuLookup (menuTitle, itemTitle, false);
}

// ============================================================================
// Run sub-command helpers (static, used only by RcCmd_Run)
// ============================================================================

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

static std::string RunSubCommands (const QStringList& subcmds)
{
	int cmdNum = 0;

	for (int i = 0; i < subcmds.size (); i++)
	{
		QString sub = subcmds[i].trimmed ();
		if (sub.isEmpty ()) continue;
		cmdNum++;

		QStringList words = sub.split (' ', Qt::SkipEmptyParts);
		if (!words.isEmpty () && words[0].toLower () == "repeat")
		{
			if (words.size () < 2)
				return "command " + std::to_string (cmdNum) + " 'repeat': requires count";

			int count = words[1].toInt ();
			if (count < 1 || count > 10000)
				return "command " + std::to_string (cmdNum) + " 'repeat': count must be 1-10000";

			QString body;
			bool foundOpen = false;
			bool foundClose = false;

			for (int w = 2; w < words.size (); w++)
			{
				QString word = words[w];
				if (word.startsWith ('{'))
				{
					foundOpen = true;
					word = word.mid (1);
				}
				if (word.endsWith ('}'))
				{
					foundClose = true;
					word.chop (1);
				}
				if (!body.isEmpty ()) body += ' ';
				body += word;
			}

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

			QStringList bodyParts = body.split (';');
			for (int r = 0; r < count; r++)
			{
				std::string err = RunSubCommands (bodyParts);
				if (!err.empty ())
					return "command " + std::to_string (cmdNum) + " repeat iteration " + std::to_string (r + 1) + ": " + err;
			}
			continue;
		}

		std::string err = RunOneSub (sub);
		if (!err.empty ())
			return "command " + std::to_string (cmdNum) + " '" + sub.toStdString () + "': " + err;
	}

	return "";
}

// ============================================================================
// RcCmd_Run — Custom (batch/script command, uses QueueWorkResult)
// ============================================================================

void RcCmd_Run (ReControlSession* session, const QStringList& args)
{
	if (args.size () < 2)
	{
		session->SendErr ("usage", "run <cmd1>; <cmd2>; ...");
		return;
	}
	if (!gSession)
	{
		session->SendErr ("transient", "no session");
		return;
	}

	// Rejoin everything after "run" and split on semicolons
	QString script;
	for (int i = 1; i < args.size (); i++)
	{
		if (i > 1) script += ' ';
		script += args[i];
	}

	QStringList subcmds = script.split (';');

	session->QueueWorkResult ([subcmds]() -> std::string {
		if (!gSession) return "ERR transient: no session\n";

		std::string err = RunSubCommands (subcmds);
		if (!err.empty ())
			return "ERR transient: " + err + "\n";

		int count = 0;
		for (const QString& s : subcmds)
			if (!s.trimmed ().isEmpty ()) count++;
		return "OK " + std::to_string (count) + " commands\n";
	});
}
