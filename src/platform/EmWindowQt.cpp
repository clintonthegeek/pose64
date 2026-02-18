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
#include <QBitmap>
#include <QApplication>
#include <QScreen>
#include <QMenu>
#include <QWindow>

#include "Skins.h"

#include <QImage>

#include <cstdio>

using namespace std;

const int kDefaultWidth = 220;
const int kDefaultHeight = 330;


// ---------------------------------------------------------------------------
//		PrvMaskFromEmPixMap
// ---------------------------------------------------------------------------
// Convert a 1-bpp EmPixMap mask to a QBitmap.  The EmPixMap uses MSB-first
// bit order, matching QImage::Format_Mono.  In the mask, bit=1 means opaque
// (inside the skin) and bit=0 means transparent (outside).
//
// In QBitmap, Qt::color1 (bit=1) = opaque/foreground, Qt::color0 (bit=0) =
// transparent/background.  The raw bits match directly, so we use
// Qt::MonoOnly to prevent QBitmap::fromImage from doing luminance conversion.

static QBitmap PrvMaskFromEmPixMap (const EmPixMap& mask)
{
	EmPoint size = mask.GetSize ();
	int w = size.fX;
	int h = size.fY;

	if (w <= 0 || h <= 0)
		return QBitmap ();

	const void* bits = mask.GetBits ();
	if (!bits)
		return QBitmap ();

	EmPixMapRowBytes srcRowBytes = mask.GetRowBytes ();

	// Build a fresh QImage and copy row by row.  EmPixMap 1-bpp is
	// MSB-first (matching Format_Mono) but may have different row
	// padding than Qt expects.
	QImage img (w, h, QImage::Format_Mono);
	img.setColor (0, qRgb (255, 255, 255));	// bit 0 = white (transparent in mask)
	img.setColor (1, qRgb (0, 0, 0));			// bit 1 = black (opaque in mask)

	int dstRowBytes = img.bytesPerLine ();
	const uint8_t* src = static_cast<const uint8_t*> (bits);

	int copyBytes = (srcRowBytes < dstRowBytes) ? srcRowBytes : dstRowBytes;
	for (int y = 0; y < h; ++y)
	{
		memcpy (img.scanLine (y), src + y * srcRowBytes, copyBytes);
	}

	// In QBitmap/setMask: 1-bits (black/color1) = opaque, 0-bits (white/color0) = transparent.
	// The EmPixMap mask convention is the same (1=opaque), so the raw bits
	// can be used directly.  Qt::MonoOnly prevents luminance re-quantization.
	return QBitmap::fromImage (img, Qt::MonoOnly);
}


// ---------------------------------------------------------------------------
//		PrvApplyMaskAlpha
// ---------------------------------------------------------------------------
// Apply a 1-bpp EmPixMap mask as the alpha channel of a QImage.
//
// When feather=true, derives smooth alpha from the skin image's own pixel
// luminance near the mask boundary.  The binary mask identifies the interior;
// for a band of pixels near the edge, alpha is computed from how dark the
// skin pixel is (dark = opaque, white = transparent).  This uses the JPEG's
// natural anti-aliased color gradients for genuinely smooth edges.
//
// When feather=false, applies a hard binary alpha (0 or 255).

