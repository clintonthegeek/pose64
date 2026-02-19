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
#include "EmTransportSerialUnix.h"

#include "Logging.h"			// LogSerial
#include "Platform.h"			// Platform::AllocateMemory

#include <errno.h>				// errno
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>			// select, fd_set
#include <fcntl.h>				// open(), close()
#include <termios.h>			// struct termios
#include <stdlib.h>				// posix_openpt, grantpt, unlockpt, ptsname
#include <dirent.h>				// opendir, readdir
#include <string.h>				// strlen

#include <algorithm>			// max, copy
#include <string>

using namespace std;


#define PRINTF	if (!LogSerial ()) ; else LogAppendMsg


/***********************************************************************
 *
 * FUNCTION:	EmTransportSerial::HostConstruct
 *
 * DESCRIPTION:	Construct platform-specific objects/data.
 *
 * PARAMETERS:	None
 *
 * RETURNED:	The platform-specific serial object.
 *
 ***********************************************************************/

void EmTransportSerial::HostConstruct (void)
{
	fHost = new EmHostTransportSerial;
}


/***********************************************************************
 *
 * FUNCTION:	EmTransportSerial::HostDestruct
 *
 * DESCRIPTION:	Destroy platform-specific objects/data.
 *
 * PARAMETERS:	hostData - The platform-specific serial object.
 *
 * RETURNED:	Nothing
 *
 ***********************************************************************/

void EmTransportSerial::HostDestruct (void)
{
	delete fHost;
	fHost = NULL;
}


/***********************************************************************
 *
 * FUNCTION:	EmTransportSerial::HostOpen
 *
 * DESCRIPTION:	Open the serial port in a platform-specific fashion.
 *
 * PARAMETERS:	None
 *
 * RETURNED:	0 if no error.
 *
 ***********************************************************************/

ErrCode EmTransportSerial::HostOpen (void)
{
	ErrCode	err = fHost->OpenCommPort (fConfig);

	if (!err)
		err = fHost->CreateCommThreads (fConfig);

	if (err)
		this->HostClose ();

	return err;
}


/***********************************************************************
 *
 * FUNCTION:	EmTransportSerial::HostClose
 *
 * DESCRIPTION:	Close the serial port in a platform-specific fashion.
 *
 * PARAMETERS:	None
 *
 * RETURNED:	0 if no error.
 *
 ***********************************************************************/

ErrCode EmTransportSerial::HostClose (void)
{
	ErrCode err;

	err = fHost->DestroyCommThreads ();
	err = fHost->CloseCommPort ();

	return err;
}


/***********************************************************************
 *
 * FUNCTION:	EmTransportSerial::HostRead
 *
 * DESCRIPTION:	Read bytes from the port in a platform-specific fashion.
 *
 * PARAMETERS:	len - maximum number of bytes to read.
 *				data - buffer to receive the bytes.
 *
 * RETURNED:	0 if no error.  The number of bytes actually read is
 *				returned in len if there was no error.
 *
 ***********************************************************************/

ErrCode EmTransportSerial::HostRead (long& len, void* data)
{
	fHost->GetIncomingData (data, len);

	return errNone;
}


/***********************************************************************
 *
 * FUNCTION:	EmTransportSerial::HostWrite
 *
 * DESCRIPTION:	Write bytes to the port in a platform-specific fashion.
 *
 * PARAMETERS:	len - number of bytes in the buffer.
 *				data - buffer containing the bytes.
 *
 * RETURNED:	0 if no error.  The number of bytes actually written is
 *				returned in len if there was no error.
 *
 ***********************************************************************/

ErrCode EmTransportSerial::HostWrite (long& len, const void* data)
{
	fHost->PutOutgoingData (data, len);

	return errNone;
}


/***********************************************************************
 *
 * FUNCTION:	EmTransportSerial::HostBytesInBuffer
 *
 * DESCRIPTION:	Returns the number of bytes that can be read with the
 *				Read method.  Note that bytes may be received in
 *				between the time BytesInBuffer is called and the time
 *				Read is called, so calling the latter with the result
 *				of the former is not guaranteed to fetch all received
 *				and buffered bytes.
 *
 * PARAMETERS:	None
 *
 * RETURNED:	Number of bytes that can be read.
 *
 ***********************************************************************/

