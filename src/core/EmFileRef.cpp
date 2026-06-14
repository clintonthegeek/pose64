/* -*- mode: C++; tab-width: 4 -*- */
/* ===================================================================== *\
	Copyright (c) 2000-2001 Palm, Inc. or its subsidiaries.
	All rights reserved.

	This file is part of the Palm OS Emulator.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.
\* ===================================================================== */

#include "EmCommon.h"
#include "EmFileRef.h"

using namespace std;

// Phase 5 dead-code sweep: EmFileRef::SetEmulatorRef / GetEmulatorRef (and the
// file-static gEmulatorRef they backed) were declared in EmFileRef.h and
// defined here but never called by any compiled translation unit, so they were
// removed.  EmFileRef itself is very much alive — only these two unused static
// helpers were dropped.
