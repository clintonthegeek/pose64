/* -*- mode: C++; tab-width: 4 -*- */
/* Qt PORT v2: Qt-based EmWindow implementation.
 *
 * Threading model (matches original FLTK POSE):
 *   UI Thread  — Qt event loop, QWidget painting, mouse/key/menu events
 *   CPU Thread — omnithread, runs PalmOS CPU emulation
 *
 * All Host* methods run on the UI thread (called from HandleIdle→PaintScreen).
 * No bridge thread needed.
 */

#include "EmCommon.h"
#include "EmWindowQt.h"
#include "EmScreen.h"
#include "EmMenus.h"
#include "EmApplication.h"
#include "EmDocument.h"
#include "EmSession.h"

// Undefine Palm OS macros that conflict with Qt
#undef daysInYear
#undef monthsInYear

#include <QMouseEvent>
#include <QKeyEvent>
#include <QContextMenuEvent>
#include <QCloseEvent>
#include <QPainter>
#include <QPen>
#include <QApplication>
#include <QScreen>
#include <QMenu>

#include <cstdio>

using namespace std;

const int kDefaultWidth = 220;
const int kDefaultHeight = 330;

EmWindowQt* gHostWindow;

// ---------------------------------------------------------------------------
//		EmWindow::NewWindow
// ---------------------------------------------------------------------------
// On Unix, we create one and only one window at application startup.
// This method (called by the document) doesn't need to create a window.

EmWindow* EmWindow::NewWindow (void)
{
	EmAssert (gHostWindow != NULL);
	return NULL;
}


#pragma mark -

// ---------------------------------------------------------------------------
//		EmWindowQt::EmWindowQt
// ---------------------------------------------------------------------------

EmWindowQt::EmWindowQt () :
	QWidget (nullptr),
	EmWindow (),
	fSkinValid (false),
	fMouseX (0),
	fMouseY (0),
	fButtonFrameVisible (false),
	fLEDVisible (false)
{
	EmAssert (gHostWindow == NULL);
	gHostWindow = this;

	setWindowTitle ("QtPOSE - Palm OS Emulator");
	resize (kDefaultWidth, kDefaultHeight);

	// Ensure the user can't freely resize this window.
	setFixedSize (kDefaultWidth, kDefaultHeight);

	// Enable keyboard focus
	setFocusPolicy (Qt::StrongFocus);
}


// ---------------------------------------------------------------------------
//		EmWindowQt::~EmWindowQt
// ---------------------------------------------------------------------------

EmWindowQt::~EmWindowQt ()
{
	this->PreDestroy ();

	EmAssert (gHostWindow == this);
	gHostWindow = NULL;
}


// ---------------------------------------------------------------------------
//		EmWindowQt::closeEvent
// ---------------------------------------------------------------------------
// Handle window close (WM_DELETE_WINDOW equivalent).

void EmWindowQt::closeEvent (QCloseEvent* event)
{
	// If quit is already requested, force-exit on second close attempt.
	if (gApplication && gApplication->GetTimeToQuit ())
	{
		event->accept ();
		QApplication::quit ();
		return;
	}

	// Try the clean quit path.
	event->ignore ();

	if (gApplication)
	{
		gApplication->HandleCommand (kCommandQuit);

		// If quit was accepted, trigger exit immediately.
		if (gApplication->GetTimeToQuit ())
		{
			QApplication::quit ();
		}
	}
	else
	{
		// No application — just quit.
		QApplication::quit ();
	}
}


// ---------------------------------------------------------------------------
//		EmWindowQt::paintEvent
// ---------------------------------------------------------------------------

void EmWindowQt::paintEvent (QPaintEvent*)
{
	QPainter painter (this);

	if (gDocument)
	{
		// Draw skin (device case) as background, scaled to fill the widget.
		// On HiDPI displays the skin image pixels != logical pixels, so
		// we must scale rather than drawing at (0,0) 1:1.
		if (!fSkinImage.isNull ())
		{
			painter.drawImage (rect (), fSkinImage);
		}

		// Draw LCD contents on top
		if (!fLCDImage.isNull ())
		{
			painter.drawImage (fLCDRect, fLCDImage);
		}

		// Draw button press highlight
		if (fButtonFrameVisible)
		{
			painter.setPen (QPen (fButtonFrameColor, 2));
			painter.setBrush (Qt::NoBrush);
			painter.drawRect (fButtonFrame);
		}

		// Draw LED indicator
		if (fLEDVisible)
		{
			painter.setPen (Qt::NoPen);
			painter.setBrush (fLEDColor);
			painter.drawEllipse (fLEDRect);
		}
	}
	else
	{
		// No session — show help message
		painter.fillRect (rect (), Qt::lightGray);
		painter.drawText (rect (), Qt::AlignCenter,
						  "Right-click for menu");
	}
}


