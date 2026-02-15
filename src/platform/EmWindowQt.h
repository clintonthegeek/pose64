/* -*- mode: C++; tab-width: 4 -*- */
/* Qt PORT v2: Qt-based EmWindow implementation.
 *
 * Threading model (matches original FLTK POSE):
 *   UI Thread  — Qt event loop, QWidget painting, mouse/key/menu events,
 *                QTimer fires HandleIdle() at ~10Hz
 *   CPU Thread — omnithread, runs PalmOS CPU emulation
 *
 * NO bridge thread.  The UI thread directly drives HandleIdle() via QTimer,
 * exactly as FLTK's Fl::wait(0.1) + HandleIdle() did.
 */

#ifndef EmWindowQt_h
#define EmWindowQt_h

// Must include EmCommon.h first to define Bool, uint32, etc.
// This is needed because Qt's MOC generates a compilation unit
// that includes this header without going through EmCommon.h.
#include "EmCommon.h"

#include "EmWindow.h"
#include <QWidget>
#include <QImage>

class QMenu;

#include "EmMenus.h"

class EmWindowQt : public QWidget, public EmWindow
{
public:
	EmWindowQt ();
	virtual ~EmWindowQt ();

protected:
	// QWidget overrides
	void paintEvent (QPaintEvent* event) override;
	void mousePressEvent (QMouseEvent* event) override;
	void mouseReleaseEvent (QMouseEvent* event) override;
	void mouseMoveEvent (QMouseEvent* event) override;
	void keyPressEvent (QKeyEvent* event) override;
	void keyReleaseEvent (QKeyEvent* event) override;
	void contextMenuEvent (QContextMenuEvent* event) override;
	void closeEvent (QCloseEvent* event) override;

private:
	// EmWindow pure virtual implementations
	// All called from UI thread (via HandleIdle → PaintScreen)
	virtual void HostWindowReset () override;
	virtual void HostMouseCapture () override;
	virtual void HostMouseRelease () override;
	virtual void HostDrawingBegin () override;
	virtual void HostWindowMoveBy (const EmPoint&) override;
	virtual void HostWindowMoveTo (const EmPoint&) override;
	virtual EmRect HostWindowBoundsGet () override;
	virtual void HostWindowCenter () override;
	virtual void HostWindowShow () override;

	virtual void HostRectFrame (const EmRect&, const EmPoint&, const RGBType&) override;
	virtual void HostOvalPaint (const EmRect&, const RGBType&) override;

	virtual void HostPaintCase (const EmScreenUpdateInfo&) override;
	virtual void HostPaintLCD (const EmScreenUpdateInfo& info,
							   const EmRect& srcRect,
							   const EmRect& destRect,
							   Bool scaled) override;

	virtual void HostGetDefaultSkin (EmPixMap&, int scale) override;
	virtual EmPoint HostGetCurrentMouse () override;

	// Menu helpers
	void buildQMenu (QMenu& qmenu, const EmMenuItemList& items);

	// Pixel format conversion
	QImage emPixMapToQImage (const EmPixMap& pixmap);

private:
	// Screen state (only accessed from UI thread)
	QImage fSkinImage;
	bool fSkinValid;
	QImage fLCDImage;
	QRect fLCDRect;

	// Cached mouse position
	int fMouseX;
	int fMouseY;

	// Button press visual feedback (HostRectFrame)
	QRect fButtonFrame;
	QColor fButtonFrameColor;
	bool fButtonFrameVisible;

	// LED indicator (HostOvalPaint)
	QRect fLEDRect;
	QColor fLEDColor;
	bool fLEDVisible;
};

extern EmWindowQt* gHostWindow;

#endif // EmWindowQt_h