long EmTransportSerial::HostBytesInBuffer (long /*minBytes*/)
{
	return fHost->IncomingDataSize ();
}


string EmTransportSerial::GetPtySlaveName (void) const
{
	if (fHost)
		return fHost->fPtySlaveName;

	return string ();
}


/***********************************************************************
 *
 * FUNCTION:	EmTransportSerial::HostSetConfig
 *
 * DESCRIPTION:	Configure the serial port in a platform-specific
 *				fashion.  The port is assumed to be open.
 *
 * PARAMETERS:	config - configuration information.
 *
 * RETURNED:	0 if no error.
 *
 ***********************************************************************/

ErrCode EmTransportSerial::HostSetConfig (const ConfigSerial& config)
{
	PRINTF ("EmTransportSerial::HostSetConfig: Setting settings.");

	ErrCode	err = errNone;

	struct termios		io;

	// Get the current settings.

	if (tcgetattr (fHost->fCommHandle, &io) == -1)
	{
		err = errno;
		return err;
	}

	// Always set these for serial operation.

	io.c_cflag |= (CREAD | CLOCAL);

	// Turn off these for "raw" (as opposed to "canonical") mode.

	io.c_lflag &= ~(ICANON | ECHO | ISIG);

	// Set all input/output flags to zero for raw mode.

	io.c_iflag = io.c_oflag = 0;

	// Set the baud

	int hostBaud = fHost->GetBaud (config.fBaud);
	cfsetospeed (&io, hostBaud);
	cfsetispeed (&io, hostBaud);

	// Set the parity

	if (config.fParity == EmTransportSerial::kNoParity)
	{
		io.c_cflag &= ~PARENB;
	}
	else
	{
		io.c_cflag |= PARENB;

		if (config.fParity == EmTransportSerial::kOddParity)
		{
			io.c_cflag |= PARODD;
		}
		else
		{
			io.c_cflag &= ~PARODD;
		}
	}

	// Set the data bits

	io.c_cflag &= ~CSIZE;
	io.c_cflag |= fHost->GetDataBits (config.fDataBits);

	// Set the stop bits

	if (config.fStopBits == 2)
	{
		io.c_cflag |= CSTOPB;
	}
	else
	{
		io.c_cflag &= ~CSTOPB;
	}

	// Set the hardware handshaking.
	// For PTY mode, skip flow control (PTYs don't need CRTSCTS).

	if (fHost->fPtyMaster == 0)
	{
		if (config.fHwrHandshake)
		{
			io.c_cflag |= CRTSCTS;
		}
		else
		{
			io.c_cflag &= ~CRTSCTS;
		}
	}

	// Write out the changed settings.
	if (tcsetattr (fHost->fCommHandle, TCSANOW, &io) == -1)
	{
		err = errno;
		return err;
	}

	return err;
}


/***********************************************************************
 *
 * FUNCTION:	EmTransportSerial::HostSetRTS
 *
 * DESCRIPTION:	.
 *
 * PARAMETERS:	.
 *
 * RETURNED:	Nothing
 *
 ***********************************************************************/

void EmTransportSerial::HostSetRTS (RTSControl /*state*/)
{
}


/***********************************************************************
 *
 * FUNCTION:	EmTransportSerial::HostSetDTR
 *
 * DESCRIPTION:	.
 *
 * PARAMETERS:	.
 *
 * RETURNED:	Nothing
 *
 ***********************************************************************/

void EmTransportSerial::HostSetDTR (Bool /*state*/)
{
}


/***********************************************************************
 *
 * FUNCTION:	EmTransportSerial::HostSetBreak
 *
 * DESCRIPTION:	.
 *
 * PARAMETERS:	.
 *
 * RETURNED:	Nothing
 *
 ***********************************************************************/

void EmTransportSerial::HostSetBreak (Bool /*state*/)
{
}


/***********************************************************************
 *
 * FUNCTION:	EmTransportSerial::HostGetCTS
 *
 * DESCRIPTION:	.
 *
 * PARAMETERS:	.
 *
 * RETURNED:	Nothing
 *
 ***********************************************************************/

Bool EmTransportSerial::HostGetCTS (void)
{
	return false;
}


