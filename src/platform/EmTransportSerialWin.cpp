/* -*- mode: C++; tab-width: 4 -*- */
/* Windows serial transport stubs for POSE64.
   Serial communication is not yet implemented on Windows.
   All methods return success (errNone / 0) so the emulator
   can link and run without serial support. */

#include "EmCommon.h"
#include "EmTransportSerial.h"

#include <string>

using namespace std;


void EmTransportSerial::HostConstruct (void)
{
	fHost = NULL;
}

void EmTransportSerial::HostDestruct (void)
{
	fHost = NULL;
}

ErrCode EmTransportSerial::HostOpen (void)
{
	return errNone;
}

ErrCode EmTransportSerial::HostClose (void)
{
	return errNone;
}

ErrCode EmTransportSerial::HostRead (long& len, void* data)
{
	len = 0;
	return errNone;
}

ErrCode EmTransportSerial::HostWrite (long& len, const void* data)
{
	len = 0;
	return errNone;
}

long EmTransportSerial::HostBytesInBuffer (long /*minBytes*/)
{
	return 0;
}

string EmTransportSerial::GetPtySlaveName (void) const
{
	return string ();
}

ErrCode EmTransportSerial::HostSetConfig (const ConfigSerial& config)
{
	return errNone;
}

void EmTransportSerial::HostSetRTS (RTSControl /*state*/)
{
}

void EmTransportSerial::HostSetDTR (Bool /*state*/)
{
}

void EmTransportSerial::HostSetBreak (Bool /*state*/)
{
}

Bool EmTransportSerial::HostGetCTS (void)
{
	return false;
}

Bool EmTransportSerial::HostGetDSR (void)
{
	return false;
}

void EmTransportSerial::HostGetPortNameList (PortNameList& results)
{
	results.clear ();
}

void EmTransportSerial::HostGetSerialBaudList (BaudList& results)
{
	results.clear ();
	results.push_back (115200);
	results.push_back (57600);
	results.push_back (38400);
	results.push_back (19200);
	results.push_back (9600);
}
