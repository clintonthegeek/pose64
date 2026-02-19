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
#include "EmFileRefUnix.h"

#include "Miscellaneous.h"		// EndsWith
#include "Platform.h"			// stricmp

#include <errno.h>				// ENOENT
#include <sys/stat.h>
#include <io.h>
#include <direct.h>

using namespace std;

static const char*	kExtension[] =
{
	NULL,		// kFileTypeNone,
	NULL,		// kFileTypeApplication
	".rom",		// kFileTypeROM,
	".psf",		// kFileTypeSession,
	".pev",		// kFileTypeEvents,
	".ini",		// kFileTypePreference,
	".prc",		// kFileTypePalmApp,
	".pdb",		// kFileTypePalmDB,
	".pqa",		// kFileTypePalmQA,
	".txt",		// kFileTypeText,
	NULL,		// kFileTypePicture,
	".skin",	// kFileTypeSkin,
	".prof",	// kFileTypeProfile,
	NULL,		// kFileTypePalmAll,
	NULL		// kFileTypeAll
};


EmFileRef::EmFileRef (void) :
	fFilePath ()
{
	COMPILE_TIME_ASSERT(countof (kExtension) == kFileTypeLast);
}


EmFileRef::EmFileRef (const EmFileRef& other) :
	fFilePath (other.fFilePath)
{
}


EmFileRef::EmFileRef (const char* path) :
	fFilePath (path)
{
	this->MaybePrependCurrentDirectory ();
	this->MaybeNormalize ();
}


EmFileRef::EmFileRef (const string& path) :
	fFilePath (path)
{
	this->MaybePrependCurrentDirectory ();
	this->MaybeNormalize ();
}


EmFileRef::EmFileRef (const EmDirRef& parent, const char* path) :
	fFilePath (parent.GetFullPath () + path)
{
	this->MaybeNormalize ();
}


EmFileRef::EmFileRef (const EmDirRef& parent, const string& path) :
	fFilePath (parent.GetFullPath () + path)
{
	this->MaybeNormalize ();
}


EmFileRef::~EmFileRef (void)
{
}


EmFileRef&
EmFileRef::operator= (const EmFileRef& other)
{
	if (&other != this)
	{
		fFilePath = other.fFilePath;
	}

	return *this;
}


Bool
EmFileRef::IsSpecified (void) const
{
	return !fFilePath.empty ();
}


Bool
EmFileRef::Exists (void) const
{
	if (this->IsSpecified ())
	{
		struct _stat buf;
		int result = _stat (fFilePath.c_str (), &buf);

		return result == 0;
	}

	return false;
}


Bool
EmFileRef::IsType (EmFileType type) const
{
	if (fFilePath.size () > 4 &&
		kExtension[type] != NULL &&
		::EndsWith (fFilePath.c_str (), kExtension[type]))
	{
		return true;
	}

	// Add special hacks for ROM files.
	if (type == kFileTypeROM && ::StartsWith (fFilePath.c_str(), "rom."))
	{
		return true;
	}

	return false;
}


void
EmFileRef::SetCreatorAndType (EmFileCreator creator, EmFileType fileType) const
{
}


int
EmFileRef::GetAttr (int * mode) const
{
	EmAssert(mode);

	*mode = 0;

	if (!IsSpecified())
		return ENOENT;

	struct _stat stat_buf;
	if (_stat(GetFullPath().c_str(), &stat_buf))
		return errno;

	if ((stat_buf.st_mode & _S_IWRITE) == 0)
		*mode |= kFileAttrReadOnly;

	return 0;
}


int
EmFileRef::SetAttr (int mode) const
{
	if (!IsSpecified())
		return ENOENT;

	int newMode = _S_IREAD;
	if (!(mode & kFileAttrReadOnly))
		newMode |= _S_IWRITE;

	if (_chmod(GetFullPath().c_str(), newMode))
		return errno;

	return 0;
}


string
EmFileRef::GetName (void) const
{
	string	result;

	if (this->IsSpecified ())
	{
		string::size_type pos = fFilePath.find_last_of ("/\\");
		if (pos != string::npos)
			result = fFilePath.substr (pos + 1, string::npos);
		else
			result = fFilePath;
	}

	return result;
}


EmDirRef
EmFileRef::GetParent (void) const
{
	EmDirRef	result;

	if (this->IsSpecified ())
	{
		string::size_type pos = fFilePath.find_last_of ("/\\");
		if (pos != string::npos)
			result = EmDirRef (fFilePath.substr (0, pos + 1));
	}

	return result;
}


string
EmFileRef::GetFullPath (void) const
{
	return fFilePath;
}


bool
EmFileRef::operator== (const EmFileRef& other) const
{
	return _stricmp (fFilePath.c_str (), other.fFilePath.c_str ()) == 0;
}


bool
EmFileRef::operator!= (const EmFileRef& other) const
{
	return _stricmp (fFilePath.c_str (), other.fFilePath.c_str ()) != 0;
}


bool
EmFileRef::operator> (const EmFileRef& other) const
{
	return _stricmp (fFilePath.c_str (), other.fFilePath.c_str ()) < 0;
}


bool
EmFileRef::operator< (const EmFileRef& other) const
{
	return _stricmp (fFilePath.c_str (), other.fFilePath.c_str ()) > 0;
}


bool
EmFileRef::FromPrefString (const string& s)
{
	fFilePath = s;

	return true;
}


string
EmFileRef::ToPrefString (void) const
{
	return fFilePath;
}


void
EmFileRef::MaybePrependCurrentDirectory (void)
{
	// Check for absolute path: /path, X:path, or X:\path
	if (!fFilePath.empty () &&
		fFilePath[0] != '/' &&
		fFilePath[0] != '\\' &&
		!(fFilePath.size () >= 2 && fFilePath[1] == ':'))
	{
		char* buffer = _getcwd (NULL, 0);
		if (buffer)
		{
			string cwd (buffer);
			free (buffer);

			// Normalize backslashes
			for (size_t i = 0; i < cwd.size (); ++i)
				if (cwd[i] == '\\')
					cwd[i] = '/';

			if (!cwd.empty () && cwd[cwd.size () - 1] != '/')
				fFilePath = cwd + "/" + fFilePath;
			else
				fFilePath = cwd + fFilePath;
		}
	}
}


void
EmFileRef::MaybeNormalize (void)
{
	// Remove leading double slashes
	if (fFilePath.size () >= 2 &&
		fFilePath[0] == '/' &&
		fFilePath[1] == '/')
	{
		fFilePath.erase (fFilePath.begin ());
	}

	// Remove leading double backslashes (but not UNC paths)
	if (fFilePath.size () >= 2 &&
		fFilePath[0] == '\\' &&
		fFilePath[1] == '\\')
	{
		fFilePath.erase (fFilePath.begin ());
	}
}