// ---------------------------------------------------------------------------
//		EmWindowQt mouse events
// ---------------------------------------------------------------------------

void EmWindowQt::mousePressEvent (QMouseEvent* event)
{
	fMouseX = event->pos ().x ();
	fMouseY = event->pos ().y ();

	if (event->button () == Qt::RightButton)
	{
		// Right-click handled by contextMenuEvent
		return;
	}

	if (event->button () == Qt::LeftButton)
	{
		EmPoint pt (event->pos ().x (), event->pos ().y ());
		HandlePenEvent (pt, true);
	}
}

void EmWindowQt::mouseReleaseEvent (QMouseEvent* event)
{
	fMouseX = event->pos ().x ();
	fMouseY = event->pos ().y ();

	if (event->button () == Qt::LeftButton)
	{
		EmPoint pt (event->pos ().x (), event->pos ().y ());
		HandlePenEvent (pt, false);
	}
}

void EmWindowQt::mouseMoveEvent (QMouseEvent* event)
{
	fMouseX = event->pos ().x ();
	fMouseY = event->pos ().y ();

	if (event->buttons () & Qt::LeftButton)
	{
		EmPoint pt (event->pos ().x (), event->pos ().y ());
		HandlePenEvent (pt, true);
	}
}


// ---------------------------------------------------------------------------
//		EmWindowQt::keyPressEvent
// ---------------------------------------------------------------------------
// Map Qt key events to Palm key events.
// Reference: EmWindowFltk::handle (FL_KEYBOARD case)

void EmWindowQt::keyPressEvent (QKeyEvent* event)
{
	if (event->modifiers () & (Qt::AltModifier | Qt::MetaModifier))
	{
		// Reserved for shortcuts
		event->ignore ();
		return;
	}

	if (gDocument == NULL)
	{
		event->ignore ();
		return;
	}

	// Handle special keys that map to Palm buttons
	struct KeyConvert
	{
		int		fQtKey;
		SkinElementType	fButton;
		int		fKey;
	};

	KeyConvert kConvert[] =
	{
		{ Qt::Key_Return,	kElement_None, chrLineFeed },
		{ Qt::Key_Enter,	kElement_None, chrLineFeed },
		{ Qt::Key_Left,		kElement_None, leftArrowChr },
		{ Qt::Key_Right,	kElement_None, rightArrowChr },
		{ Qt::Key_Up,		kElement_None, upArrowChr },
		{ Qt::Key_Down,		kElement_None, downArrowChr },
		{ Qt::Key_F1,		kElement_App1Button, 0 },
		{ Qt::Key_F2,		kElement_App2Button, 0 },
		{ Qt::Key_F3,		kElement_App3Button, 0 },
		{ Qt::Key_F4,		kElement_App4Button, 0 },
		{ Qt::Key_F9,		kElement_PowerButton, 0 },
		{ Qt::Key_PageUp,	kElement_UpButton, 0 },
		{ Qt::Key_PageDown,	kElement_DownButton, 0 },
	};

	int qtKey = event->key ();

	for (size_t ii = 0; ii < sizeof (kConvert) / sizeof (kConvert[0]); ++ii)
	{
		if (qtKey == kConvert[ii].fQtKey)
		{
			if (kConvert[ii].fButton != kElement_None)
			{
				gDocument->HandleButton (kConvert[ii].fButton, true);
				gDocument->HandleButton (kConvert[ii].fButton, false);
				event->accept ();
				return;
			}

			if (kConvert[ii].fKey)
			{
				EmKeyEvent keyEvent (kConvert[ii].fKey);
				gDocument->HandleKey (keyEvent);
				event->accept ();
				return;
			}
		}
	}

	// F10 opens context menu
	if (qtKey == Qt::Key_F10)
	{
		QContextMenuEvent fakeEvent (QContextMenuEvent::Keyboard, QPoint (0, 0));
		contextMenuEvent (&fakeEvent);
		event->accept ();
		return;
	}

	// Handle printable characters
	QString text = event->text ();
	if (!text.isEmpty ())
	{
		int ch = (unsigned char) text.toLatin1 ().at (0);
		if (ch > 0)
		{
			EmKeyEvent keyEvent (ch);
			gDocument->HandleKey (keyEvent);
			event->accept ();
			return;
		}
	}

	event->ignore ();
}