/***********************************************************************
 *
 * FUNCTION:	EmTransportSerial::HostGetDSR
 *
 * DESCRIPTION:	.
 *
 * PARAMETERS:	.
 *
 * RETURNED:	Nothing
 *
 ***********************************************************************/

Bool EmTransportSerial::HostGetDSR (void)
{
	return false;
}


/***********************************************************************
 *
 * FUNCTION:	EmTransportSerial::HostGetPortNameList
 *
 * DESCRIPTION:	Return the list of serial ports on this computer.  Used
 *				to prepare a menu of serial port choices.
 *
 * PARAMETERS:	nameList - port names are added to this list.
 *
 * RETURNED:	Nothing
 *
 ***********************************************************************/

void EmTransportSerial::HostGetPortNameList (PortNameList& results)
{
	results.clear ();

	// Always offer a virtual PTY port for HotSync
	results.push_back ("pty:HotSync");

	// Scan /sys/class/tty/ for real serial ports.
	// Real serial ports (and USB-serial adapters) have a "device" symlink
	// in their sysfs entry, which filters out virtual consoles and ptys.

	DIR* dir = opendir ("/sys/class/tty");
	if (dir)
	{
		struct dirent* entry;
		while ((entry = readdir (dir)) != NULL)
		{
			if (entry->d_name[0] == '.')
				continue;

			// Check for a "device" symlink which indicates a real port
			string sysPath = string("/sys/class/tty/") + entry->d_name + "/device";
			struct stat st;
			if (lstat (sysPath.c_str (), &st) == 0)
			{
				string devPath = string("/dev/") + entry->d_name;
				results.push_back (devPath);
			}
		}
		closedir (dir);
	}
}


/***********************************************************************
 *
 * FUNCTION:	EmTransportSerial::HostGetSerialBaudList
 *
 * DESCRIPTION:	Return the list of baud rates support by this computer.
 *				Used to prepare a menu of baud rate choices.
 *
 * PARAMETERS:	baudList - baud rates are added to this list.
 *
 * RETURNED:	Nothing
 *
 ***********************************************************************/

void EmTransportSerial::HostGetSerialBaudList (BaudList& results)
{
	results.clear ();
	results.push_back (115200);
	results.push_back (57600);
	results.push_back (38400);
	results.push_back (19200);
	results.push_back (9600);
}


#pragma mark -

/***********************************************************************
 *
 * FUNCTION:	EmHostTransportSerial c'tor
 *
 * DESCRIPTION:	Constructor.  Initialize our data members.
 *
 * PARAMETERS:	None
 *
 * RETURNED:	Nothing
 *
 ***********************************************************************/

EmHostTransportSerial::EmHostTransportSerial (void) :
	fReadThread (NULL),
	fWriteThread (NULL),
	fCommHandle (0),
	fCommSignalPipeA (0),
	fCommSignalPipeB (0),
	fTimeToQuit (false),
	fPtyMaster (0),
	fDataMutex (),
	fDataCondition (&fDataMutex),
	fReadMutex (),
	fReadBuffer (),
	fWriteMutex (),
	fWriteBuffer ()
{
}


/***********************************************************************
 *
 * FUNCTION:	EmHostTransportSerial d'tor
 *
 * DESCRIPTION:	Destructor.  Delete our data members.
 *
 * PARAMETERS:	None
 *
 * RETURNED:	Nothing
 *
 ***********************************************************************/

EmHostTransportSerial::~EmHostTransportSerial (void)
{
	EmAssert (fReadThread == NULL);
	EmAssert (fWriteThread == NULL);
	EmAssert (fCommHandle == 0);
	EmAssert (fCommSignalPipeA == 0);
	EmAssert (fCommSignalPipeB == 0);
}


/***********************************************************************
 *
 * FUNCTION:	EmHostTransportSerial::OpenCommPort
 *
 * DESCRIPTION:	Open the serial port.
 *
 * PARAMETERS:	config - data block describing which port to use.
 *
 * RETURNED:	0 if no error.
 *
 ***********************************************************************/

