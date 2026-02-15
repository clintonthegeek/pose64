/* -*- mode: C++; tab-width: 4 -*- */
/* Qt PORT: Serial transport stubs for Unix */

#include "EmCommon.h"
#include "EmTransportSerial.h"

using namespace std;

// Qt PORT: Serial transport is not implemented yet
// These are stubs to allow linking

void EmTransportSerial::HostConstruct()
{
}

void EmTransportSerial::HostDestruct()
{
}

ErrCode EmTransportSerial::HostOpen()
{
	return errNone;
}

ErrCode EmTransportSerial::HostClose()
{
	return errNone;
}

ErrCode EmTransportSerial::HostRead(long& len, void* data)
{
	len = 0;
	return errNone;
}

ErrCode EmTransportSerial::HostWrite(long& len, const void* data)
{
	len = 0;
	return errNone;
}

long EmTransportSerial::HostBytesInBuffer(long minBytes)
{
	return 0;
}

ErrCode EmTransportSerial::HostSetConfig(const ConfigSerial& config)
{
	return errNone;
}

void EmTransportSerial::HostSetRTS(RTSControl rts)
{
}

void EmTransportSerial::HostSetDTR(Bool dtr)
{
}

void EmTransportSerial::HostSetBreak(Bool brk)
{
}

Bool EmTransportSerial::HostGetCTS()
{
	return false;
}

Bool EmTransportSerial::HostGetDSR()
{
	return false;
}

void EmTransportSerial::HostGetPortNameList(EmTransportSerial::PortNameList& portNames)
{
	// No serial ports available
	portNames.clear();
}

void EmTransportSerial::HostGetSerialBaudList(EmTransportSerial::BaudList& baudList)
{
	// Standard baud rates
	baudList.clear();
	baudList.push_back(9600);
	baudList.push_back(19200);
	baudList.push_back(38400);
	baudList.push_back(57600);
	baudList.push_back(115200);
}
