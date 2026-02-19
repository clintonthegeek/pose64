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

#ifndef EmCommonWin_h
#define EmCommonWin_h

// Common header file included by all Palm OS Emulator for Windows files.

#define __STL_USE_SGI_ALLOCATORS

// Palm headers

#include "Palm.h"


// Std C/C++ Library stuff

#include <ctype.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <deque>
#include <list>
#include <string>
#include <utility>
#include <vector>
#include <map>


// ============================================
// ========== Socket compatibility  ===========
// ============================================

#include <winsock2.h>
#include <ws2tcpip.h>

// Winsock already defines SOCKET, INVALID_SOCKET, SOCKET_ERROR,
// closesocket, ioctlsocket, and WSAEINVAL natively.

#ifndef INADDR_LOOPBACK
#define INADDR_LOOPBACK	0x7f000001
#endif

#endif	/* EmCommonWin_h */
