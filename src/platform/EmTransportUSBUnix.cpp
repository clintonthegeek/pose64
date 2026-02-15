/* -*- mode: C++; tab-width: 4 -*- */
/* Qt PORT: USB transport stubs for Unix */

#include "EmCommon.h"
#include "EmTransportUSB.h"

using namespace std;

// Qt PORT: USB transport is not implemented yet
// These are stubs to allow linking

void EmTransportUSB::HostConstruct()
{
}

void EmTransportUSB::HostDestruct()
{
}

ErrCode EmTransportUSB::HostOpen()
{
	return errNone;  // TODO: Implement USB support
}

ErrCode EmTransportUSB::HostClose()
{
	return errNone;
}

ErrCode EmTransportUSB::HostRead(long& len, void* data)
{
	len = 0;
	return errNone;
}

ErrCode EmTransportUSB::HostWrite(long& len, const void* data)
{
	len = 0;
	return errNone;
}

Bool EmTransportUSB::HostCanRead()
{
	return false;
}

long EmTransportUSB::HostBytesInBuffer(long minBytes)
{
	return 0;
}

ErrCode EmTransportUSB::HostSetConfig(const ConfigUSB& config)
{
	return errNone;
}

Bool EmTransportUSB::HostHasUSB()
{
	return false;  // No USB support in this build
}
