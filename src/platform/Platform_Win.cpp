/* -*- mode: C++; tab-width: 4 -*- */
/* ===================================================================== *\
	Copyright (c) 1998-2001 Palm, Inc. or its subsidiaries.
	All rights reserved.

	This file is part of the Palm OS Emulator.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.
\* ===================================================================== */

#include "EmCommon.h"
#include "Platform.h"

#include "ErrorHandling.h"		// Errors::ThrowIfNULL
#include "Miscellaneous.h"		// StMemory
#include "ResStrings.h"
#include "SessionFile.h"
#include "Strings.r.h"			// kStr_ ...

#include <windows.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>
#include <ctype.h>

// Implemented in ResStrings.cpp
extern "C++" const char* _ResGetString(int idx);

#include "omnithread.h"			// omni_mutex

using namespace std;

// Winsock version — referenced by Platform_NetLib_Sck.cpp
WORD			gWinSockVersion = MAKEWORD(2, 2);

// ===========================================================================
//		Globals
// ===========================================================================

ByteList		gClipboardDataPalm;
ByteList		gClipboardDataHost;
omni_mutex		gClipboardMutex;
omni_condition	gClipboardCondition (&gClipboardMutex);
Bool			gClipboardHaveOutgoingData;
Bool			gClipboardNeedIncomingData;
Bool			gClipboardPendingIncomingData;
Bool			gClipboardHaveIncomingData;

static long long PrvGetMicroseconds (void)
{
	static LARGE_INTEGER freq = {};
	if (freq.QuadPart == 0)
		QueryPerformanceFrequency (&freq);

	LARGE_INTEGER now;
	QueryPerformanceCounter (&now);

	return (long long)(now.QuadPart * 1000000LL / freq.QuadPart);
}


// ===========================================================================
//		Platform
// ===========================================================================


// ---------------------------------------------------------------------------
//		Platform::Initialize
// ---------------------------------------------------------------------------

void Platform::Initialize( void )
{
}


// ---------------------------------------------------------------------------
//		Platform::Reset
// ---------------------------------------------------------------------------

void Platform::Reset( void )
{
}


// ---------------------------------------------------------------------------
//		Platform::Save
// ---------------------------------------------------------------------------

void Platform::Save(SessionFile&)
{
}


// ---------------------------------------------------------------------------
//		Platform::Load
// ---------------------------------------------------------------------------

void Platform::Load(SessionFile&)
{
}


// ---------------------------------------------------------------------------
//		Platform::Dispose
// ---------------------------------------------------------------------------

void Platform::Dispose( void )
{
}


// ---------------------------------------------------------------------------
//		Platform::GetString
// ---------------------------------------------------------------------------

string Platform::GetString( StrCode id )
{
	const char* str = _ResGetString (id);
	if (str)
		return string (str);

	char	buffer[20];
	sprintf (buffer, "%ld", (long) id);
	return string ("<missing string ") + buffer + ">";
}


// ---------------------------------------------------------------------------
//		Platform::GetIDForError
// ---------------------------------------------------------------------------

int Platform::GetIDForError( ErrCode error )
{
	switch (error)
	{
	case EPERM:			break;
	case ENOENT:		return kStr_FileNotFound;
	case ESRCH:			break;
	case EINTR:			break;
	case EIO:			return kStr_IOError;
	case ENXIO:			break;
	case E2BIG:			break;
	case ENOEXEC:		break;
	case EBADF:			break;
	case ECHILD:		break;
	case EAGAIN:		break;
	case ENOMEM:		return kStr_MemFull;
	case EACCES:		return kStr_SerialPortBusy;
	case EFAULT:		break;
	case EBUSY:			return kStr_FileBusy;
	case EEXIST:		return kStr_DuplicateFileName;
	case EXDEV:			break;
	case ENODEV:		return kStr_DiskMissing;
	case ENOTDIR:		break;
	case EISDIR:		break;
	case EINVAL:		break;
	case ENFILE:		break;
	case EMFILE:		return kStr_TooManyFilesOpen;
	case ENOTTY:		break;
	case EFBIG:			break;
	case ENOSPC:		return kStr_DiskFull;
	case ESPIPE:		break;
	case EROFS:			return kStr_DiskWriteProtected;
	case EMLINK:		break;
	case EPIPE:			break;
	case EDOM:			break;
	case ERANGE:		break;
	case EDEADLK:		break;
	case ENAMETOOLONG:	return kStr_BadFileName;
	}

	return 0;
}


// ---------------------------------------------------------------------------
//		Platform::GetIDForRecovery
// ---------------------------------------------------------------------------

int Platform::GetIDForRecovery( ErrCode error )
{
	return 0;
}