ErrCode EmHostTransportSerial::OpenCommPort (const EmTransportSerial::ConfigSerial& config)
{
	EmTransportSerial::PortName	portName = config.fPort;

	PRINTF ("EmTransportSerial::HostOpen: attempting to open port \"%s\"",
			portName.c_str());

	if (portName.empty ())
	{
		PRINTF ("EmTransportSerial::HostOpen: No port selected in the Properties dialog box...");
		return -1;
	}

	// Check for PTY mode: port names starting with "pty:"
	if (portName.compare (0, 4, "pty:") == 0)
	{
		return OpenPtyPort (portName);
	}

	// Real serial port
	PRINTF ("EmTransportSerial::HostOpen: Opening serial port...");

	fCommHandle = open (portName.c_str (), O_RDWR | O_NOCTTY | O_NDELAY);

	if (fCommHandle <= 0)
	{
		fCommHandle = 0;
		return errno;
	}

	return errNone;
}


/***********************************************************************
 *
 * FUNCTION:	EmHostTransportSerial::OpenPtyPort
 *
 * DESCRIPTION:	Create a PTY pair for virtual serial communication.
 *				The master fd is used by the emulator; the slave path
 *				is printed to stderr so the user can connect HotSync
 *				tools to it.
 *
 * PARAMETERS:	portName - the pty: prefixed port name.
 *
 * RETURNED:	0 if no error.
 *
 ***********************************************************************/

ErrCode EmHostTransportSerial::OpenPtyPort (const string& portName)
{
	PRINTF ("EmTransportSerial::HostOpen: Creating PTY for \"%s\"...",
			portName.c_str());

	int master = posix_openpt (O_RDWR | O_NOCTTY);
	if (master < 0)
	{
		PRINTF ("EmTransportSerial::HostOpen: posix_openpt failed: %s",
				strerror (errno));
		return errno;
	}

	if (grantpt (master) != 0)
	{
		close (master);
		return errno;
	}

	if (unlockpt (master) != 0)
	{
		close (master);
		return errno;
	}

	const char* slaveName = ptsname (master);
	if (!slaveName)
	{
		close (master);
		return errno;
	}

	fPtySlaveName = slaveName;

	fprintf (stderr, "SERIAL: PTY created for \"%s\" — connect HotSync tools to: %s\n",
			portName.c_str(), slaveName);

	fCommHandle = master;
	fPtyMaster = master;

	return errNone;
}


/***********************************************************************
 *
 * FUNCTION:	EmHostTransportSerial::CreateCommThreads
 *
 * DESCRIPTION:	Create the threads that asynchronously read from and
 *				write to the serial port.
 *
 * PARAMETERS:	config - data block describing which port to use.
 *
 * RETURNED:	0 if no error.
 *
 ***********************************************************************/

ErrCode EmHostTransportSerial::CreateCommThreads (const EmTransportSerial::ConfigSerial& /*config*/)
{
	if (fCommHandle)
	{
		PRINTF ("EmTransportSerial::HostOpen: Creating serial port handler threads...");

		// Create the pipe used to communicate with CommRead.

		int filedes[] = { 0, 0 };
		if (pipe (filedes) == 0)
		{
			fCommSignalPipeA = filedes[0];	// for reading
			fCommSignalPipeB = filedes[1];	// for writing
		}

		// Create the threads and start them up.

		fTimeToQuit = false;
		fReadThread = omni_thread::create (CommRead, this);
		fWriteThread = omni_thread::create (CommWrite, this);
	}

	return errNone;
}


/***********************************************************************
 *
 * FUNCTION:	EmHostTransportSerial::DestroyCommThreads
 *
 * DESCRIPTION:	Shutdown and destroy the comm threads.
 *
 * PARAMETERS:	None.
 *
 * RETURNED:	0 if no error.
 *
 ***********************************************************************/

ErrCode EmHostTransportSerial::DestroyCommThreads (void)
{
	// If never created, nothing to destroy.

	if (!fCommSignalPipeA)
		return errNone;

	// Signal the threads to quit.

	fDataMutex.lock ();

	fTimeToQuit = true;

	int dummy = 0;
	write (fCommSignalPipeB, &dummy, sizeof (dummy));		// Signals CommRead.

	fDataCondition.broadcast ();	// Signals CommWrite.
	fDataMutex.unlock ();

	// Wait for the threads to quit.

	if (fReadThread)
	{
		fReadThread->join (NULL);
		fWriteThread->join (NULL);
	}

	// Thread objects are owned by us after join, so delete them.

	delete fReadThread;
	delete fWriteThread;

	fReadThread = NULL;
	fWriteThread = NULL;

	// Close the signal pipe.

	close (fCommSignalPipeA);
	close (fCommSignalPipeB);

	fCommSignalPipeA = fCommSignalPipeB = 0;

	return errNone;
}