// ---------------------------------------------------------------------------
//		EmWindowQt::keyReleaseEvent
// ---------------------------------------------------------------------------

void EmWindowQt::keyReleaseEvent (QKeyEvent* event)
{
	// POSE doesn't track key-up events separately for most keys.
	// Button presses (F1-F4, etc.) send press+release in keyPressEvent.
	event->ignore ();
}


// ---------------------------------------------------------------------------
//		EmWindowQt::contextMenuEvent
// ---------------------------------------------------------------------------

void EmWindowQt::contextMenuEvent (QContextMenuEvent* event)
{
	EmMenu* popupMenu = MenuFindMenu (kMenuPopupMenuPreferred);
	if (!popupMenu)
		return;

	MenuUpdateMruMenus (*popupMenu);
	MenuUpdateMenuItemStatus (*popupMenu);

	QMenu menu (this);
	buildQMenu (menu, *popupMenu);

	QAction* selected = menu.exec (event->globalPos ());
	if (selected)
	{
		EmCommandID cmd = static_cast<EmCommandID> (selected->data ().toInt ());

		if (gDocument)
			if (gDocument->HandleCommand (cmd))
				return;

		if (gApplication)
			gApplication->HandleCommand (cmd);
	}
}


// ---------------------------------------------------------------------------
//		EmWindowQt::buildQMenu
// ---------------------------------------------------------------------------

void EmWindowQt::buildQMenu (QMenu& qmenu, const EmMenuItemList& items)
{
	for (const auto& item : items)
	{
		if (item.GetIsDivider ())
		{
			qmenu.addSeparator ();
		}
		else if (!item.GetChildren ().empty ())
		{
			QMenu* sub = qmenu.addMenu (
				QString::fromStdString (item.GetTitle ()));
			buildQMenu (*sub, item.GetChildren ());
		}
		else
		{
			QAction* action = qmenu.addAction (
				QString::fromStdString (item.GetTitle ()));
			action->setData (QVariant (static_cast<int> (item.GetCommand ())));
			action->setEnabled (item.GetIsActive ());
			if (item.GetShortcut ())
			{
				action->setShortcut (
					QKeySequence (QString ("Ctrl+%1").arg (
						QChar (item.GetShortcut ()))));
			}
		}
	}
}


// ---------------------------------------------------------------------------
//		EmWindowQt pixel format conversion
// ---------------------------------------------------------------------------
// Converts POSE EmPixMap to QImage.  Thread-safe (no QWidget calls).

QImage EmWindowQt::emPixMapToQImage (const EmPixMap& pixmap)
{
	EmPoint size = pixmap.GetSize ();
	int w = size.fX;
	int h = size.fY;

	if (w <= 0 || h <= 0)
		return QImage ();

	const void* bits = pixmap.GetBits ();
	if (!bits)
		return QImage ();

	EmPixMapFormat fmt = pixmap.GetFormat ();
	EmPixMapRowBytes rowBytes = pixmap.GetRowBytes ();

	switch (fmt)
	{
		case kPixMapFormat32ARGB:
		{
			QImage img (static_cast<const uchar*> (bits), w, h, rowBytes,
						QImage::Format_ARGB32);
			return img.copy ();  // deep copy since bits may be transient
		}

		case kPixMapFormat32RGBA:
		{
			QImage img (w, h, QImage::Format_ARGB32);
			const uint8_t* src = static_cast<const uint8_t*> (bits);
			for (int y = 0; y < h; y++)
			{
				const uint8_t* srcRow = src + y * rowBytes;
				uint32_t* dstRow = reinterpret_cast<uint32_t*> (img.scanLine (y));
				for (int x = 0; x < w; x++)
				{
					uint8_t r = srcRow[x * 4 + 0];
					uint8_t g = srcRow[x * 4 + 1];
					uint8_t b = srcRow[x * 4 + 2];
					uint8_t a = srcRow[x * 4 + 3];
					dstRow[x] = (a << 24) | (r << 16) | (g << 8) | b;
				}
			}
			return img;
		}

		case kPixMapFormat24RGB:
		{
			QImage img (w, h, QImage::Format_RGB32);
			const uint8_t* src = static_cast<const uint8_t*> (bits);
			for (int y = 0; y < h; y++)
			{
				const uint8_t* srcRow = src + y * rowBytes;
				uint32_t* dstRow = reinterpret_cast<uint32_t*> (img.scanLine (y));
				for (int x = 0; x < w; x++)
				{
					uint8_t r = srcRow[x * 3 + 0];
					uint8_t g = srcRow[x * 3 + 1];
					uint8_t b = srcRow[x * 3 + 2];
					dstRow[x] = 0xFF000000 | (r << 16) | (g << 8) | b;
				}
			}
			return img;
		}

		case kPixMapFormat8:
		{
			const RGBList& colors = pixmap.GetColorTable ();
			QImage img (w, h, QImage::Format_ARGB32);
			const uint8_t* src = static_cast<const uint8_t*> (bits);
			for (int y = 0; y < h; y++)
			{
				const uint8_t* srcRow = src + y * rowBytes;
				uint32_t* dstRow = reinterpret_cast<uint32_t*> (img.scanLine (y));
				for (int x = 0; x < w; x++)
				{
					uint8_t idx = srcRow[x];
					if (idx < colors.size ())
					{
						const RGBType& c = colors[idx];
						dstRow[x] = 0xFF000000 | (c.fRed << 16) | (c.fGreen << 8) | c.fBlue;
					}
					else
					{
						dstRow[x] = 0xFF000000;  // black fallback
					}
				}
			}
			return img;
		}

		case kPixMapFormat1:
		{
			QImage tmp (static_cast<const uchar*> (bits), w, h, rowBytes,
						QImage::Format_Mono);
			return tmp.convertToFormat (QImage::Format_RGB32);
		}

		default:
		{
			EmPixMap temp (pixmap);
			temp.ConvertToFormat (kPixMapFormat32ARGB);
			return emPixMapToQImage (temp);
		}
	}
}


