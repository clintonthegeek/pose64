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

#ifndef StringConversions_h
#define StringConversions_h

#include "EmDevice.h"			// EmDevice

#include <string>

class EmDirRef;
class EmFileRef;
class EmTransportDescriptor;


// ----------------------------------------------------------------------
//	Our preferences routines need to convert our preference data to and
//	from string format (the format in which the data is stored on disk).
//	Following are a bunch of overloaded routines for converting strings
//	into all the data types we use for preferences.
// ----------------------------------------------------------------------

bool FromString (const std::string& s, bool& value);
bool FromString (const std::string& s, char& value);
bool FromString (const std::string& s, signed char& value);
bool FromString (const std::string& s, unsigned char& value);
bool FromString (const std::string& s, signed short& value);
bool FromString (const std::string& s, unsigned short& value);
bool FromString (const std::string& s, signed int& value);
bool FromString (const std::string& s, unsigned int& value);
bool FromString (const std::string& s, signed long& value);
bool FromString (const std::string& s, unsigned long& value);
bool FromString (const std::string& s, std::string& value);
bool FromString (const std::string& s, char* value);
bool FromString (const std::string& s, CloseActionType& value);
bool FromString (const std::string& s, EmDevice& value);
bool FromString (const std::string& s, EmDirRef& value);
bool FromString (const std::string& s, EmErrorHandlingOption& value);
bool FromString (const std::string& s, EmFileRef& value);
bool FromString (const std::string& s, EmTransportDescriptor& value);

// ----------------------------------------------------------------------
//	Our preferences routines need to convert our preference data to and
//	from string format (the format in which the data is stored on disk).
//	Following are a bunch of overloaded routines for converting data
//	(in all the types we use for preferences) into strings.
// ----------------------------------------------------------------------

std::string ToString (bool value);
std::string ToString (char value);
std::string ToString (signed char value);
std::string ToString (unsigned char value);
std::string ToString (signed short value);
std::string ToString (unsigned short value);
std::string ToString (signed int value);
std::string ToString (unsigned int value);
std::string ToString (signed long value);
std::string ToString (unsigned long value);
std::string ToString (const std::string& value);
std::string ToString (const char* value);
std::string ToString (CloseActionType value);
std::string ToString (const EmDevice& value);
std::string ToString (const EmDirRef& value);
std::string ToString (EmErrorHandlingOption value);
std::string ToString (const EmFileRef& value);
std::string ToString (const EmTransportDescriptor& value);

#endif	/* StringConversions_h */