/***********************************************************************
 *
 * FUNCTION:	EmHostTransportSerial::CloseCommPort
 *
 * DESCRIPTION:	Close the comm port.
 *
 * PARAMETERS:	None.
 *
 * RETURNED:	0 if no error.
 *
 ***********************************************************************/

ErrCode EmHostTransportSerial::CloseCommPort (void)
{
	(void) close (fCommHandle);

	fCommHandle = 0;
	fPtyMaster = 0;
	fPtySlaveName.clear ();

	return errNone;
}


/***********************************************************************
 *
 * FUNCTION:	EmHostTransportSerial::PutIncomingData
 *
 * DESCRIPTION:	Thread-safe method for adding data to the queue that
 *				holds data read from the serial port.
 *
 * PARAMETERS:	data - pointer to the read data.
 *				len - number of bytes pointed to by "data".
 *
 * RETURNED:	Nothing
 *
 ***********************************************************************/

void EmHostTransportSerial::PutIncomingData	(const void* data, long& len)
{
	if (len == 0)
		return;

	omni_mutex_lock lock (fReadMutex);

	char*	begin = (char*) data;
	char*	end = begin + len;
	while (begin < end)
		fReadBuffer.push_back (*begin++);
}


/***********************************************************************
 *
 * FUNCTION:	EmHostTransportSerial::GetIncomingData
 *
 * DESCRIPTION:	Thread-safe method for getting data from the queue
 *				holding data read from the serial port.
 *
 * PARAMETERS:	data - pointer to buffer to receive data.
 *				len - on input, number of bytes available in "data".
 *					On exit, number of bytes written to "data".
 *
 * RETURNED:	Nothing
 *
 ***********************************************************************/

void EmHostTransportSerial::GetIncomingData	(void* data, long& len)
{
	omni_mutex_lock lock (fReadMutex);

	if (len > (long) fReadBuffer.size ())
		len = (long) fReadBuffer.size ();

	char*	p = (char*) data;
	deque<char>::iterator	begin = fReadBuffer.begin ();
	deque<char>::iterator	end = begin + len;

	copy (begin, end, p);
	fReadBuffer.erase (begin, end);
}


/***********************************************************************
 *
 * FUNCTION:	EmHostTransportSerial::IncomingDataSize
 *
 * DESCRIPTION:	Thread-safe method returning the number of bytes in the
 *				read queue.
 *
 * PARAMETERS:	None.
 *
 * RETURNED:	Number of bytes in the read queue.
 *
 ***********************************************************************/

long EmHostTransportSerial::IncomingDataSize (void)
{
	omni_mutex_lock lock (fReadMutex);

	return fReadBuffer.size ();
}


/***********************************************************************
 *
 * FUNCTION:	EmHostTransportSerial::PutOutgoingData
 *
 * DESCRIPTION:	Thread-safe method for adding data to the queue that
 *				holds data to be written to the serial port.
 *
 * PARAMETERS:	data - pointer to the read data.
 *				len - number of bytes pointed to by "data".
 *
 * RETURNED:	Nothing
 *
 ***********************************************************************/

void EmHostTransportSerial::PutOutgoingData	(const void* data, long& len)
{
	if (len == 0)
		return;

	omni_mutex_lock lock (fWriteMutex);

	char*	begin = (char*) data;
	char*	end = begin + len;
	while (begin < end)
		fWriteBuffer.push_back (*begin++);

	// Wake up CommWrite.

	fDataMutex.lock ();
	fDataCondition.broadcast ();
	fDataMutex.unlock ();
}


