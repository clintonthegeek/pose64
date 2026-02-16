/* -*- mode: C++; tab-width: 4 -*- */
/* Qt PORT: JPEG decoding via Qt's QImage */

#include "EmCommon.h"
#include "EmJPEG.h"
#include "EmPixMap.h"
#include "EmStream.h"

#include <QImage>
#include <QByteArray>

using namespace std;

void JPEGToPixMap(EmStream& stream, EmPixMap& pixmap)
{
	// Read entire stream into a buffer.
	int32 len = stream.GetLength ();
	if (len <= 0)
		return;

	QByteArray data (len, Qt::Uninitialized);
	stream.SetMarker (0, kStreamFromStart);
	stream.GetBytes (data.data (), len);

	// Decode via Qt (handles JPEG, PNG, BMP, etc.)
	QImage img = QImage::fromData (
		reinterpret_cast<const uchar*> (data.constData ()), len);

	if (img.isNull ())
		return;

	// Convert to RGB888 so we get a predictable 24-bit layout.
	img = img.convertToFormat (QImage::Format_RGB888);

	int w = img.width ();
	int h = img.height ();

	pixmap.SetSize (EmPoint (w, h));
	pixmap.SetFormat (kPixMapFormat24RGB);
	pixmap.SetRowBytes (w * 3);

	uint8* dst = static_cast<uint8*> (pixmap.GetBits ());
	for (int y = 0; y < h; ++y)
	{
		const uchar* src = img.constScanLine (y);
		memcpy (dst + y * w * 3, src, w * 3);
	}
}