// ---------------------------------------------------------------------------
//		Platform::GetShortVersionString
// ---------------------------------------------------------------------------

string Platform::GetShortVersionString( void )
{
	return string("3.5");
}


/***********************************************************************
 *
 * FUNCTION:	Platform::CopyToClipboard
 *
 ***********************************************************************/

void Platform::CopyToClipboard (const ByteList& palmChars,
								const ByteList& hostChars)
{
	ByteList	palmChars2 (palmChars);
	ByteList	hostChars2 (hostChars);

	if (hostChars2.size () > 0 && palmChars2.size () == 0)
	{
		Platform::RemapHostToPalmChars (hostChars2, palmChars2);
	}
	else if (palmChars2.size () > 0 && hostChars2.size () == 0)
	{
		Platform::RemapPalmToHostChars (palmChars2, hostChars2);
	}

	omni_mutex_lock lock (gClipboardMutex);

	gClipboardDataPalm = palmChars2;
	gClipboardDataHost = hostChars2;

	gClipboardHaveOutgoingData = true;
}


/***********************************************************************
 *
 * FUNCTION:	Platform::CopyFromClipboard
 *
 ***********************************************************************/

void Platform::CopyFromClipboard (ByteList& palmChars,
								  ByteList& hostChars)
{
	omni_mutex_lock lock (gClipboardMutex);

	gClipboardNeedIncomingData = true;
	gClipboardHaveIncomingData = false;

	while (!gClipboardHaveIncomingData)
		gClipboardCondition.wait ();

	palmChars = gClipboardDataPalm;
	hostChars = gClipboardDataHost;

	if (hostChars.size () > 0 && palmChars.size () == 0)
	{
		Platform::RemapHostToPalmChars (hostChars, palmChars);
	}
	else if (palmChars.size () > 0 && hostChars.size () == 0)
	{
		Platform::RemapPalmToHostChars (palmChars, hostChars);
	}
}


/***********************************************************************
 *
 * FUNCTION:	Platform::RemapHostToPalmChars
 *
 ***********************************************************************/

void Platform::RemapHostToPalmChars	(const ByteList& hostChars,
									 ByteList& palmChars)
{
	ByteList::const_iterator	iter = hostChars.begin ();
	while (iter != hostChars.end ())
	{
		uint8	ch = *iter++;

		if (ch == 0x0D)
		{
			// Skip \r in \r\n sequences (Windows line endings)
		}
		else if (ch == 0x0A)
		{
			palmChars.push_back (chrLineFeed);
		}
		else
		{
			palmChars.push_back (ch);
		}
	}
}


/***********************************************************************
 *
 * FUNCTION:	Platform::RemapPalmToHostChars
 *
 ***********************************************************************/

void Platform::RemapPalmToHostChars	(const ByteList& palmChars,
									 ByteList& hostChars)
{
	ByteList::const_iterator	iter = palmChars.begin ();
	while (iter != palmChars.end ())
	{
		uint8	ch = *iter++;

		if (ch == chrLineFeed)
		{
			hostChars.push_back (0x0D);
			hostChars.push_back (0x0A);
		}
		else
		{
			hostChars.push_back (ch);
		}
	}
}


/***********************************************************************
 *
 * FUNCTION:	Platform::PinToScreen
 *
 ***********************************************************************/

Bool Platform::PinToScreen (EmRect& r)
{
	return false;
}


/***********************************************************************
 *
 * FUNCTION:	ToHostEOL
 *
 ***********************************************************************/

void Platform::ToHostEOL (	StMemory& dest, long& destLen,
				const char* src, long srcLen)
{
	// On Windows, convert to \r\n.  Worst case: every char becomes two.
	char*	d = (char*) Platform::AllocateMemory (srcLen * 2);
	char*	p = d;
	Bool	previousWas0x0D = false;

	for (long ii = 0; ii < srcLen; ++ii)
	{
		char	ch = src[ii];

		if (ch == 0x0D)
		{
			*p++ = 0x0D;
			*p++ = 0x0A;
		}
		else if (ch == 0x0A && previousWas0x0D)
		{
			// Already emitted \r\n above; swallow this \n.
		}
		else if (ch == 0x0A)
		{
			*p++ = 0x0D;
			*p++ = 0x0A;
		}
		else
		{
			*p++ = ch;
		}

		previousWas0x0D = ch == 0x0D;
	}

	destLen = p - d;
	d = (char*) Platform::ReallocMemory (d, destLen);
	dest.Adopt (d);
}


// -----------------------------------------------------------------------------
// find the ROM file path embedded in the saved ram image