static void PrvApplyMaskAlpha (QImage& image, const EmPixMap& mask,
							   bool feather)
{
	EmPoint msize = mask.GetSize ();
	int mw = msize.fX;
	int mh = msize.fY;
	int iw = image.width ();
	int ih = image.height ();

	if (mw <= 0 || mh <= 0)
		return;

	const void* bits = mask.GetBits ();
	if (!bits)
		return;

	// Convert image to ARGB32 if needed so we can set alpha.
	if (image.format () != QImage::Format_ARGB32 &&
		image.format () != QImage::Format_ARGB32_Premultiplied)
	{
		image = image.convertToFormat (QImage::Format_ARGB32);
	}

	EmPixMapRowBytes maskRowBytes = mask.GetRowBytes ();
	const uint8_t* maskBits = static_cast<const uint8_t*> (bits);

	int w = (iw < mw) ? iw : mw;
	int h = (ih < mh) ? ih : mh;

	// --- Step 1: Convert 1-bpp mask to 8-bit alpha (0 or 255) ---

	vector<uint8_t> alpha (w * h);

	for (int y = 0; y < h; ++y)
	{
		const uint8_t* maskRow = maskBits + y * maskRowBytes;
		for (int x = 0; x < w; ++x)
		{
			int byteIdx = x >> 3;
			int bitIdx  = 7 - (x & 7);
			alpha[y * w + x] = ((maskRow[byteIdx] >> bitIdx) & 1) ? 255 : 0;
		}
	}

	// --- Step 2: Luminance-based edge feather ---

	if (feather)
	{
		// Sub-pixel coverage anti-aliasing via supersampling.
		//
		// 1. Upsample binary mask to 4x (nearest-neighbor)
		// 2. Chamfer distance transform at 4x resolution
		// 3. Erode by thresholding (eats JPEG fringe)
		// 4. Box-downsample to 1x: each pixel's alpha = fraction of
		//    its 16 sub-pixels that are inside the eroded mask.
		//
		// At diagonal staircase steps, 4x4 blocks straddle the edge
		// and produce fractional alpha — the same geometric AA that
		// OpenGL uses for rasterized edges.

		static const int kScale   = 4;
		static const int kErode   = 18;  // erosion in chamfer units
		                                  // (6 px at 4x = 1.5 px at 1x)
		static const int kDistInf = 10000;

		int hw = w * kScale;
		int hh = h * kScale;

		// Upsample to 4x (nearest-neighbor).
		vector<uint8_t> hires (hw * hh);
		for (int y = 0; y < hh; ++y)
		{
			int sy = y / kScale;
			for (int x = 0; x < hw; ++x)
				hires[y * hw + x] = alpha[sy * w + x / kScale];
		}

		// Chamfer 3/4 interior distance at 4x.
		vector<int> dist (hw * hh);
		for (int i = 0; i < hw * hh; ++i)
			dist[i] = hires[i] ? kDistInf : 0;

		for (int y = 0; y < hh; ++y)
			for (int x = 0; x < hw; ++x)
			{
				int d = dist[y * hw + x];
				if (d == 0) continue;
				if (x > 0)
					d = min (d, dist[y * hw + (x - 1)] + 3);
				if (y > 0)
				{
					d = min (d, dist[(y - 1) * hw + x] + 3);
					if (x > 0)
						d = min (d, dist[(y - 1) * hw + (x - 1)] + 4);
					if (x < hw - 1)
						d = min (d, dist[(y - 1) * hw + (x + 1)] + 4);
				}
				dist[y * hw + x] = d;
			}

		for (int y = hh - 1; y >= 0; --y)
			for (int x = hw - 1; x >= 0; --x)
			{
				int d = dist[y * hw + x];
				if (d == 0) continue;
				if (x < hw - 1)
					d = min (d, dist[y * hw + (x + 1)] + 3);
				if (y < hh - 1)
				{
					d = min (d, dist[(y + 1) * hw + x] + 3);
					if (x < hw - 1)
						d = min (d, dist[(y + 1) * hw + (x + 1)] + 4);
					if (x > 0)
						d = min (d, dist[(y + 1) * hw + (x - 1)] + 4);
				}
				dist[y * hw + x] = d;
			}

		// Box-downsample: alpha = count of sub-pixels surviving erosion.
		int total = kScale * kScale;
		for (int y = 0; y < h; ++y)
		{
			for (int x = 0; x < w; ++x)
			{
				int count = 0;
				for (int dy = 0; dy < kScale; ++dy)
					for (int dx = 0; dx < kScale; ++dx)
						if (dist[(y * kScale + dy) * hw
								 + x * kScale + dx] > kErode)
							++count;
				alpha[y * w + x] = (uint8_t)(count * 255 / total);
			}
		}
	}

	// --- Final: Apply alpha to image ---

	for (int y = 0; y < h; ++y)
	{
		uint32_t* imgRow = reinterpret_cast<uint32_t*> (image.scanLine (y));
		for (int x = 0; x < w; ++x)
		{
			uint8_t a = alpha[y * w + x];
			if (a == 0)
				imgRow[x] = 0x00000000;
			else if (a < 255)
				imgRow[x] = ((uint32_t)a << 24) | (imgRow[x] & 0x00FFFFFF);
			// a == 255: pixel unchanged (fully opaque)
		}
	}
}

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
	fLEDVisible (false),
	fLCDTintActive (false)
{
	EmAssert (gHostWindow == NULL);
	gHostWindow = this;

	setWindowTitle ("POSE64");

	// Enable keyboard focus
	setFocusPolicy (Qt::StrongFocus);

	// Set WA_TranslucentBackground BEFORE the native window is created
	// (i.e. before show() is ever called).  This tells Qt to request an
	// alpha-capable surface from the compositor.  Same approach as Konsole.
	Preference<bool> prefFrameless (kPrefKeyFramelessWindow);
	if (*prefFrameless)
	{
		setAttribute (Qt::WA_TranslucentBackground, true);
	}

	// Load the generic skin so the window has a proper appearance
	// even before a session is created.  WindowResetDefault() calls
	// HostWindowReset() which handles sizing and flag application.
	WindowResetDefault ();
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

	// Always draw the skin as background — even before a session exists,
	// WindowResetDefault() has loaded the generic skin.  The skin image
	// has alpha=0 in masked (non-skin) areas, so default SourceOver
	// blending over the transparent background (WA_TranslucentBackground
	// + alpha surface format) produces correct transparency.
	if (!fSkinImage.isNull ())
	{
		painter.drawImage (rect (), fSkinImage);
	}

	// Only draw LCD, button, and LED overlays when a session exists.
	if (gDocument)
	{
		// When the backlight is on with transparent LCD, draw a
		// semi-transparent color wash over the LCD area first.
		// The skin texture shows through the tint, then the
		// alpha-black LCD pixels composite on top.
		if (fLCDTintActive)
		{
			painter.fillRect (fLCDRect, fLCDTint);
		}

		if (!fLCDImage.isNull ())
		{
			painter.drawImage (fLCDRect, fLCDImage);
		}

		if (fButtonFrameVisible)
		{
			painter.setPen (QPen (fButtonFrameColor, 2));
			painter.setBrush (Qt::NoBrush);
			painter.drawRect (fButtonFrame);
		}

		if (fLEDVisible)
		{
			painter.setPen (Qt::NoPen);
			painter.setBrush (fLEDColor);
			painter.drawEllipse (fLEDRect);
		}
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
		// If no session exists, any left-click starts a window drag.
		if (!gSession)
		{
			// Use system move for Wayland compatibility.
			if (windowHandle ())
				windowHandle ()->startSystemMove ();
			return;
		}

		// Check what skin element was clicked.
		EmPoint pt (event->pos ().x (), event->pos ().y ());
		SkinElementType what = ::SkinTestPoint (pt);

		if (what == kElement_Frame)
		{
			// Click on skin frame — start a window drag.
			if (windowHandle ())
				windowHandle ()->startSystemMove ();
			return;
		}

		if (what == kElement_None)
		{
			// Dead zone around buttons — ignore the click entirely
			// so high-precision mice don't accidentally start drags.
			return;
		}

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
//		PrvFindShortcutCommand
// ---------------------------------------------------------------------------
// Recursively search a menu item list for an active item whose shortcut
// matches the given character (case-insensitive).  Returns kCommandNone
// if no match is found.

static EmCommandID PrvFindShortcutCommand (const EmMenuItemList& items, char ch)
{
	ch = (char) toupper ((unsigned char) ch);

	for (const auto& item : items)
	{
		if (!item.GetChildren ().empty ())
		{
			EmCommandID cmd = PrvFindShortcutCommand (item.GetChildren (), ch);
			if (cmd != kCommandNone)
				return cmd;
		}
		else if (item.GetIsActive ()
				 && toupper ((unsigned char) item.GetShortcut ()) == ch)
		{
			return item.GetCommand ();
		}
	}

	return kCommandNone;
}


// ---------------------------------------------------------------------------
//		EmWindowQt::keyPressEvent
// ---------------------------------------------------------------------------
// Map Qt key events to Palm key events.
// Reference: EmWindowFltk::handle (FL_KEYBOARD case)

void EmWindowQt::keyPressEvent (QKeyEvent* event)
{
	if (event->modifiers () & Qt::MetaModifier)
	{
		event->ignore ();
		return;
	}

	// Alt+key: dispatch menu shortcut commands.
	if (event->modifiers () & Qt::AltModifier)
	{
		int key = event->key ();
		if (key >= Qt::Key_A && key <= Qt::Key_Z)
		{
			char ch = (char) (key & 0xFF);	// Qt::Key_A == 'A'
			EmMenu* popupMenu = MenuFindMenu (kMenuPopupMenuPreferred);
			if (popupMenu)
			{
				MenuUpdateMenuItemStatus (*popupMenu);
				EmCommandID cmd = PrvFindShortcutCommand (*popupMenu, ch);
				if (cmd != kCommandNone)
				{
					if (gDocument && gDocument->HandleCommand (cmd))
					{
						event->accept ();
						return;
					}
					if (gApplication)
					{
						gApplication->HandleCommand (cmd);
						event->accept ();
						return;
					}
				}
			}
		}
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
			if (item.GetIsChecked ())
			{
				action->setCheckable (true);
				action->setChecked (true);
			}
			if (item.GetShortcut ())
			{
				action->setShortcut (
					QKeySequence (QString ("Alt+%1").arg (
						QChar (item.GetShortcut ()))));
			}
		}
	}
}


// ---------------------------------------------------------------------------
//		EmWindowQt pixel format conversion
// ---------------------------------------------------------------------------
// Converts POSE EmPixMap to QImage.  Thread-safe (no QWidget calls).

QImage EmWindowQt::emPixMapToQImage (const EmPixMap& pixmap,
									  bool transparentLCD)
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
			int numColors = (int) colors.size ();
			QImage img (w, h, QImage::Format_ARGB32);
			const uint8_t* src = static_cast<const uint8_t*> (bits);
			for (int y = 0; y < h; y++)
			{
				const uint8_t* srcRow = src + y * rowBytes;
				uint32_t* dstRow = reinterpret_cast<uint32_t*> (img.scanLine (y));
				for (int x = 0; x < w; x++)
				{
					uint8_t idx = srcRow[x];
					if (transparentLCD && numColors > 1)
					{
						uint8_t a = (uint8_t)(idx * 255 / (numColors - 1));
						dstRow[x] = ((uint32_t)a << 24);
					}
					else if (idx < numColors)
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
			// 1-bpp indexed: use the color table from PrvGetPalette().
			// Palm convention: index 0 = background (LCD green), index 1 = foreground (black).
			// Qt's default Format_Mono color table is inverted (0=black, 1=white)
			// and lacks the LCD green, so we must apply the color table manually.
			//
			// When transparentLCD: index 0 = fully transparent, index 1 = fully
			// opaque black.  The skin's own LCD area color shows through.
			const RGBList& colors = pixmap.GetColorTable ();
			int numColors = (int) colors.size ();
			QImage img (w, h, QImage::Format_ARGB32);
			const uint8_t* src = static_cast<const uint8_t*> (bits);
			for (int y = 0; y < h; y++)
			{
				const uint8_t* srcRow = src + y * rowBytes;
				uint32_t* dstRow = reinterpret_cast<uint32_t*> (img.scanLine (y));
				for (int x = 0; x < w; x++)
				{
					int byteIdx = x >> 3;
					int bitIdx  = 7 - (x & 7);  // MSB first
					uint8_t idx = (srcRow[byteIdx] >> bitIdx) & 1;
					if (transparentLCD && numColors > 1)
					{
						uint8_t a = (uint8_t)(idx * 255 / (numColors - 1));
						dstRow[x] = ((uint32_t)a << 24);
					}
					else if (idx < numColors)
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

		case kPixMapFormat2:
		{
			// 2-bpp indexed (4-shade grayscale): apply color table.
			const RGBList& colors = pixmap.GetColorTable ();
			int numColors = (int) colors.size ();
			QImage img (w, h, QImage::Format_ARGB32);
			const uint8_t* src = static_cast<const uint8_t*> (bits);
			for (int y = 0; y < h; y++)
			{
				const uint8_t* srcRow = src + y * rowBytes;
				uint32_t* dstRow = reinterpret_cast<uint32_t*> (img.scanLine (y));
				for (int x = 0; x < w; x++)
				{
					int byteIdx = x >> 2;
					int shift   = 6 - ((x & 3) << 1);  // MSB first: pixels 0,1,2,3 at bits 6,4,2,0
					uint8_t idx = (srcRow[byteIdx] >> shift) & 0x03;
					if (transparentLCD && numColors > 1)
					{
						uint8_t a = (uint8_t)(idx * 255 / (numColors - 1));
						dstRow[x] = ((uint32_t)a << 24);
					}
					else if (idx < numColors)
					{
						const RGBType& c = colors[idx];
						dstRow[x] = 0xFF000000 | (c.fRed << 16) | (c.fGreen << 8) | c.fBlue;
					}
					else
					{
						dstRow[x] = 0xFF000000;
					}
				}
			}
			return img;
		}

		case kPixMapFormat4:
		{
			// 4-bpp indexed (16-shade grayscale): apply color table.
			const RGBList& colors = pixmap.GetColorTable ();
			int numColors = (int) colors.size ();
			QImage img (w, h, QImage::Format_ARGB32);
			const uint8_t* src = static_cast<const uint8_t*> (bits);
			for (int y = 0; y < h; y++)
			{
				const uint8_t* srcRow = src + y * rowBytes;
				uint32_t* dstRow = reinterpret_cast<uint32_t*> (img.scanLine (y));
				for (int x = 0; x < w; x++)
				{
					int byteIdx = x >> 1;
					int shift   = (x & 1) ? 0 : 4;  // MSB first: high nibble first
					uint8_t idx = (srcRow[byteIdx] >> shift) & 0x0F;
					if (transparentLCD && numColors > 1)
					{
						uint8_t a = (uint8_t)(idx * 255 / (numColors - 1));
						dstRow[x] = ((uint32_t)a << 24);
					}
					else if (idx < numColors)
					{
						const RGBType& c = colors[idx];
						dstRow[x] = 0xFF000000 | (c.fRed << 16) | (c.fGreen << 8) | c.fBlue;
					}
					else
					{
						dstRow[x] = 0xFF000000;
					}
				}
			}
			return img;
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
	// Invalidate cached skin so it gets re-rendered
	fSkinValid = false;

	// Invalidate stale LCD image and rect — they were rendered at
	// the old scale.  PaintScreen will re-render them at the new
	// scale on the next idle cycle.
	fLCDImage = QImage ();
	fLCDRect  = QRect ();

	// Clear overlays that reference old-scale coordinates
	fButtonFrameVisible = false;
	fLEDVisible = false;

	// Read frameless / on-top / feather preferences up front.
	Preference<bool> prefFrameless (kPrefKeyFramelessWindow);
	Preference<bool> prefOnTop (kPrefKeyStayOnTop);
	Preference<bool> prefFeather (kPrefKeyFeatheredEdges);

	// Pre-render the skin image for paintEvent (Qt retained-mode).
	// PaintScreen only paints the case on specific triggers, but
	// paintEvent can fire anytime after resize, so we need the
	// skin QImage ready now.
	//
	// The widget is sized to match the skin IMAGE dimensions (not the
	// mask region bounds).  This ensures LCD coordinates (which are in
	// skin image space) map 1:1 to widget pixels.  The mask/alpha
	// channel handles visual and input clipping of edge pixels.
	const EmPixMap& skin = GetCurrentSkin ();
	int w = skin.GetSize ().fX;
	int h = skin.GetSize ().fY;

	if (w > 0 && h > 0)
	{
		fSkinImage = emPixMapToQImage (skin);
		fSkinValid = true;

		// When frameless, apply the mask as the alpha channel so
		// the non-skin area is visually transparent (Wayland
		// compositors don't clip visuals via setMask alone).
		if (*prefFrameless)
			PrvApplyMaskAlpha (fSkinImage, GetCurrentSkinMask (),
							   *prefFeather);
	}

	if (w <= 0)
		w = kDefaultWidth;
	if (h <= 0)
		h = kDefaultHeight;

	// Clear old mask before resize — on Wayland, a stale larger mask
	// can prevent the window from shrinking to a smaller scale.
	clearMask ();

	// Resize the window.  Skin dimensions are logical pixel sizes.
	// paintEvent uses drawImage(rect(), ...) to scale the skin image
	// to fill the widget, so Qt handles HiDPI device pixel mapping.
	setMinimumSize (0, 0);
	setMaximumSize (QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
	resize ((int)w, (int)h);
	setFixedSize ((int)w, (int)h);

	// Apply Frameless Window and Stay On Top preferences.
	// setWindowFlags() hides the widget, so we must call show()
	// after — but only if we were already visible.
	Qt::WindowFlags flags = Qt::Window;
	if (*prefFrameless)
		flags |= Qt::FramelessWindowHint;
	if (*prefOnTop)
		flags |= Qt::WindowStaysOnTopHint;

	if (flags != windowFlags ())
	{
		bool wasVisible = isVisible ();
		setWindowFlags (flags);

		// Set WA_TranslucentBackground before show() so the native
		// window surface is created with an alpha channel from the
		// start (same ordering Konsole uses).
		setAttribute (Qt::WA_TranslucentBackground, *prefFrameless);

		if (wasVisible)
			show ();
	}
	else
	{
		// Flags unchanged, but ensure the attribute is still set
		// (it may have been lost to a prior setWindowFlags call).
		setAttribute (Qt::WA_TranslucentBackground, *prefFrameless);
	}

	// Apply window mask (clips input on all platforms, visual on X11).
	if (*prefFrameless)
	{
		QBitmap qmask = PrvMaskFromEmPixMap (GetCurrentSkinMask ());
		if (!qmask.isNull ())
			setMask (qmask);
		else
			clearMask ();
	}
	else
	{
		clearMask ();
	}

	// Force a full PaintScreen cycle to re-render the LCD at the new
	// scale before we paint.  PaintScreen(true, true) redraws the case
	// and the entire LCD, populating fLCDImage/fLCDRect with correct
	// new-scale data.  Then repaint() synchronously blits it all.
	if (gSession)
		this->PaintScreen (true, true);
	repaint ();
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

	// When frameless, apply the mask as alpha so the non-skin area
	// is visually transparent.
	Preference<bool> prefFrameless (kPrefKeyFramelessWindow);
	Preference<bool> prefFeather (kPrefKeyFeatheredEdges);
	if (*prefFrameless)
		PrvApplyMaskAlpha (fSkinImage, GetCurrentSkinMask (),
						   *prefFeather);

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

	Preference<bool> prefTransparent (kPrefKeyTransparentLCD);
	bool transparent = *prefTransparent;

	// Color devices (8-bpp+) need an opaque background — the alpha
	// transparency technique only works for grayscale/mono displays
	// where pixel index maps linearly to ink intensity.
	if (transparent && info.fImage.GetDepth () >= 8)
		transparent = false;

	QImage newImage = emPixMapToQImage (info.fImage, transparent);

	// Detect backlight for transparent LCD mode.  When the palette's
	// background color (index 0) differs from the skin's normal
	// background, the backlight is on — use it as a color wash.
	fLCDTintActive = false;
	if (transparent)
	{
		const RGBList& colors = info.fImage.GetColorTable ();
		if (!colors.empty ())
		{
			RGBType skinBg = ::SkinGetBackgroundColor ();
			const RGBType& palBg = colors[0];

			if (palBg.fRed != skinBg.fRed ||
				palBg.fGreen != skinBg.fGreen ||
				palBg.fBlue != skinBg.fBlue)
			{
				fLCDTint = QColor (palBg.fRed, palBg.fGreen, palBg.fBlue, 102);
				fLCDTintActive = true;
			}
		}
	}

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


void EmWindowQt::HostGetDefaultSkin (EmPixMap& pixMap, int scale)
{
	QImage img (":/DefaultLarge.png");

	if (img.isNull ())
	{
		fprintf (stderr, "SKIN: failed to load embedded DefaultLarge.png\n");
		return;
	}

	// Scale down for 1x (the resource is 2x)
	if (scale == 1)
	{
		img = img.scaled (img.width () / 2, img.height () / 2,
						  Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
	}

	img = img.convertToFormat (QImage::Format_RGB888);

	int w = img.width ();
	int h = img.height ();

	pixMap.SetSize (EmPoint (w, h));
	pixMap.SetFormat (kPixMapFormat24RGB);
	pixMap.SetRowBytes (w * 3);

	uint8* dest = (uint8*) pixMap.GetBits ();
	for (int y = 0; y < h; ++y)
	{
		memcpy (dest + y * w * 3, img.scanLine (y), w * 3);
	}
}


EmPoint EmWindowQt::HostGetCurrentMouse (void)
{
	return EmPoint (fMouseX, fMouseY);
}
