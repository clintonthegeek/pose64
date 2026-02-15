/* -*- mode: C++; tab-width: 4 -*- */
/* Qt PORT: JPEG support stub */

#include "EmCommon.h"
#include "EmJPEG.h"
#include "EmPixMap.h"
#include "EmStream.h"

using namespace std;

// Qt PORT: JPEG loading not yet implemented
// This is a stub to allow linking

void JPEGToPixMap(EmStream& stream, EmPixMap& pixmap)
{
	// TODO: Implement JPEG loading using Qt's QImage or libjpeg
	// For now, create a blank pixmap to avoid crashes
	pixmap.SetSize(EmPoint(1, 1));
	pixmap.SetFormat(kPixMapFormat8);
}