/***********************************************************************
 *
 * FUNCTION:	EmHostTransportSerial::GetOutgoingData
 *
 * DESCRIPTION:	Thread-safe method for getting data from the queue
 *				holding data to be written to the serial port.
 *
 * PARAMETERS:	data - pointer to buffer to receive data.
 *				len - on input, number of bytes available in "data".
 *					On exit, number of bytes written to "data".
 *
 * RETURNED:	Nothing
 *
 ***********************************************************************/

void EmHostTransportSerial::GetOutgoingData	(void* data, long& len)
{
	omni_mutex_lock lock (fWriteMutex);

	if (len > (long) fWriteBuffer.size ())
		len = (long) fWriteBuffer.size ();

	char*	p = (char*) data;
	deque<char>::iterator	begin = fWriteBuffer.begin ();
	deque<char>::iterator	end = begin + len;

	copy (begin, end, p);
	fWriteBuffer.erase (begin, end);
}


/***********************************************************************
 *
 * FUNCTION:	EmHostTransportSerial::OutgoingDataSize
 *
 * DESCRIPTION:	Thread-safe method returning the number of bytes in the
 *				write queue.
 *
 * PARAMETERS:	None.
 *
 * RETURNED:	Number of bytes in the read queue.
 *
 ***********************************************************************/

long EmHostTransportSerial::OutgoingDataSize (void)
{
	omni_mutex_lock lock (fWriteMutex);

	return fWriteBuffer.size ();
}


/***********************************************************************
 *
 * FUNCTION:	EmHostTransportSerial::CommRead
 *
 * DESCRIPTION:	This function sits in its own thread, waiting for data
 *				to show up in the serial port.  If data arrives, this
 *				function plucks it out and stores it in a thread-safe
 *				queue.  It quits when it detects that the comm handle
 *				has been deleted.
 *
 * PARAMETERS:	data - pointer to owning EmHostTransportSerial.
 *
 * RETURNED:	Nothing.
 *
 ***********************************************************************/

void EmHostTransportSerial::CommRead (void* data)
{
	EmHostTransportSerial*	This = (EmHostTransportSerial*) data;

	PRINTF ("CommRead starting.");

	while (!This->fTimeToQuit)
	{
		int		status;
		int		fd1 = This->fCommHandle;
		int		fd2 = This->fCommSignalPipeA;
		int		maxfd = max (fd1, fd2);
		fd_set	read_fds;

		FD_ZERO (&read_fds);
		FD_SET (fd1, &read_fds);
		FD_SET (fd2, &read_fds);

		status = select (maxfd + 1, &read_fds, NULL, NULL, NULL);

		if (This->fTimeToQuit)
			break;

		if (status > 0)	// data available
		{
			if (FD_ISSET (fd1, &read_fds))
			{
				char	buf[1024];
				int		len = 1024;
				len = read (fd1, buf, len);

				if (len <= 0)
					break; // port closed or error

				// Log the data.
				if (LogSerialData ())
					LogAppendData (buf, len, "EmHostTransportSerial::CommRead: Received data:");
				else
					PRINTF ("EmHostTransportSerial::CommRead: Received %d serial bytes.", len);

				// Add the data to the EmHostTransportSerial object's buffer.
				long	n = (long) len;
				This->PutIncomingData (buf, n);
			}
		}
	}

	PRINTF ("CommRead exitting.");
}


/***********************************************************************
 *
 * FUNCTION:	EmHostTransportSerial::CommWrite
 *
 * DESCRIPTION:	This function sits in its own thread, waiting for data
 *				to show up in the write queue.  If data arrives, this
 *				function plucks it out and sends it out to the port.  It
 *				quits when it detects that the comm handle has been
 *				deleted.
 *
 * PARAMETERS:	data - pointer to owning EmHostTransportSerial.
 *
 * RETURNED:	Nothing.
 *
 ***********************************************************************/

