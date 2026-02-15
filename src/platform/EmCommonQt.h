/* -*- mode: C++; tab-width: 4 -*- */
/* ===================================================================== *\
	QtPOSE - Qt-based Palm OS Emulator
	Common header for Qt platform layer
\* ===================================================================== */

#ifndef EmCommonQt_h
#define EmCommonQt_h

// Common header file included by all Palm OS Emulator Qt platform files

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

// Qt includes (instead of FLTK)
#include <QApplication>
#include <QString>
#include <QByteArray>

// ============================================
// ========== Socket compatibility  ===========
// ============================================

#include <sys/types.h>
#include <sys/socket.h>

#ifndef HAVE_TYPE_SOCKLEN_T
	typedef int socklen_t;
#endif

typedef int SOCKET;

#define WSAEINVAL       EINVAL
#define INVALID_SOCKET  ((SOCKET)(~0))
#define SOCKET_ERROR    (-1)
#define closesocket     close
#define ioctlsocket     ioctl

#ifndef INADDR_LOOPBACK
#define INADDR_LOOPBACK 0x7f000001
#endif

#endif /* EmCommonQt_h */
