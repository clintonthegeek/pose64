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
#include "EmDirRefUnix.h"

#include "EmFileRef.h"
#include "Platform.h"

#include <windows.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <direct.h>
#include <dirent.h>

using namespace std;

/***********************************************************************
 *
 * FUNCTION:	EmDirRef::EmDirRef
 *
 ***********************************************************************/

EmDirRef::EmDirRef (void) :
	fDirPath ()
{
}


EmDirRef::EmDirRef (const EmDirRef& other) :
	fDirPath (other.fDirPath)
{
}


EmDirRef::EmDirRef (const char* path) :
	fDirPath (path)
{
	this->MaybeAppendSlash ();
}


EmDirRef::EmDirRef (const string& path) :
	fDirPath (path)
{
	this->MaybeAppendSlash ();
}


EmDirRef::EmDirRef (const EmDirRef& parent, const char* path) :
	fDirPath (parent.GetFullPath () + path)
{
	this->MaybeAppendSlash ();
}


EmDirRef::EmDirRef (const EmDirRef& parent, const string& path) :
	fDirPath (parent.GetFullPath () + path)
{
	this->MaybeAppendSlash ();
}


EmDirRef::~EmDirRef (void)
{
}


EmDirRef&
EmDirRef::operator= (const EmDirRef& other)
{
	if (&other != this)
	{
		fDirPath = other.fDirPath;
	}

	return *this;
}


Bool
EmDirRef::IsSpecified (void) const
{
	return !fDirPath.empty ();
}


Bool
EmDirRef::Exists (void) const
{
	if (this->IsSpecified ())
	{
		DIR* dir = opendir (fDirPath.c_str ());
		if (dir)
		{
			closedir (dir);
			return true;
		}
	}

	return false;
}


void
EmDirRef::Create (void) const
{
	if (!this->Exists () && this->IsSpecified ())
	{
		EmDirRef	parent = this->GetParent ();
		parent.Create ();

		if (_mkdir (fDirPath.c_str ()) != 0)
		{
			// !!! throw...
		}
	}
}


string
EmDirRef::GetName (void) const
{
	string	result;

	if (this->IsSpecified () && fDirPath != "/" && fDirPath.size () > 3)
	{
		string	dirPath (fDirPath);
		dirPath.resize (dirPath.size () - 1);

		string::size_type pos = dirPath.find_last_of ("/\\");
		if (pos != string::npos)
			result = dirPath.substr (pos + 1, string::npos);
	}

	return result;
}


EmDirRef
EmDirRef::GetParent (void) const
{
	EmDirRef	result;

	if (this->IsSpecified () && fDirPath != "/" && fDirPath.size () > 3)
	{
		string	dirPath (fDirPath);
		dirPath.resize (dirPath.size () - 1);

		string::size_type pos = dirPath.find_last_of ("/\\");
		if (pos != string::npos)
			result = EmDirRef (dirPath.substr (0, pos + 1));
	}

	return result;
}


string
EmDirRef::GetFullPath (void) const
{
	return fDirPath;
}


void
EmDirRef::GetChildren (EmFileRefList* fileList, EmDirRefList* dirList) const
{
	DIR* dir = opendir (fDirPath.c_str ());
	if (dir)
	{
		struct dirent* ent;
		while ((ent = readdir (dir)) != NULL)
		{
			if (strcmp (ent->d_name, ".") == 0)
				continue;

			if (strcmp (ent->d_name, "..") == 0)
				continue;

			string full_path (fDirPath + ent->d_name);
			struct stat buf;
			stat (full_path.c_str (), &buf);

			if (S_ISDIR (buf.st_mode))
			{
				if (dirList)
					dirList->push_back (EmDirRef (*this, ent->d_name));
			}
			else
			{
				if (fileList)
					fileList->push_back (EmFileRef (*this, ent->d_name));
			}
		}

		closedir (dir);
	}
}


bool
EmDirRef::operator== (const EmDirRef& other) const
{
	return _stricmp (fDirPath.c_str (), other.fDirPath.c_str ()) == 0;
}


bool
EmDirRef::operator!= (const EmDirRef& other) const
{
	return _stricmp (fDirPath.c_str (), other.fDirPath.c_str ()) != 0;
}


bool
EmDirRef::operator> (const EmDirRef& other) const
{
	return _stricmp (fDirPath.c_str (), other.fDirPath.c_str ()) < 0;
}


bool
EmDirRef::operator< (const EmDirRef& other) const
{
	return _stricmp (fDirPath.c_str (), other.fDirPath.c_str ()) > 0;
}


bool
EmDirRef::FromPrefString (const string& s)
{
	fDirPath = s;
	this->MaybeAppendSlash ();

	return true;
}


string
EmDirRef::ToPrefString (void) const
{
	return fDirPath;
}


EmDirRef
EmDirRef::GetEmulatorDirectory (void)
{
	const char*	dir = getenv ("POSER_DIR");
	if (dir != NULL)
		return EmDirRef (dir);

	// Resolve from the executable's location via GetModuleFileName.
	char buf[MAX_PATH];
	DWORD len = GetModuleFileNameA (NULL, buf, MAX_PATH);
	if (len > 0 && len < MAX_PATH)
	{
		string exePath (buf);
		// Normalize backslashes to forward slashes
		for (size_t i = 0; i < exePath.size (); ++i)
			if (exePath[i] == '\\')
				exePath[i] = '/';
		string::size_type pos = exePath.rfind ('/');
		if (pos != string::npos)
			return EmDirRef (exePath.substr (0, pos));
	}

	// Fall back to APPDATA
	dir = getenv ("APPDATA");
	if (dir != NULL)
		return EmDirRef (string (dir) + "/pose64");

	// Last resort
	return EmDirRef (".");
}


EmDirRef
EmDirRef::GetPrefsDirectory (void)
{
	return GetEmulatorDirectory ();
}


void
EmDirRef::MaybeAppendSlash (void)
{
	if (this->IsSpecified ())
	{
		char last = fDirPath[fDirPath.size () - 1];
		if (last != '/' && last != '\\')
		{
			fDirPath += '/';
		}
	}
}
