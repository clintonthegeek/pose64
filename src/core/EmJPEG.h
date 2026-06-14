/* -*- mode: C++; tab-width: 4 -*- */
/* ===================================================================== *\
	Copyright (c) 1999-2001 Palm, Inc. or its subsidiaries.
	All rights reserved.

	This file is part of the Palm OS Emulator.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.
\* ===================================================================== */

#ifndef EmJPEG_h
#define EmJPEG_h

// JPEG decoding is done by Qt's QImage (see EmJPEG.cpp / JPEGToPixMap).  The
// original bundled-libjpeg path (DISABLE_JPEG_SUPPORT, EmJPEGDecompress*,
// ConvertJPEG) was dead code and was removed in Phase 5.

class EmPixMap;
class EmStream;

// Utility function that converts a JPEG (or PNG/BMP — QImage auto-detects)
// from the stream to the given pixmap.

void	JPEGToPixMap	(EmStream&, EmPixMap&);

#endif	/* EmJPEG_h */