void EmHostTransportSerial::CommWrite (void* data)
{
	EmHostTransportSerial*	This = (EmHostTransportSerial*) data;

	PRINTF ("CommWrite starting.");

	// We manage fDataMutex manually instead of using omni_mutex_lock RAII.
	// The condition variable requires fDataMutex to be held for wait(),
	// but we MUST release it before touching fWriteMutex (via
	// OutgoingDataSize / GetOutgoingData).  Otherwise we deadlock with
	// PutOutgoingData, which locks fWriteMutex then fDataMutex.

	This->fDataMutex.lock ();

	while (!This->fTimeToQuit)
	{
		This->fDataCondition.wait ();

		// It's the EmHostTransportSerial object telling us to quit.

		if (This->fTimeToQuit)
			break;

		// Release fDataMutex before accessing the write buffer.
		// PutOutgoingData locks fWriteMutex → fDataMutex; holding
		// fDataMutex while locking fWriteMutex would be ABBA deadlock.

		This->fDataMutex.unlock ();

		// Get the data to write.

		long	len = This->OutgoingDataSize ();

		if (len > 0)
		{
			// Get the data.

			void*	buf = Platform::AllocateMemory (len);
			This->GetOutgoingData (buf, len);

			// Log the data.

			if (LogSerialData ())
				LogAppendData (buf, len, "EmHostTransportSerial::CommWrite: Transmitted data:");
			else
				PRINTF ("EmHostTransportSerial::CommWrite: Transmitted %ld serial bytes.", len);

			// Write the data.

			::write (This->fCommHandle, buf, len);

			// Dispose of the data.

			Platform::DisposeMemory (buf);
		}

		// Reacquire fDataMutex before calling wait() again.

		This->fDataMutex.lock ();
	}

	This->fDataMutex.unlock ();

	PRINTF ("CommWrite exitting.");
}


/***********************************************************************
 *
 * FUNCTION:	EmHostTransportSerial::GetBaud
 *
 * DESCRIPTION:	Map a baud rate into the Unix constant that represents
 *				that rate in a termios call.
 *
 * PARAMETERS:	baud - raw baud rate.
 *
 * RETURNED:	Unix constant that represents that rate.
 *
 ***********************************************************************/

int EmHostTransportSerial::GetBaud (EmTransportSerial::Baud baud)
{
	switch (baud)
	{
#if defined (B150)
		case    150:		PRINTF ("	Baud = 150");		return B150;
#endif

#if defined (B300)
		case    300:		PRINTF ("	Baud = 300");		return B300;
#endif

#if defined (B600)
		case    600:		PRINTF ("	Baud = 600");		return B600;
#endif

#if defined (B1200)
		case   1200:		PRINTF ("	Baud = 1200");		return B1200;
#endif

#if defined (B1800)
		case   1800:		PRINTF ("	Baud = 1800");		return B1800;
#endif

#if defined (B2400)
		case   2400:		PRINTF ("	Baud = 2400");		return B2400;
#endif

#if defined (B4800)
		case   4800:		PRINTF ("	Baud = 4800");		return B4800;
#endif

#if defined (B9600)
		case   9600:		PRINTF ("	Baud = 9600");		return B9600;
#endif

#if defined (B19200)
		case  19200:		PRINTF ("	Baud = 19200");		return B19200;
#endif

#if defined (B38400)
		case  38400:		PRINTF ("	Baud = 38400");		return B38400;
#endif

#if defined (B57600)
		case  57600:		PRINTF ("	Baud = 57600");		return B57600;
#endif

#if defined (B115200)
		case 115200:		PRINTF ("	Baud = 115200");	return B115200;
#endif

#if defined (B230400)
		case 230400:		PRINTF ("	Baud = 230400");	return B230400;
#endif
	}

	PRINTF ("	Unknown Baud value: %ld.", (long) baud);

	return baud;
}


/***********************************************************************
 *
 * FUNCTION:	EmHostTransportSerial::GetDataBits
 *
 * DESCRIPTION:	Map a dataBits value into the Unix constant that
 *				represents that value in a termios call.
 *
 * PARAMETERS:	dataBits - raw data bits value.
 *
 * RETURNED:	Unix constant that represents that dataBits value.
 *
 ***********************************************************************/

int EmHostTransportSerial::GetDataBits (EmTransportSerial::DataBits dataBits)
{
	switch (dataBits)
	{
		case 5:				PRINTF ("	dataBits = 5");		return CS5;
		case 6:				PRINTF ("	dataBits = 6");		return CS6;
		case 7:				PRINTF ("	dataBits = 7");		return CS7;
		case 8:				PRINTF ("	dataBits = 8");		return CS8;
	}

	PRINTF ("	Unknown DataBits value.");
	EmAssert (false);
	return 0;
}
