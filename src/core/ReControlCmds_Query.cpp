/* -*- mode: C++; tab-width: 4 -*- */
/* ReControl: query command handlers (screenshot, screen-hash, ui, peek, poke, regs, backtrace). */

#include "EmCommon.h"

// Undefine Palm OS macros that conflict with Qt
#undef daysInYear
#undef monthsInYear

#include "ReControl.h"
#include "EmSession.h"
#include "EmScreen.h"
#include "EmLowMem.h"
#include "Hardware/EmMemory.h"
#include "PalmFormReader.h"
#include "EmPalmOS.h"
#include "UAE.h"

#include <QImage>
#include <QPainter>
#include <QFont>
#include <QStringList>

#include <string>
#include <vector>
#include <cstring>
#include <zlib.h>

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

// ---------------------------------------------------------------------------
// Address parsing — shared with ReControlCmds_Debug.cpp
// ---------------------------------------------------------------------------

bool ParseAddress (const std::string& addrStr, emuptr& outAddr)
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

// ---------------------------------------------------------------------------
// Helper: format m68k register dump string
// ---------------------------------------------------------------------------

static std::string PrvFormatRegs (void)
{
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
}

// ============================================================================
// RcCmd_Screenshot — WorkerRaw (creates own kStopNow stopper)
// ============================================================================