Bool Platform::ReadROMFileReference (ChunkFile& docFile, EmFileRef& f)
{
	// Look for a Unix-style path first (cross-platform sessions).
	string	path;
	if (docFile.ReadString (SessionFile::kROMUnixPathTag, path))
	{
		f = EmFileRef (path);
		return true;
	}

	string	name;
	if (docFile.ReadString (SessionFile::kROMNameTag, name))
	{
		// !!! TBD
	}

	return false;
}

void Platform::WriteROMFileReference (ChunkFile& docFile, const EmFileRef& f)
{
	docFile.WriteString (SessionFile::kROMUnixPathTag, f.GetFullPath ());
}


// ---------------------------------------------------------------------------
//		Platform::Delay
// ---------------------------------------------------------------------------

void Platform::Delay (void)
{
	omni_thread::sleep( 0, 10000 ); // 10k nanoseconds = 1/100 sec
}


// ---------------------------------------------------------------------------
//		Platform::CycleSlowly
// ---------------------------------------------------------------------------

void Platform::CycleSlowly (void)
{
}


// ---------------------------------------------------------------------------
//		Platform::RealAllocateMemory
// ---------------------------------------------------------------------------

void* Platform::RealAllocateMemory (size_t size, Bool clear, const char*, int)
{
	void*	result;

	if (clear)
		result = calloc (size, 1);
	else
		result = malloc (size);

	Errors::ThrowIfNULL (result);

	return result;
}


// ---------------------------------------------------------------------------
//		Platform::RealReallocMemory
// ---------------------------------------------------------------------------

void* Platform::RealReallocMemory (void* p, size_t size, const char*, int)
{
	void*	result = realloc (p, size);

	Errors::ThrowIfNULL (result);

	return result;
}


// ---------------------------------------------------------------------------
//		Platform::RealDisposeMemory
// ---------------------------------------------------------------------------

void Platform::RealDisposeMemory (void* p)
{
	if (p)
	{
		free (p);
	}
}


/***********************************************************************
 *
 * FUNCTION:	Platform::ForceStartupScreen
 *
 ***********************************************************************/

Bool Platform::ForceStartupScreen (void)
{
	return false;
}


// ---------------------------------------------------------------------------
//		Platform::StopOnResetKeyDown
// ---------------------------------------------------------------------------

Bool Platform::StopOnResetKeyDown( void )
{
	return false;
}


// ---------------------------------------------------------------------------
//		Platform::CollectOptions
// ---------------------------------------------------------------------------

int Platform::CollectOptions (int argc, char** argv, int& errorArg, int (*cb)(int, char**, int&))
{
	return true;
}


// ---------------------------------------------------------------------------
//		Platform::PrintHelp
// ---------------------------------------------------------------------------

void Platform::PrintHelp (void)
{
	printf ("POSE64 - Palm OS Emulator\n");
}


// ---------------------------------------------------------------------------
//		Platform::GetMilliseconds
// ---------------------------------------------------------------------------

uint32 Platform::GetMilliseconds( void )
{
	long long usecs = PrvGetMicroseconds ();
	uint32   millis = (uint32) (usecs / 1000);

	return millis;
}


// ---------------------------------------------------------------------------
//		Platform::CreateDebuggerSocket
// ---------------------------------------------------------------------------

CSocket* Platform::CreateDebuggerSocket (void)
{
	return NULL;
}


// ---------------------------------------------------------------------------
//		Platform::ExitDebugger
// ---------------------------------------------------------------------------

void Platform::ExitDebugger( void )
{
}


// ---------------------------------------------------------------------------
//		Platform::ViewDrawLine
// ---------------------------------------------------------------------------

void Platform::ViewDrawLine( int xStart, int yStart, int xEnd, int yEnd )
{
}


// ---------------------------------------------------------------------------
//		Platform::ViewDrawPixel
// ---------------------------------------------------------------------------

void Platform::ViewDrawPixel( int xPos, int yPos )
{
}


static void PrvQueueNote (int frequency, int duration, int amplitude)
{
	if (duration > 0 && amplitude > 0)
	{
		Beep (frequency, duration);
	}
}


CallROMType Platform::SndDoCmd (SndCommandType& cmd)
{
	switch (cmd.cmd)
	{
		case sndCmdFreqDurationAmp:
			PrvQueueNote (cmd.param1, cmd.param2, cmd.param3);
			break;

		case sndCmdNoteOn:
			return kExecuteROM;

		case sndCmdFrqOn:
			PrvQueueNote (cmd.param1, cmd.param2, cmd.param3);
			break;

		case sndCmdQuiet:
			return kExecuteROM;
	}

	return kSkipROM;
}

void Platform::StopSound (void)
{
}


void Platform::Beep (void)
{
	MessageBeep (MB_OK);
}