#pragma mark -

// ---------------------------------------------------------------------------
//		EmWindowQt Host method implementations
//		All run on the UI thread (called from HandleIdle → PaintScreen).
// ---------------------------------------------------------------------------

void EmWindowQt::HostWindowReset (void)
{
	// Get the desired client size from the skin region.
	EmRect newBounds = this->GetCurrentSkinRegion ().Bounds ();
	EmCoord w = newBounds.Width ();
	EmCoord h = newBounds.Height ();

	if (w == 0)
		w = kDefaultWidth;
	if (h == 0)
		h = kDefaultHeight;

	// Invalidate cached skin so it gets re-rendered
	fSkinValid = false;

	// Pre-render the skin image for paintEvent (Qt retained-mode).
	// PaintScreen only paints the case on specific triggers, but
	// paintEvent can fire anytime after resize, so we need the
	// skin QImage ready now.
	const EmPixMap& skin = GetCurrentSkin ();
	if (skin.GetSize ().fX > 0 && skin.GetSize ().fY > 0)
	{
		fSkinImage = emPixMapToQImage (skin);
		fSkinValid = true;
	}

	fprintf (stderr, "WINDOW: HostWindowReset skinRegion bounds=(%d,%d,%d,%d) w=%d h=%d skinImg=%dx%d\n",
		(int)newBounds.fLeft, (int)newBounds.fTop,
		(int)newBounds.fRight, (int)newBounds.fBottom,
		(int)w, (int)h,
		fSkinImage.isNull () ? 0 : fSkinImage.width (),
		fSkinImage.isNull () ? 0 : fSkinImage.height ());

	// Resize the window.  Skin dimensions are logical pixel sizes.
	// paintEvent uses drawImage(rect(), ...) to scale the skin image
	// to fill the widget, so Qt handles HiDPI device pixel mapping.
	setMinimumSize (0, 0);
	setMaximumSize (QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
	resize ((int)w, (int)h);
	setFixedSize ((int)w, (int)h);

	fprintf (stderr, "WINDOW: after setFixedSize actual size=%dx%d\n",
		width (), height ());

	// Force repaint
	update ();
}


void EmWindowQt::HostMouseCapture (void)
{
	grabMouse ();
}


void EmWindowQt::HostMouseRelease (void)
{
	releaseMouse ();
}


void EmWindowQt::HostDrawingBegin (void)
{
	// In Qt retained mode, drawing happens in paintEvent.
	// Nothing to do here.
}


void EmWindowQt::HostWindowMoveBy (const EmPoint& offset)
{
	this->HostWindowMoveTo (this->HostWindowBoundsGet ().TopLeft () + offset);
}


void EmWindowQt::HostWindowMoveTo (const EmPoint& loc)
{
	move (loc.fX, loc.fY);
}


EmRect EmWindowQt::HostWindowBoundsGet (void)
{
	return EmRect (
		this->x (),
		this->y (),
		this->x () + this->width (),
		this->y () + this->height ());
}


void EmWindowQt::HostWindowCenter (void)
{
	QScreen* screen = QApplication::primaryScreen ();
	if (screen)
	{
		QRect screenGeom = screen->geometry ();
		int cx = (screenGeom.width () - width ()) / 2;
		int cy = (screenGeom.height () - height ()) / 2;
		move (cx, cy);
	}
}


void EmWindowQt::HostWindowShow (void)
{
	show ();
	raise ();
	activateWindow ();
}


void EmWindowQt::HostRectFrame (const EmRect& r, const EmPoint& pen, const RGBType& color)
{
	fButtonFrame = QRect (r.fLeft, r.fTop,
						  r.fRight - r.fLeft, r.fBottom - r.fTop);
	fButtonFrameColor = QColor (color.fRed, color.fGreen, color.fBlue);
	fButtonFrameVisible = true;
	update ();
}


void EmWindowQt::HostOvalPaint (const EmRect& r, const RGBType& color)
{
	fLEDRect = QRect (r.fLeft, r.fTop,
					  r.fRight - r.fLeft, r.fBottom - r.fTop);
	fLEDColor = QColor (color.fRed, color.fGreen, color.fBlue);
	fLEDVisible = true;
	update ();
}


void EmWindowQt::HostPaintCase (const EmScreenUpdateInfo& info)
{
	const EmPixMap& skin = GetCurrentSkin ();
	fSkinImage = emPixMapToQImage (skin);
	fSkinValid = true;

	// Clear overlays — they get redrawn by their respective Host* calls
	fButtonFrameVisible = false;
	fLEDVisible = false;

	update ();
}


void EmWindowQt::HostPaintLCD (const EmScreenUpdateInfo& info,
							   const EmRect& srcRect,
							   const EmRect& destRect,
							   Bool scaled)
{
	// GetLCDScanlines only fills dirty scanlines (fFirstLine..fLastLine)
	// in info.fImage — the rest is uninitialized garbage.  Keep a
	// persistent fLCDImage and merge only the dirty portion.

	QImage newImage = emPixMapToQImage (info.fImage);

	int w = newImage.width ();
	int h = newImage.height ();

	if (fLCDImage.isNull ()
		|| fLCDImage.width () != w
		|| fLCDImage.height () != h
		|| fLCDImage.format () != newImage.format ())
	{
		// First frame or resolution change — take the whole image.
		fLCDImage = newImage;
	}
	else
	{
		// Merge only the dirty scanlines into the persistent image.
		int firstLine = info.fFirstLine;
		int lastLine  = info.fLastLine;

		if (firstLine < 0)        firstLine = 0;
		if (lastLine > h)         lastLine  = h;

		for (int y = firstLine; y < lastLine; y++)
		{
			memcpy (fLCDImage.scanLine (y),
					newImage.scanLine (y),
					fLCDImage.bytesPerLine ());
		}
	}

	// Qt retained-mode: paintEvent redraws the full widget, so always use
	// the full LCD bounds.  Skin coords map 1:1 to widget logical pixels.
	EmRect lcdBounds = this->GetLCDBounds ();
	fLCDRect = QRect (lcdBounds.fLeft, lcdBounds.fTop,
					  lcdBounds.fRight - lcdBounds.fLeft,
					  lcdBounds.fBottom - lcdBounds.fTop);

	update ();
}


void EmWindowQt::HostGetDefaultSkin (EmPixMap& skin, int scale)
{
	// Generate a simple gray skin with white LCD area
	int lcdX = 32 * scale;
	int lcdY = 32 * scale;
	int lcdW = 160 * scale;
	int lcdH = 160 * scale;

	int skinW = (32 + 160 + 32) * scale;
	int skinH = (32 + 220 + 60) * scale;

	skin.SetSize (EmPoint (skinW, skinH));
	skin.SetFormat (kPixMapFormat24RGB);
	skin.SetRowBytes (skinW * 3);

	void* bits = skin.GetBits ();
	if (bits)
	{
		uint8_t* p = static_cast<uint8_t*> (bits);
		int totalBytes = skinW * skinH * 3;

		// Fill with gray
		for (int i = 0; i < totalBytes; i += 3)
		{
			p[i + 0] = 0x60;
			p[i + 1] = 0x60;
			p[i + 2] = 0x60;
		}

		// White LCD area
		for (int y = lcdY; y < lcdY + lcdH; y++)
		{
			for (int x = lcdX; x < lcdX + lcdW; x++)
			{
				int offset = (y * skinW + x) * 3;
				p[offset + 0] = 0xFF;
				p[offset + 1] = 0xFF;
				p[offset + 2] = 0xFF;
			}
		}
	}
}


EmPoint EmWindowQt::HostGetCurrentMouse (void)
{
	return EmPoint (fMouseX, fMouseY);
}