std::string RcCmd_Screenshot (const QStringList& args)
{
	if (args.size () < 2)
		return "ERR usage: screenshot <filepath> [scale=N] [grid] [annotate] [crosshair=X,Y]\n";

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

	// Construct fonts (QFont is thread-safe in Qt6)
	QFont gridFont;
	gridFont.setPixelSize (std::max (8, scaleFactor * 3));
	QFont crossFont;
	crossFont.setPixelSize (std::max (10, scaleFactor * 3));
	crossFont.setBold (true);

	EmSessionStopper stopper (gSession, kStopNow);
	if (!stopper.Stopped ())
		return "ERR transient: could not stop session\n";

	EmScreen::InvalidateAll ();

	EmScreenUpdateInfo info;
	info.fScreenLow  = 0;
	info.fScreenHigh = 0xFFFFFFFF;
	if (!EmScreen::GetBits (info))
		return "ERR transient: could not capture screen\n";

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
		if (!raw.save (path, "PNG"))
			return "ERR transient: could not write " + path.toStdString () + "\n";
		return "OK " + hash + " " + std::to_string (w) + " " + std::to_string (h) + "\n";
	}

	// ── Scale ──────────────────────────────────────────────────
	int sw = w * scaleFactor;
	int sh = h * scaleFactor;
	int s  = scaleFactor;

	QImage img = raw.scaled (sw, sh, Qt::IgnoreAspectRatio, Qt::FastTransformation)
	                 .convertToFormat (QImage::Format_RGB32);

	QPainter painter (&img);
	painter.setRenderHint (QPainter::Antialiasing, false);

	// ── Grid ───────────────────────────────────────────────────
	if (drawGrid)
	{
		QPen gridPen (QColor (255, 255, 255, 60), 1);
		painter.setPen (gridPen);
		for (int px = 10; px < w; px += 10)
			painter.drawLine (px * s, 0, px * s, sh);
		for (int py = 10; py < h; py += 10)
			painter.drawLine (0, py * s, sw, py * s);

		painter.setFont (gridFont);
		int textBase = gridFont.pixelSize ();

		for (int px = 0; px < w; px += 20)
		{
			QString num = QString::number (px);
			int tx = px * s + 2;

			painter.setPen (QColor (0, 0, 0, 200));
			painter.drawText (tx - 1, textBase, num);
			painter.drawText (tx + 1, textBase, num);
			painter.drawText (tx, textBase - 1, num);
			painter.drawText (tx, textBase + 1, num);
			painter.setPen (QColor (255, 255, 0, 220));
			painter.drawText (tx, textBase, num);
		}
		for (int py = 20; py < h; py += 20)
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
		std::vector<PalmObjInfo> objs;
		{
			CEnableFullAccess munge;
			objs = PalmFormReader_GetObjectBounds ();
		}

		painter.setFont (gridFont);

		for (const auto& obj : objs)
		{
			QColor color;
			switch (obj.type)
			{
				case kFrmControlObj:   color = QColor (80, 140, 255, 140); break;
				case kFrmFieldObj:     color = QColor (80, 220, 80, 140);  break;
				case kFrmListObj:      color = QColor (255, 160, 40, 140); break;
				case kFrmGadgetObj:    color = QColor (180, 80, 220, 140); break;
				case kFrmScrollBarObj: color = QColor (220, 220, 40, 140); break;
				case kFrmTitleObj:     color = QColor (40, 200, 200, 140); break;
				case kFrmLabelObj:     color = QColor (200, 200, 200, 100); break;
				default:               color = QColor (255, 255, 255, 100); break;
			}

			int rx = obj.screenX * s;
			int ry = obj.screenY * s;
			int rw = obj.w * s;
			int rh = obj.h * s;

			if (rw > 0 && rh > 0)
			{
				painter.fillRect (rx, ry, rw, rh, color);
				QPen borderPen (color.darker (150), std::max (1, s / 2));
				painter.setPen (borderPen);
				painter.drawRect (rx, ry, rw, rh);
			}

			QString idText = QString::number (obj.id);
			int labelX = rx + 2;
			int labelY = ry - 2;
			if (labelY < gridFont.pixelSize ())
				labelY = ry + gridFont.pixelSize () + 2;

			painter.setPen (QColor (0, 0, 0, 220));
			painter.drawText (labelX - 1, labelY, idText);
			painter.drawText (labelX + 1, labelY, idText);
			painter.drawText (labelX, labelY - 1, idText);
			painter.drawText (labelX, labelY + 1, idText);
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
		painter.drawLine (cx, 0, cx, sh);
		painter.drawLine (0, cy, sw, cy);

		painter.setFont (crossFont);

		QString coordText = QString ("(%1,%2)").arg (crossX).arg (crossY);
		int tx = cx + s;
		int ty = cy - s;
		if (tx + s * 20 > sw) tx = std::max (0, cx - s * 20);
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
}

// ============================================================================
// RcCmd_ScreenHash — WorkerRaw (creates own kStopNow stopper)
// ============================================================================

std::string RcCmd_ScreenHash (const QStringList& args)
{
	EmSessionStopper stopper (gSession, kStopNow);
	if (!stopper.Stopped ())
		return "ERR transient: could not stop session\n";

	EmScreen::InvalidateAll ();

	EmScreenUpdateInfo info;
	info.fScreenLow  = 0;
	info.fScreenHigh = 0xFFFFFFFF;
	if (!EmScreen::GetBits (info))
		return "ERR transient: could not capture screen\n";

	EmPoint size = info.fImage.GetSize ();
	int w = size.fX;
	int h = size.fY;

	info.fImage.ConvertToFormat (kPixMapFormat24RGB);

	std::string hash = ComputeScreenHash (info, w, h);
	return "OK " + hash + " " + std::to_string (w) + " " + std::to_string (h) + "\n";
}

// ============================================================================
// RcCmd_UI — WorkerCycle (stopper created by dispatch loop)
// ============================================================================

std::string RcCmd_UI (const QStringList& args)
{
	CEnableFullAccess munge;
	return PalmFormReader_ReadActiveForm ();
}

// ============================================================================
// RcCmd_Peek — Adaptive (dispatch loop handles blocked_on_ui vs worker)
// ============================================================================

std::string RcCmd_Peek (const QStringList& args)
{
	if (args.size () != 3)
		return "ERR usage: peek <addr> <nbytes>\n";

	std::string addrStr = args[1].toStdString ();
	int nbytes = args[2].toInt ();
	if (nbytes < 1 || nbytes > 256)
		return "ERR usage: nbytes must be 1-256\n";

	CEnableFullAccess munge;

	emuptr addr;
	if (!ParseAddress (addrStr, addr))
		return "ERR usage: invalid address '" + addrStr + "'\n";

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
}

// ============================================================================
// RcCmd_Poke — Adaptive (dispatch loop handles blocked_on_ui vs worker)
// ============================================================================

std::string RcCmd_Poke (const QStringList& args)
{
	if (args.size () != 4)
		return "ERR usage: poke <addr> <nbytes> <hexdata>\n";

	std::string addrStr = args[1].toStdString ();
	int nbytes = args[2].toInt ();
	std::string hexdata = args[3].toStdString ();

	if (nbytes < 1 || nbytes > 256)
		return "ERR usage: nbytes must be 1-256\n";
	if ((int) hexdata.size () != nbytes * 2)
		return "ERR usage: hexdata length must be nbytes*2\n";

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
		if (h < 0 || l < 0)
			return "ERR usage: invalid hex in data\n";
		data.push_back ((uint8) ((h << 4) | l));
	}

	CEnableFullAccess munge;

	emuptr addr;
	if (!ParseAddress (addrStr, addr))
		return "ERR usage: invalid address '" + addrStr + "'\n";

	for (size_t i = 0; i < data.size (); i++)
		EmMemPut8 (addr + i, data[i]);

	return "OK\n";
}

// ============================================================================
// RcCmd_Regs — Adaptive (dispatch loop handles blocked_on_ui vs worker)
// ============================================================================

std::string RcCmd_Regs (const QStringList& args)
{
	return PrvFormatRegs ();
}

// ============================================================================
// RcCmd_Backtrace — Adaptive (dispatch loop handles blocked_on_ui vs worker)
// ============================================================================

std::string RcCmd_Backtrace (const QStringList& args)
{
	CEnableFullAccess munge;
	EmStackFrameList frameList;
	EmPalmOS::GenerateStackCrawl (frameList);

	std::string result = "OK backtrace\n";
	for (size_t i = 0; i < frameList.size (); i++)
	{
		char buf[80];
		snprintf (buf, sizeof (buf), " #%zu PC=%08X A6=%08X\n",
			i,
			(unsigned) frameList[i].fAddressInFunction,
			(unsigned) frameList[i].fA6);
		result += buf;
	}
	result += ".\n";
	return result;
}
