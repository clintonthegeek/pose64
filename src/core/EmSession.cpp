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
#include "EmSession.h"

#include "ChunkFile.h"			// ChunkFile
#include "EmApplication.h"		// gApplication, GetBoundDevice, etc.
#include "EmCPU.h"				// EmCPU::Execute
#include "EmDocument.h"			// gDocument
#include "EmErrCodes.h"			// kError_InvalidSessionFile
#include "EmEventPlayback.h"	// EmEventPlayback::ReplayingEvents
#include "EmException.h"		// EmExceptionTopLevelAction
#include "EmHAL.h"				// EmHAL::ButtonEvent (ReleaseBootKeys)
#include "EmMemory.h"			// Memory::ResetBankHandlers
#include "EmMinimize.h"			// EmMinimize::RealLoadInitialState
#include "EmStreamFile.h"		// EmStreamFile
#include "ErrorHandling.h"		// Errors::Throw
#include "Hordes.h"				// Hordes::AutoSaveState, etc.
#include "Logging.h"			// LogAppendMsg
#include "Miscellaneous.h"		// EmValueChanger
#include "PreferenceMgr.h"		// Preference
#include "ROMStubs.h"			// EvtWakeup
#include "SessionFile.h"		// SessionFile
#include "Strings.r.h"			// kStr_EnterPen

#include "EmMemory.h"			// Memory::Initialize ();
#include "Platform.h"			// Platform::Initialize ();
#include "DebugMgr.h"			// Debug::Initialize ();
#include "HostControlPrv.h"		// Host::Initialize ();
#include "EmScreen.h"			// EmScreen::Initialize ();
#include "ErrorHandling.h"		// Errors::Initialize ();
#include "EmPalmOS.h"			// EmPalmOS::Initialize

using namespace std;

EmSession*	gSession;

#ifndef NDEBUG
Bool	gIterating = false;
#endif

Bool	PrvCanBotherCPU	(void);
void	PrvWakeUpCPU	(long strID);


/*
	Sub-system methods:

		Startup:
			Called just once when Poser is started.

		Initialize:
			Called just once when a session is created.  Will be followed
			by one or more Reset or Load calls.

		Reset:
			Called any time the Reset menu item is selected or the SysReset
			function is called.

		Save:
			Called to save the state to a file.  May be called when the user
			selects the Save menu item, when the user closes the session,
			or as part of a Gremlin Horde auto-saving sequence.

		Load:
			Called to restore the saved state from a file.  Can assume that
			Initialize has been called first.

		Dispose:
			Called just once when a session is closed.  May be called on a
			partially constructed session, so Dispose methods should be
			prepared to handle NULL pointers, etc.

		Shutdown:
			Called just once when Poser quits.
*/


// ---------------------------------------------------------------------------
//		� EmSession::EmSession
// ---------------------------------------------------------------------------
// EmSession constructor.  Initialize data members and point the global
// "current EmSession" pointer to us.  This method does not explicitly
// throw any exceptions, and really shouldn't fail unless we've exhausted
// the free store.

EmSession::EmSession (void) :
	fConfiguration (),
	fFile (),
	fCPU (NULL),
#if HAS_OMNI_THREAD
	fThread (NULL),
	fSharedLock (),
	fSharedCondition (&fSharedLock),
	fSleepLock (),
	fSleepCondition (&fSleepLock),
	fStop (false),
#endif
	fSuspendState (),
	fState (kStopped),
	fBreakOnSysCall (false),
	fNestLevel (0),
	fReset (false),
	fResetBanks (false),
	fHordeAutoSaveState (false),
	fHordeSaveRootState (false),
	fHordeSaveSuspendState (false),
	fHordeLoadRootState (false),
	fHordeNextGremlinFromRootState (false),
	fHordeNextGremlinFromSuspendState (false),
	fMinimizeLoadState (false),
	fDeferredErrs (),
	fResetType (kResetSys),
	fKeyQueue (),
	fPenQueue (),
	fLastPenEvent (EmPoint (-1, -1), false),
	fBootKeys (0)
{
	fSuspendState.fAllCounters = 0;

	Preference<long> prefSpeed (kPrefKeyEmulationSpeed);
	long speed = *prefSpeed;

	// Migrate old encoding (1=1x, 2=2x, 4=4x, 8=8x) to percentage encoding.
	switch (speed)
	{
		case 1: speed = 100; break;
		case 2: speed = 200; break;
		case 4: speed = 400; break;
		case 8: speed = 800; break;
	}

	fEmulationSpeed.store ((int) speed, std::memory_order_relaxed);

	EmAssert (gSession == NULL);
	gSession = this;
}


// ---------------------------------------------------------------------------
//		� EmSession::~EmSession
// ---------------------------------------------------------------------------
// EmSession destructor.  Stop the CPU thread and release all resources.  
// Clear the global "current EmSession" pointer.  This method really shouldn't
// throw any exceptions or in any other way fail.

EmSession::~EmSession (void)
{
	this->DestroyThread ();
	this->Dispose ();

	// Delete the CPU object here instead of in Dispose.  When reloading a
	// saved state as part of a Gremlin Horde procedure,  EmSession::Load
	// is called.  That method calls Dispose, Initialize, and then the
	// various Load methods of all the subsystems.  This process is
	// performed while the current session and cpu objects are active (that
	// is, functions belonging to them are on the stack).  Since Dispose is
	// called, it would be unwise for Dispose to delete the active cpu object.

	delete fCPU;
	fCPU = NULL;

	EmAssert (gSession == this);
	gSession = NULL;
}


#pragma mark -

// ---------------------------------------------------------------------------
//		� EmSession::CreateNew
// ---------------------------------------------------------------------------
// Create a new session based on the given configuration.  This method can
// throw an exception if the creation attemp fails (for instance, the
// configuration is invalid, or if the free store is exhausted).

void EmSession::CreateNew (const Configuration& cfg)
{
	EmAssert (!gApplication->IsBound ());

	this->Initialize (cfg);
	this->Reset (kResetSoft);
}


// ---------------------------------------------------------------------------
//		� EmSession::CreateOld
// ---------------------------------------------------------------------------
// Create a new session based on information stored in the given disk file.
// If possible, the entire previously-saved state is restored.  However, if
// any critical information is missing from the file, the emulated state may
// be reset so that it can be used (as if the user had used a pin to reset
// an actual device).  If the file is corrupted or missing even the basic
// saved-state information, and exception is thrown.

void EmSession::CreateOld (const EmFileRef& ref)
{
	EmAssert (!gApplication->IsBound ());

	EmStreamFile	stream (ref, kOpenExistingForRead);
	ChunkFile		chunkFile (stream);
	SessionFile		sessionFile (chunkFile);

	// Load enough information so that we can initialize the system.

	Configuration	cfg;
	if (!sessionFile.ReadConfiguration (cfg))
	{
		Errors::Throw (kError_InvalidSessionFile);
	}

	this->Initialize (cfg);

	// Now load the saved state.

	this->Load (sessionFile);

	// Remember who we are.

	fFile = ref;
}


// ---------------------------------------------------------------------------
//		� EmSession::CreateBound
// ---------------------------------------------------------------------------
// Create a new session based on information stored in resources or other data
// attached to the emulator executable.

void EmSession::CreateBound (void)
{
	EmAssert (gApplication->IsBound ());
	EmAssert (gApplication);

	Configuration	cfg;

	cfg.fDevice		= gApplication->GetBoundDevice ();
	cfg.fRAMSize	= gApplication->GetBoundRAMSize ();
//	cfg.fROMFile ignored in Initialize (gets the ROM from the resource)

	// Initialize the system.

	this->Initialize (cfg);

	// If there is session data, read that in and continue initializing
	// (like in CreateOld).

	if (gApplication->IsBoundFully ())
	{
		Chunk	psf;
		gApplication->GetPSFResource (psf);

		EmStreamBlock	stream (psf.GetPointer (), psf.GetLength ());
		ChunkFile		chunkFile (stream);
		SessionFile 	sessionFile (chunkFile);

		// Now load the saved state.

		this->Load (sessionFile);
	}

	// Otherwise, reset the session (like in CreateNew).

	else
	{
		this->Reset (kResetSoft);
	}
}


#pragma mark -

// ---------------------------------------------------------------------------
//		� EmSession::Initialize
// ---------------------------------------------------------------------------
// Called by the various CreateFoo methods to initialize our state according
// to the given configuration.

void EmSession::Initialize (const Configuration& cfg)
{
	// Set the hardware device here.  We need to have it set up now
	// for things like EmBankSRAM::Initialize and Memory::Initialize,
	// which need to know what mode they're in.

	fConfiguration = cfg;

	// Ideally, we can initialize sub-systems in any order.

	EmAssert (!fCPU);
	fCPU = this->GetDevice ().CreateCPU (this);

	// If ROM is an embedded resouce, use it.

	if (gApplication->IsBound ())
	{
		Chunk	rom;

#ifndef NDEBUG
		Bool	resourceLoaded = gApplication->GetROMResource (rom);
		EmAssert (resourceLoaded);
#else
		gApplication->GetROMResource (rom);
#endif

		EmStreamChunk	stream (rom);
		Memory::Initialize (stream, cfg.fRAMSize);
	}

	// If ROM is not embedded, use the filespec.

	else
	{
		EmStreamFile	stream (cfg.fROMFile, kOpenExistingForRead);
		Memory::Initialize (stream, cfg.fRAMSize);
	}

	Platform::Initialize ();
	Debug::Initialize ();
	Host::Initialize ();
	EmScreen::Initialize ();
	Errors::Initialize ();

	EmPalmOS::Initialize ();
}


// ---------------------------------------------------------------------------
//		� EmSession::Dispose
// ---------------------------------------------------------------------------

void EmSession::Dispose (void)
{
	// Ideally, we can dispose sub-systems in any order.  However,
	// it's probably a good idea to dispose them in the reverse
	// order in which they were initialized.
	//
	// Note also that this is called from the destructor, which could
	// be called on an object that was not completely initialized.
	// Therefore, each Dispose method should be prepared to handle
	// NULL pointers, etc.

	EmPalmOS::Dispose ();

	Errors::Dispose ();
	EmScreen::Dispose ();
	Host::Dispose ();
	Debug::Dispose ();
	Platform::Dispose ();

	Memory::Dispose ();

	fInstructionBreakFuncs.clear ();
	fDataBreakFuncs.clear ();

	this->ClearDeferredErrors ();
}


#pragma mark -

// ---------------------------------------------------------------------------
//		� EmSession::Reset
// ---------------------------------------------------------------------------

void EmSession::Reset (EmResetType resetType)
{
	/*
		React to the various ways to reset as follows:

		kResetSys
			Not much to do here.  We reset our internal state, but don't
			reset any hardware registers.

		kResetSoft
			Same as above, but we also reset the hardware registers.

		kResetHard
			Same as above, but we also force the wiping out of the storage
			heap by simulating the Power key down.

		kResetDebug
			Same as kResetSoft, but we also force the entering of the
			Debugger by simulating the Page Down key down.


		NOTES ON VARIOUS RESET SEQUENCES:
		---------------------------------
		The ROM has three entry points for resetting: SoftReset, HardReset,
		and DebugReset.  From the ROM's comments:

		// These two entry points are the soft and hard reset entry points
		// The soft reset is called when the processor comes out of reset or
		//  when the SysReset call is executed.
		// The hard reset is called when the SysColdBoot call is executed.

		I can't find that DebugReset is called.

		SoftReset sets D0 to 0 and branches to Reset.  HardReset sets D0 to
		-1 and branches to Reset.  DebugReset sets D0 to 1 and falls through
		to Reset.  When Reset calls InitStage1, this value is passed as the
		hardResetOrDebug parameter.

		InitStage1 checks to see if hardResetOrDebug is 1.  If so, it sets
		it to zero, sets enterDebugger to true, and sets sysResetFlagGoingToDebugger
		in GSysResetFlags.

		Later in InitStage1, the Power key is checked (by calling KeyBootKeys)
		and hardResetOrDebug is set to true (that is, 1) if it's down.  Also,
		the Page Down key is checked and enterDebugger is set to true if it's
		down.  (As an aside, the Page Up key is checked and the sysResetFlagNoExtensions
		bit of GSysResetFlags is set if it's down.)  After all this, if enterDebugger
		is true, DbgBreak is called.  Finally, InitStage2 is called with hardResetOrDebug
		as the parameter.

		InitStage2 merely calls SysLaunch with the value of the parameter
		passed to it.  At this point, the parameter is merely a Boolean
		called "hardReset".

		SysLaunch checks the state of the storage heap.  If the heap looks bad,
		then hardReset is forced to true.  Next, if hardReset is true because
		of the Power key, the user is asked if they want to erase the device's
		contents (they are NOT asked this if the heap looks bad).  Depending
		on their choice (by pressing the Page Up key or not), hardReset is set
		to true or false.

		Finally if hardReset is true, MemCardFormat is called.
	*/

	EmAssert (fNestLevel == 0);

	// !!! Need to re-establish any pressed buttons.

	// Perform any last minute cleanup.

	// If we're resetting while running a Gremlin, save the events.

	if (Hordes::IsOn ())
	{
		Hordes::SaveEvents ();
	}

	// Ideally, we can reset sub-systems in any order.	However,
	// it's probably a good idea to reset them in the same order
	// in which they were initialized.

	// Reset Memory *before* CPU, as the CPU looks at memory location
	// zero to find it's reset address.  Memory can't be accessed until
	// the first call to Memory::ResetBankHandlers, which occurs at the
	// end of Memory::Reset.

	Memory::Reset ((resetType & kResetTypeMask) != kResetSys);

	EmAssert (fCPU);
	fCPU->Reset ((resetType & kResetTypeMask) != kResetSys);

	Platform::Reset ();
	Debug::Reset ();
	Host::Reset ();
	EmScreen::Reset ();
	Errors::Reset ();

	EmPalmOS::Reset ();


#if HAS_OMNI_THREAD
	omni_mutex_lock	lock (fSharedLock);
#endif

	// Now reset self.

//	fSuspendState.fCounters.fSuspendByUIThread			= 0;
	fSuspendState.fCounters.fSuspendByDebugger			= 0;
	fSuspendState.fCounters.fSuspendByExternal			= 0;
//	fSuspendState.fCounters.fSuspendByTimeout			= 0;
	fSuspendState.fCounters.fSuspendBySysCall			= 0;
	fSuspendState.fCounters.fSuspendBySubroutineReturn	= 0;

	fBreakOnSysCall = false;
	fNestLevel = 0;

	fReset = false;
	fResetBanks = false;
	fHordeAutoSaveState = false;
	fHordeSaveRootState = false;
	fHordeSaveSuspendState = false;
	fHordeLoadRootState = false;
	fHordeNextGremlinFromRootState = false;
	fHordeNextGremlinFromSuspendState = false;
	fMinimizeLoadState = false;

	this->ClearDeferredErrors ();

	fResetType = kResetSys;

	// Don't clear these out on a SysReset call.  kResetSys
	// is also issued when transitioning between the small and
	// big ROMs.  In that case, we don't want to clear out the
	// button queues in case we have a pending button up event
	// to complete a button down event used for triggering a
	// Hard, Debug, or No Extensions reset.

	if ((resetType & kResetTypeMask) != kResetSys)
	{
		this->ClearButtonState ();
		fKeyQueue.Clear ();
		fPenQueue.Clear ();
	}

	fLastPenEvent = EmPenEvent (EmPoint (-1, -1), false);

	// All of meta-memory gets wiped out on reset; re-establish these.

	this->InstallInstructionBreaks ();
	this->InstallDataBreaks ();

	// Deal with the emulation of the pressing of the keys that
	// modify the Reset sequence.  We set the hardware bits here.
	// After it looks like the key state has been read, the hardware
	// emulation routines call EmSession::ReleaseBootKeys, where we
	// reverse the bit setting.

	fBootKeys = 0;

	if ((resetType & kResetTypeMask) == kResetHard)
	{
		EmHAL::ButtonEvent (kElement_PowerButton, true);
		fBootKeys |= 1L << kElement_PowerButton;
	}
	else if ((resetType & kResetTypeMask) == kResetDebug)
	{
		EmHAL::ButtonEvent (kElement_DownButton, true);
		fBootKeys |= 1L << kElement_DownButton;
	}

	if ((resetType & kResetExtMask) == kResetNoExt)
	{
		EmHAL::ButtonEvent (kElement_UpButton, true);
		fBootKeys |= 1L << kElement_UpButton;
	}
}


// ---------------------------------------------------------------------------
//		� EmSession::Save
// ---------------------------------------------------------------------------

void EmSession::Save (SessionFile& f)
{
	// Write out the device type.

	EmAssert (fConfiguration.fDevice.Supported ());
	f.WriteDevice (fConfiguration.fDevice);

	// Ideally, we can save sub-systems in any order.  However,
	// it's probably a good idea to save them in the same order
	// in which they were initialized.

	EmAssert (fCPU);
	fCPU->Save (f);
	Memory::Save (f);

	Platform::Save (f);
	Debug::Save (f);
	Host::Save (f);
	EmScreen::Save (f);
	Errors::Save (f);

	EmPalmOS::Save (f);
}


// ---------------------------------------------------------------------------
//		� EmSession::Load
// ---------------------------------------------------------------------------

void EmSession::Load (SessionFile& f)
{
	// Load the saved state from the session file.	First, set the flag
	// that says whether or not we can successfully restart from the
	// information in this file.  As parts are loaded, the various
	// sub-systems will have a chance to veto this optimistic assumption.

	f.SetCanReload (true);

	// Ideally, we can load sub-systems in any order.  However,
	// it's probably a good idea to load them in the same order
	// in which they were initialized.

	// Load Memory before EmCPU.  That way, the memory system will be
	// initialized when we restore the CPU registers (which will include
	// a sanity check of the PC).

	Memory::Load (f);

	EmAssert (fCPU);
	fCPU->Load (f);

	Platform::Load (f);
	Debug::Load (f);
	Host::Load (f);
	EmScreen::Load (f);
	Errors::Load (f);

	EmPalmOS::Load (f);

	// If we weren't able to get all the pieces from the file we needed,
	// force a reset.

	if (!f.GetCanReload ())
	{
		this->Reset (kResetSoft);
		fNeedPostLoad = false;
	}

	// Otherwise, set a flag saying that "post load" activities need to
	// take place.  These activities normally take place while the device
	// is first booted up.  However, any side-effect of those activities
	// may no longer be valid (for instance, we may need to install or
	// remove a 'gdbS' Feature).
	//
	// We set a flag to schedule these activities rather than performing
	// them now, as the emulated state may not be in any shape to respond
	// to them.  For instance, the OS may not be able to handle a call to
	// FtrSet at the moment.  Therefore, we check for this flag at the next
	// convenient moment and take action at that time.

	else
	{
		fNeedPostLoad = true;
	}
}


// ---------------------------------------------------------------------------
//		� EmSession::Save
// ---------------------------------------------------------------------------

void EmSession::Save (const EmFileRef& ref, Bool updateFileRef)
{
	EmStreamFile	stream (ref, kCreateOrEraseForUpdate,
						kFileCreatorEmulator, kFileTypeSession);
	ChunkFile		chunkFile (stream);
	SessionFile		sessionFile (chunkFile);

	this->Save (sessionFile);

	if (updateFileRef)
	{
		fFile = ref;
	}
}


// ---------------------------------------------------------------------------
//		� EmSession::Load
// ---------------------------------------------------------------------------

void EmSession::Load (const EmFileRef& ref)
{
	EmStreamFile	stream (ref, kOpenExistingForRead);
	ChunkFile		chunkFile (stream);
	SessionFile		sessionFile (chunkFile);

	this->Load (sessionFile);
}


#pragma mark -

// ---------------------------------------------------------------------------
//		� EmSession::CreateThread
// ---------------------------------------------------------------------------

void EmSession::CreateThread (Bool suspended)
{
#if HAS_OMNI_THREAD
	if (fThread)
		return;

	// Initialize some variables that control the thread state.

	fStop				= false;

	fSuspendState.fAllCounters	= 0;
	fSuspendState.fCounters.fSuspendByUIThread	= suspended ? 1 : 0;
	fState						= suspended ? kSuspended : kRunning;

	// Create the thread and start it running.

	fThread = new omni_thread (&EmSession::RunStatic, this);
	fThread->start ();
#else

	// Initialize some variables that control the thread state.

	fSuspendState.fAllCounters	= 0;
	fSuspendState.fCounters.fSuspendByUIThread	= suspended ? 1 : 0;
	fState						= kSuspended;
#endif
}


// ---------------------------------------------------------------------------
//		� EmSession::DestroyThread
// ---------------------------------------------------------------------------

void EmSession::DestroyThread (void)
{
#if HAS_OMNI_THREAD
	if (!fThread)
		return;

	omni_mutex_lock	lock (fSharedLock);

	fStop = true;
	fSuspendState.fCounters.fSuspendByUIThread++;
	fSharedCondition.broadcast ();

	while (fState != kStopped)
		fSharedCondition.wait ();

	// fThread thread will quit and destroy itself.

	fThread = NULL;
#endif
}


// ---------------------------------------------------------------------------
//		� EmSession::SuspendThread
// ---------------------------------------------------------------------------
// Suspend the thread in the requested fashion.  Returns true if the attempt
// succeeded and the thread needs to be resumed with a call to ResumeThread.
// Returns false if the thread could not or was not suspended, and
// ResumeThread should not be called.

Bool EmSession::SuspendThread (EmStopMethod how)
{
	if (how == kStopNone)
		return false;

#if HAS_OMNI_THREAD

	EmAssert (fThread);

	omni_mutex_lock	lock (fSharedLock);

	// Set a flag for the CPU thread to find, telling it how to stop.
	//
	// !!! What to do when fSuspendByUIThread, fSuspendByDebugger or
	// fSuspendByExternal are set, especially if how == kStopOnSysCall?

	Bool	desiredBreakOnSysCall = false;

//	LogAppendMsg ("EmSession::SuspendThread (enter): fState = %ld", (long) fState);

	switch (how)
	{
		case kStopNone:
			EmAssert (false);
			break;

		case kStopNow:
			fSuspendState.fCounters.fSuspendByUIThread++;
			break;

		case kStopOnCycle:
			fSuspendState.fCounters.fSuspendByUIThread++;
			break;

		case kStopOnSysCall:
			desiredBreakOnSysCall = true;
			break;
	}

	// Force the CPU to check spcflags so it notices the suspend request,
	// even if it's currently in a nested subroutine (where CYCLE is skipped).
	EmAssert (fCPU);
	fCPU->CheckAfterCycle ();

	// Get it to a suspended or blocked state, if not there already.
	// Run the CPU loop until we are suspended or blocked on the UI.
	//
	// Note: if we're doing a kStopOnSysCall and the
	// the thread's already been suspended by, say, kStopNow, then
	// we might want to start the thread up again if it's OK.

	if (fState == kRunning)
	{
		// Wake up the thread if it's sleeping.

		fSleepLock.lock ();
		fSleepCondition.broadcast ();
		fSleepLock.unlock ();

		// Wait for it to stop.
		// !!! Do a timed wait in case we never reach the desired stop point?

		while (fState == kRunning)
		{
			// Establish these inside the while loop.  It's possible for them
			// to get cleared in Reset, and we don't want that.
			//
			// More specifically, we're trying to prevent a condition that can
			// occur when ending a CodeWarrior debug session.  After clicking
			// on the X button in CodeWarrior, we get an RPC packet for
			// SysReset.  In handling that packet, Poser merely sets the
			// PC to the SysReset function address.  When the CPU is
			// restarted, it starts executing that code.  When SysReset
			// gets to the ultimate JSR to the reset vector, Poser notices
			// that and call gSession->Reset(false).  EmSession::Reset
			// clears the fBreakOnSysCall flag.
			//
			// In the meantime, CodeWarrior has broken the socket connection
			// to us.  Poser responds to that by trying to unregister the
			// 'gdbS' feature.  In order to do that, it must halt the CPU
			// on a system call.  On my PC, the disconnect and call to
			// FtrUnregister can happen *before* we get to this code here.
			//
			// So, here is the UI thread, waiting for a system call to
			// occur.  Before that can happen, the Reset code executes,
			// clearing the fBreakOnSysCall flag!  Thus, the UI thread keeps
			// waiting for a system call to occur, while the CPU thread no
			// longer thinks it needs to halt on that condition.

			fBreakOnSysCall	= desiredBreakOnSysCall;
			fSharedCondition.broadcast ();

			fSharedCondition.wait ();
//			LogAppendMsg ("EmSession::SuspendThread (waking): fState = %ld", (long) fState);

#ifndef NDEBUG
			if (!this->IsNested ())
			{
				if (how == kStopNow)
				{
					EmAssert (fSuspendState.fCounters.fSuspendByUIThread != 0 ||
						this->fState == kBlockedOnUI);
				}
				else if (how == kStopOnCycle)
				{
					EmAssert (fSuspendState.fCounters.fSuspendByUIThread != 0);
				}
			}
#endif
		}
	}

#else

	// Set a flag for the CPU thread to find, telling it how to stop.
	//
	// !!! What to do when fSuspendByUIThread, fSuspendByDebugger or
	// fSuspendByExternal are set, especially if how == kStopOnSysCall?

	switch (how)
	{
		case kStopNone:
			EmAssert (false);
			break;

		case kStopNow:
			fSuspendState.fCounters.fSuspendByUIThread++;
			break;

		case kStopOnCycle:
			fSuspendState.fCounters.fSuspendByUIThread++;
			break;

		case kStopOnSysCall:
			fBreakOnSysCall = true;
			break;
	}

	// Run the CPU loop until we break at the right point.

	fSuspendState.fCounters.fSuspendByTimeout = 0;

	while (fState == kSuspended && fSuspendState.fAllCounters == 0)
	{
		this->ExecuteIncremental ();

		// Ignore this suspend condition.

		fSuspendState.fCounters.fSuspendByTimeout = 0;
	}

#endif

	// Resulting state should be kSuspended or kBlockedOnUI.  It should never
	// be kStopped, because we didn't set the fStop flag.

	EmAssert (fState == kSuspended || fState == kBlockedOnUI);


	// Make sure we stopped the way we wanted to.
	//
	// If appropriate, clear the flag that got us here and say that the UI
	// thread stopped us.  If any other flags remain set, the client needs
	// to deal with that.

	Bool	result = true;	// assume we stopped OK

	switch (how)
	{
		case kStopNone:
			EmAssert (false);
			break;

		case kStopNow:
			// Stopped on either "suspended" or "blocked" is OK.
			result = true;
			break;

		case kStopOnCycle:
			// Only "stopped on suspended" is OK
			result = (fState == kSuspended);
			break;

		case kStopOnSysCall:
			// Must be "stopped on suspended" and on a system call.
			result = (fState == kSuspended) && fSuspendState.fCounters.fSuspendBySysCall;
			if (result)
			{
				fSuspendState.fCounters.fSuspendByUIThread++;
			}
			break;
	}

	fBreakOnSysCall = false;

	if (result)
	{
		EmAssert (fSuspendState.fCounters.fSuspendByUIThread > 0);
		EmAssert (fSuspendState.fCounters.fSuspendBySubroutineReturn == 0);
		EmAssert (fNestLevel == 0 || fState == kBlockedOnUI);	// (If blocked on UI, fNestLevel may be > 0).
	}

//	LogAppendMsg ("EmSession::SuspendThread (exit): fState = %ld", (long) fState);

	return result;
}


// ---------------------------------------------------------------------------
//		� EmSession::ResumeThread
// ---------------------------------------------------------------------------

void EmSession::ResumeThread (void)
{
#if HAS_OMNI_THREAD
	EmAssert (fThread);

	omni_mutex_lock	lock (fSharedLock);
#endif

	if (fSuspendState.fCounters.fSuspendByUIThread > 0)
	{
		--fSuspendState.fCounters.fSuspendByUIThread;

		if (fSuspendState.fCounters.fSuspendByUIThread == 0 &&
			fSuspendState.fCounters.fSuspendByExternal == 0)
		{
			fSuspendState.fCounters.fSuspendBySysCall = 0;
		}

#if HAS_OMNI_THREAD
		if (fSuspendState.fAllCounters == 0)
		{
			// Don't change the state if it's kBlockedOnUI.
			if (fState == kSuspended)
			{
				fState = kRunning;
			}
		}

		fSharedCondition.broadcast ();
#endif
	}
}


// ---------------------------------------------------------------------------
//		� EmSession::Sleep
// ---------------------------------------------------------------------------

#if HAS_OMNI_THREAD
void EmSession::Sleep (unsigned long msecs)
{
	const unsigned long	kMillisecondsPerSecond = 1000;
	const unsigned long	kNanosecondsPerMillisecond = 1000000;

	unsigned long	secs = msecs / kMillisecondsPerSecond;
	unsigned long	nsecs = (msecs % kMillisecondsPerSecond) * kNanosecondsPerMillisecond;

	fThread->get_time (&secs, &nsecs, secs, nsecs);

	fSleepLock.lock ();
	fSleepCondition.timedwait (secs, nsecs);
	fSleepLock.unlock ();
}
#endif


// ---------------------------------------------------------------------------
//		� EmSession::InCPUThread
// ---------------------------------------------------------------------------

#if HAS_OMNI_THREAD
Bool EmSession::InCPUThread (void) const
{
	return (omni_thread::self () == fThread);
}
#endif


#pragma mark -

// ---------------------------------------------------------------------------
//		� EmSession::GetSessionState
// ---------------------------------------------------------------------------

EmSessionState EmSession::GetSessionState (void) const
{
#if HAS_OMNI_THREAD
	omni_mutex_lock	lock (fSharedLock);
#endif

	return fState;
}


// ---------------------------------------------------------------------------
//		� EmSession::GetSuspendState
// ---------------------------------------------------------------------------

EmSuspendState EmSession::GetSuspendState (void) const
{
#if HAS_OMNI_THREAD
	omni_mutex_lock	lock (fSharedLock);

	if (this->InCPUThread ())
		EmAssert (fState == kRunning);
	else
		EmAssert ((fNestLevel == 0 && fState != kRunning) || (fNestLevel > 0 && fState == kRunning));
#endif

	return fSuspendState;
}


// ---------------------------------------------------------------------------
//		� EmSession::SetSuspendState
// ---------------------------------------------------------------------------

void EmSession::SetSuspendState (const EmSuspendState& s)
{
#if HAS_OMNI_THREAD
	omni_mutex_lock	lock (fSharedLock);

	if (this->InCPUThread ())
		EmAssert (fState == kRunning);
	else
		EmAssert ((fNestLevel == 0 && fState != kRunning) || (fNestLevel > 0 && fState == kRunning));
#endif

	fSuspendState = s;
}


#pragma mark -

// ---------------------------------------------------------------------------
//		� EmSession::ExecuteIncremental
// ---------------------------------------------------------------------------

void EmSession::ExecuteIncremental (void)
{
//	LogAppendMsg ("EmSession::ExecuteIncremental (enter): fState = %ld", (long) fState);

	fSuspendState.fCounters.fSuspendByTimeout = 0;

	EmAssert (fState == kSuspended || fState == kBlockedOnUI);
	EmAssert (fSuspendState.fCounters.fSuspendByTimeout == 0);
	EmAssert (fNestLevel == 0);

	// Enter the CPU loop only if it's not suspended.  The thread could
	// be suspended if fSuspendByUIThread, fSuspendByDebugger or
	// fSuspendByExternal are true.

	if (fState == kBlockedOnUI)
		return;

	if (fSuspendState.fAllCounters == 0)
	{
		try
		{
			this->CallCPU ();
		}
		catch (EmExceptionReset& e)
		{
			e.Display ();
			e.DoAction ();
		}
		catch (EmExceptionTopLevelAction& e)
		{
			e.DoAction ();
		}
		catch (...)
		{
			EmAssert (false);
		}

		/*
			Check the reason for EmCPU::Execute returning:

			fSuspendByUIThread
				Should not happen.  That flag is set if the UI thread asks
				the CPU thread to suspend.  ExecuteIncremental is called
				on the Mac with no multiple threads.

			fSuspendByDebugger
				Could happen.  Let it make this function exit.  The debugger
				should already have been notified.

			fSuspendByExternal
				Could happen.  Let it make this function exit.

			fSuspendByTimeout
				Could happen.  Let it make this function exit.

			fSuspendBySysCall
				Could happen.  Let it make this function exit.

			fSuspendBySubroutineReturn
				Should not happen.  Should occur only on calls to
				ExecuteSubroutine.
		*/

		EmAssert (fSuspendState.fCounters.fSuspendByUIThread == 0);
	}

	EmAssert (fState == kSuspended);
	EmAssert (fSuspendState.fCounters.fSuspendBySubroutineReturn == 0);
	EmAssert (fNestLevel == 0);

//	LogAppendMsg ("EmSession::ExecuteIncremental (exit): fState = %ld", (long) fState);
}


// ---------------------------------------------------------------------------
//		� EmSession::ExecuteSubroutine
// ---------------------------------------------------------------------------

void EmSession::ExecuteSubroutine (void)
{
#if HAS_OMNI_THREAD
	omni_mutex_lock	lock (fSharedLock);
#endif

	EmAssert (fNestLevel >= 0);

//	LogAppendMsg ("EmSession::ExecuteSubroutine (enter): fState = %ld", (long) fState);

#if HAS_OMNI_THREAD
	if (this->InCPUThread ())
		EmAssert (fState == kRunning);
	else
		EmAssert ((fNestLevel == 0 && fState != kRunning) || (fNestLevel > 0 && fState == kRunning));
#endif

	EmSuspendCounters	oldState = fSuspendState.fCounters;
	fSuspendState.fAllCounters = 0;

	while (fSuspendState.fAllCounters == 0)
	{
		// Enter new scope so that the omni_mutex_unlock will re-lock
		// the mutex before we look at fSuspendState, etc.
		{
			EmValueChanger<int>	oldNestLevel (fNestLevel, fNestLevel + 1);

#if HAS_OMNI_THREAD
			fSharedCondition.broadcast ();

			omni_mutex_unlock	unlock (fSharedLock);
#endif

			this->CallCPU ();
		}

		/*
			Check the reason for EmCPU::Execute returning:

			fSuspendByUIThread
			fSuspendByExternal
			fSuspendByTimeout
			fSuspendBySysCall
				Could happen.  Remember that it occurred, clear it, and
				re-enter the CPU loop.  On the way out of this function
				re-establish the request.

			fSuspendByDebugger
				Could happen.  Let it make this function exit.

			fSuspendBySubroutineReturn
				Could happen.  Let it make this function exit.
		*/

		/*
			If fSuspendByUIThread is set, the bridge thread's PaintScreen
			is trying to suspend us.  DON'T clear it here — let it stay
			set so the while loop exits.  The counter will remain in the
			live state where ResumeThread can find and decrement it.
			We break immediately so Run() can set fState=kSuspended.
		*/

		if (fSuspendState.fCounters.fSuspendByUIThread)
		{
			// Leave fSuspendByUIThread in the live state — don't touch it
			break;
		}

		oldState.fSuspendByDebugger += fSuspendState.fCounters.fSuspendByDebugger;

		oldState.fSuspendByExternal += fSuspendState.fCounters.fSuspendByExternal;
		fSuspendState.fCounters.fSuspendByExternal = 0;

		fSuspendState.fCounters.fSuspendBySysCall = 0;

		oldState.fSuspendByTimeout |= fSuspendState.fCounters.fSuspendByTimeout;
		fSuspendState.fCounters.fSuspendByTimeout = 0;
	}

	// Preserve any live fSuspendByUIThread before restoring old state
	int liveUIThread = fSuspendState.fCounters.fSuspendByUIThread;
	fSuspendState.fCounters = oldState;
	// Re-add any pending UI thread suspend that arrived during this subroutine
	fSuspendState.fCounters.fSuspendByUIThread += liveUIThread;

	// This could have gone negative..._HostSignalWait will decrement the
	// counter as a courtesy.

	if (fSuspendState.fCounters.fSuspendByExternal < 0)
	{
		fSuspendState.fCounters.fSuspendByExternal = 0;
	}

	EmAssert (fNestLevel >= 0);

#if HAS_OMNI_THREAD
	if (this->InCPUThread ())
		EmAssert (fState == kRunning);
	else
		EmAssert ((fNestLevel == 0 && fState != kRunning) || (fNestLevel > 0 && fState == kRunning));

	fSharedCondition.broadcast ();
#endif

//	LogAppendMsg ("EmSession::ExecuteSubroutine (exit): fState = %ld", (long) fState);
}


// ---------------------------------------------------------------------------
//		� EmSession::ExecuteSpecial
// ---------------------------------------------------------------------------

Bool EmSession::ExecuteSpecial (Bool checkForResetOnly)
{
	if (fReset)
	{
		fReset = false;
		fResetBanks = false;

		this->Reset (fResetType);
	}

	if (fResetBanks)
	{
		fResetBanks = false;

		Memory::ResetBankHandlers ();
	}

	if (!fDeferredErrs.empty ())
	{
#ifndef NDEBUG
		gIterating = true;
#endif

		EmDeferredErrList::iterator	iter = fDeferredErrs.begin ();

		while (iter != fDeferredErrs.end ())
		{
			EmAssert (iter >= fDeferredErrs.begin ());
			EmAssert (iter < fDeferredErrs.end ());

			try
			{
				(*iter)->Do ();
			}
			catch (...)
			{
				// Clicking on Reset or Debug will throw an
				// exception; we need to clean up from that.

#ifndef NDEBUG
				gIterating = false;
#endif

				this->ClearDeferredErrors ();

				throw;
			}

			++iter;
		}

#ifndef NDEBUG
		gIterating = false;
#endif

		this->ClearDeferredErrors ();
	}

	if (checkForResetOnly)
		return false;

	if (fHordeAutoSaveState)
	{
		fHordeAutoSaveState = false;

		Hordes::AutoSaveState ();
	}

	if (fHordeSaveRootState)
	{
		EmAssert (!fHordeSaveSuspendState);

		EmAssert (!fHordeLoadRootState);
		EmAssert (!fHordeNextGremlinFromRootState);
		EmAssert (!fHordeNextGremlinFromSuspendState);

		fHordeSaveRootState = false;

		Hordes::SaveRootState ();
	}

	if (fHordeSaveSuspendState)
	{
		EmAssert (!fHordeSaveRootState);

		EmAssert (!fHordeLoadRootState);
		EmAssert (!fHordeNextGremlinFromRootState);
		EmAssert (!fHordeNextGremlinFromSuspendState);

		fHordeSaveSuspendState = false;

		Hordes::SaveSuspendedState ();
	}

	if (fHordeLoadRootState)
	{
		EmAssert (!fHordeSaveSuspendState);
		EmAssert (!fHordeSaveRootState);

		EmAssert (!fHordeNextGremlinFromRootState);
		EmAssert (!fHordeNextGremlinFromSuspendState);

		fHordeLoadRootState = false;

		Hordes::LoadRootState ();
	}

	if (fHordeNextGremlinFromRootState)
	{
		EmAssert (!fHordeSaveSuspendState);
		EmAssert (!fHordeSaveRootState);

		EmAssert (!fHordeLoadRootState);
		EmAssert (!fHordeNextGremlinFromSuspendState);

		fHordeNextGremlinFromRootState = false;

		if (Hordes::LoadRootState () == errNone)
		{
			Hordes::StartGremlinFromLoadedRootState ();
		}
		else
		{
			Hordes::TurnOn (false);
		}
	}

	if (fHordeNextGremlinFromSuspendState)
	{
		EmAssert (!fHordeSaveSuspendState);
		EmAssert (!fHordeSaveRootState);

		EmAssert (!fHordeLoadRootState);
		EmAssert (!fHordeNextGremlinFromRootState);

		fHordeNextGremlinFromSuspendState = false;

		if (Hordes::LoadSuspendedState () == errNone)
		{
			Hordes::StartGremlinFromLoadedSuspendedState ();
		}
		else
		{
			Hordes::TurnOn (false);
		}
	}

	if (fMinimizeLoadState)
	{
		fMinimizeLoadState = false;
		EmMinimize::RealLoadInitialState ();
	}

	return false;
}


// ---------------------------------------------------------------------------
//		� EmSession::CheckForBreak
// ---------------------------------------------------------------------------
// Check to see if the conditions tell us to break from the CPU Execute loop.

Bool EmSession::CheckForBreak (void)
{
#if HAS_OMNI_THREAD
	omni_mutex_lock	lock (fSharedLock);
#endif

	if (fSuspendState.fAllCounters == 0)
		return false;

	// If nested, ignore fSuspendByExternal.  If we're nested and it's
	// non-zero, then that's because someone just made a HostControl call to
	// change it.  We need to let that call finish.  Because we're in a
	// nested call, fSuspendByExternal starts out at zero.  Therefore, a
	// call to suspend the CPU thread will increment it to 1, and a call to
	// resume the thread will decrement it to -1.  We need to preserve those
	// values so that they can be integrated into the the state that was
	// saved in EmSession::ExecuteSubroutine.

	if (this->IsNested ())
	{
		int	old = fSuspendState.fCounters.fSuspendByExternal;
		fSuspendState.fCounters.fSuspendByExternal = 0;

		Bool result = fSuspendState.fAllCounters != 0;

		fSuspendState.fCounters.fSuspendByExternal = old;

		return result;
	}

	return fSuspendState.fAllCounters != 0;
}


// ---------------------------------------------------------------------------
//		� EmSession::CallCPU
// ---------------------------------------------------------------------------
// Wrapper for EmCPU::Execute.  Called by EmSession::ExecuteIncremental,
// EmSession::ExecuteSubroutine, and EmSession::Run.  This wrapper ensures
// that the state is set to "running", calls EmCPU::Execute, and restores the
// state to what it was before this function was called when it exits.

void EmSession::CallCPU (void)
{
#if HAS_OMNI_THREAD
	omni_mutex_lock					lock (fSharedLock);
#endif

	EmValueChanger<EmSessionState>	oldState (fState, kRunning);

#if HAS_OMNI_THREAD
	omni_mutex_unlock				unlock (fSharedLock);
#endif

	EmAssert (fCPU);
	fCPU->Execute ();
}


#pragma mark -

// ---------------------------------------------------------------------------
//		� EmSession::BlockOnDialog
// ---------------------------------------------------------------------------
// Schedule an error dialog to be displayed.  If running on a multi-threaded
// implementation, save the parameters and signal the UI thread to use them
// to display a dialog box.  Otherwise, if we are not on a multi-threaded
// system, indicate that the session is suspended and safely call back into
// EmDlg::RunDialog.
//
// The result of this function is the button used to dismiss the dialog.  If
// the UI thread is more interested in aborting the CPU thread instead of
// displaying a dialog box, it will cause this function to return -1.

EmDlgItemID EmSession::BlockOnDialog (EmDlgThreadFn fn, const void* parameters)
{
#if HAS_OMNI_THREAD
	EmAssert (this->InCPUThread ());
	EmAssert (gDocument);

	omni_mutex_lock	lock (fSharedLock);

	EmDlgItemID	result = kDlgItemNone;

	gDocument->ScheduleDialog (fn, parameters, result);

//	LogAppendMsg ("EmSession::RunDialog (enter): fState = %ld", (long) fState);

	{
		EmValueChanger<EmSessionState>	oldState (fState, kBlockedOnUI);

		// Broadcast the change in fState.

		fSharedCondition.broadcast ();

		while (result == kDlgItemNone && !fStop)
		{
//			LogAppendMsg ("EmSession::RunDialog (middle): fState = %ld", (long) fState);
			EmAssert (fState == kBlockedOnUI);
			fSharedCondition.wait ();
		}
	}

	// Broadcast the change in fState.

	fSharedCondition.broadcast ();

//	LogAppendMsg ("EmSession::RunDialog (exit): fState = %ld", (long) fState);

	// !!! Throw an exception if fDialogResult == -1.

	return result;

#else

//	LogAppendMsg ("EmSession::RunDialog (enter): fState = %ld", (long) fState);

	// Change the state so that (a) calling EmDlg::RunDialog will call
	// EmDlg::HostRunDialog instead of calling back into here, and (b)
	// so that EmSessionStopper's call to EmSession::SuspendThread
	// doesn't complain when the state is kRunning.

	EmValueChanger<EmSessionState>	oldState (fState, kBlockedOnUI);

	// Don't let the CPU loop run at idle.

	EmSessionStopper	stopper (this, kStopNow);

	// Call back in to RunDialog now that the state has changed to
	// indicate that the session is suspended in some way.

	EmDlgItemID	result = fn (parameters);

//	LogAppendMsg ("EmSession::RunDialog (exit): fState = %ld", (long) fState);

	return result;

#endif
}


// ---------------------------------------------------------------------------
//		� EmSession::UnblockDialog
// ---------------------------------------------------------------------------
// Called by the UI thread after displaying a CPU thread-requested dialog,
// reporting the button the user used to dismiss the dialog.

#if HAS_OMNI_THREAD
void EmSession::UnblockDialog (void)
{
	omni_mutex_lock	lock (fSharedLock);
	fSharedCondition.broadcast ();
}
#endif


#pragma mark -

// ---------------------------------------------------------------------------
//		� EmSession::SetButtonDown
// ---------------------------------------------------------------------------
// Called by the UI thread when a skin button is pressed (mouse down).

void EmSession::SetButtonDown (SkinElementType button)
{
	if (!::PrvCanBotherCPU())
		return;

	uint32	mask = 1U << (int) button;
	fButtonState.fetch_or (mask, std::memory_order_release);
}


// ---------------------------------------------------------------------------
//		� EmSession::SetButtonUp
// ---------------------------------------------------------------------------
// Called by the UI thread when a skin button is released (mouse up).

void EmSession::SetButtonUp (SkinElementType button)
{
	uint32	mask = 1U << (int) button;
	fButtonReleaseRequests.fetch_or (mask, std::memory_order_release);
}


// ---------------------------------------------------------------------------
//		� EmSession::SetButtonTap
// ---------------------------------------------------------------------------
// Called by the UI thread for keyboard shortcuts (F1-F4) and menu actions
// (HotSync).  Sets the button pressed and marks it for auto-release so that
// a quick press/release pair isn't lost between CycleSlowly polls.

void EmSession::SetButtonTap (SkinElementType button)
{
	if (!::PrvCanBotherCPU())
		return;

	uint32	mask = 1U << (int) button;
	fButtonTaps.fetch_or (mask, std::memory_order_release);
	fButtonState.fetch_or (mask, std::memory_order_release);
}


// ---------------------------------------------------------------------------
//		� EmSession::PollButtonChanges
// ---------------------------------------------------------------------------
// Called from CycleSlowly (CPU thread) to detect button state changes.
// Returns bitmasks of pressed/released buttons.  The caller (the hardware
// handler's CycleSlowly) is responsible for dispatching ButtonEvent and
// HotSyncEvent on its own instance, matching the original non-virtual
// dispatch pattern (EmRegsVZ::ButtonEvent, not EmHAL::ButtonEvent).

EmSession::ButtonChanges EmSession::PollButtonChanges (void)
{
	// Cooldown: after dispatching any button change, suppress further
	// changes for a few CycleSlowly intervals.  This replaces the
	// original 100 ms wall-clock throttle (kButtonEventThreshold) and
	// gives PalmOS — especially older ROMs that use level-triggered
	// port-D interrupts — enough emulated time to read and acknowledge
	// the key registers before the state changes again.
	//
	// At 16 MHz (DragonBall 328/EZ) each CycleSlowly interval ≈ 2 ms
	// emulated time, so 5 intervals ≈ 10 ms.  At 33 MHz (VZ) it's
	// about 5 ms.  Both are generous for an ISR + event-post cycle.
	//
	// At fractional speeds the wall-clock delay stretches (e.g. 5
	// intervals at 0.25x = 40 ms wall), making quick clicks feel like
	// holds.  Scale the cooldown down so wall time stays ≈ 10-16 ms.

	uint32	cooldownCycles = 5;
	int		speed = fEmulationSpeed.load (std::memory_order_relaxed);
	if (speed > 0 && speed < 100)
		cooldownCycles = speed * 5 / 100 + 1;	// min 1 at lowest speeds

	if (fButtonCooldown > 0)
	{
		--fButtonCooldown;
		ButtonChanges result = { 0, 0 };
		return result;
	}

	// Step 1: Auto-release any buttons that were tapped in a previous
	// cycle.  Just clear the state bits — edge detection in step 3
	// will pick up the 1→0 transition and include it in the returned
	// mask.

	if (fButtonAutoRelease != 0)
	{
		fButtonState.fetch_and (~fButtonAutoRelease, std::memory_order_release);
		fButtonAutoRelease = 0;
	}

	// Step 2: Process pending release requests from SetButtonUp.
	// Only clear state bits for buttons whose press has already been
	// dispatched (bit is in fButtonPrevState).  This guarantees the
	// press edge is visible for at least one poll before the release.

	uint32	releaseReqs = fButtonReleaseRequests.exchange (0, std::memory_order_acquire);
	if (releaseReqs != 0)
	{
		uint32	canRelease = releaseReqs & fButtonPrevState;
		if (canRelease != 0)
			fButtonState.fetch_and (~canRelease, std::memory_order_release);

		// Defer releases for buttons whose press hasn't been dispatched yet.
		uint32	deferred = releaseReqs & ~fButtonPrevState;
		if (deferred != 0)
			fButtonReleaseRequests.fetch_or (deferred, std::memory_order_release);
	}

	// Step 3: Read current state and detect edges.

	uint32	currentState = fButtonState.load (std::memory_order_acquire);
	uint32	changed = currentState ^ fButtonPrevState;

	ButtonChanges result;
	result.pressed  = changed & currentState;		// 0→1 transitions
	result.released = changed & fButtonPrevState;	// 1→0 transitions

	// If anything changed, start the cooldown so the next change is
	// held off for a few cycles.

	if (result.pressed != 0 || result.released != 0)
		fButtonCooldown = cooldownCycles;

	// Step 4: Move tap bits to auto-release for next cycle.

	uint32	taps = fButtonTaps.exchange (0, std::memory_order_acquire);
	fButtonAutoRelease = taps & currentState;

	// Step 5: Remember current state for next poll.

	fButtonPrevState = currentState;

	return result;
}


// ---------------------------------------------------------------------------
//		� EmSession::HasButtonActivity
// ---------------------------------------------------------------------------
// Returns true if any button is currently pressed or there are pending taps.
// Used by the sleep check in EmCPU68K.

Bool EmSession::HasButtonActivity (void)
{
	return fButtonState.load (std::memory_order_acquire) != 0 ||
		   fButtonTaps.load (std::memory_order_acquire) != 0 ||
		   fButtonReleaseRequests.load (std::memory_order_acquire) != 0 ||
		   fButtonAutoRelease != 0;
}


// ---------------------------------------------------------------------------
//		� EmSession::ClearButtonState
// ---------------------------------------------------------------------------
// Reset all button state.  Called during Reset.

void EmSession::ClearButtonState (void)
{
	fButtonState.store (0, std::memory_order_release);
	fButtonTaps.store (0, std::memory_order_release);
	fButtonReleaseRequests.store (0, std::memory_order_release);
	fButtonPrevState = 0;
	fButtonAutoRelease = 0;
	fButtonCooldown = 0;
}


// ---------------------------------------------------------------------------
//		� EmSession::PostKeyEvent
//		� EmSession::HasKeyEvent
//		� EmSession::PeekKeyEvent
//		� EmSession::GetKeyEvent
// ---------------------------------------------------------------------------

void EmSession::PostKeyEvent (const EmKeyEvent& event)
{
	if (!::PrvCanBotherCPU())
		return;

	fKeyQueue.Put (event);

	// Wake up the CPU in case it's sleeping so that it can
	// quickly handle the event.

	::PrvWakeUpCPU (kStr_EnterPen);
}


Bool EmSession::HasKeyEvent (void)
{
	return fKeyQueue.GetUsed () > 0;
}


EmKeyEvent EmSession::PeekKeyEvent (void)
{
	return fKeyQueue.Peek ();
}


EmKeyEvent EmSession::GetKeyEvent (void)
{
	return fKeyQueue.Get ();
}


// ---------------------------------------------------------------------------
//		� EmSession::PostPenEvent
//		� EmSession::HasPenEvent
//		� EmSession::PeekPenEvent
//		� EmSession::GetPenEvent
// ---------------------------------------------------------------------------

void EmSession::PostPenEvent (const EmPenEvent& event)
{
	if (!::PrvCanBotherCPU())
		return;

	// If this pen-down event is the same as the last pen-down
	// event, do nothing.

	if (event.fPenIsDown && event == fLastPenEvent)
	{
		return;
	}

	// Add the event to our queue.

	fPenQueue.Put (event);

	// Remember this event for the next time.

	fLastPenEvent = event;

	// Wake up the CPU in case it's sleeping so that it can
	// quickly handle the event.

	::PrvWakeUpCPU (kStr_EnterPen);
}


Bool EmSession::HasPenEvent (void)
{
	return fPenQueue.GetUsed () > 0;
}


EmPenEvent EmSession::PeekPenEvent (void)
{
	return fPenQueue.Peek ();
}


EmPenEvent EmSession::GetPenEvent (void)
{
	return fPenQueue.Get ();
}


// ---------------------------------------------------------------------------
//		� EmSession::ReleaseBootKeys
// ---------------------------------------------------------------------------

void EmSession::ReleaseBootKeys (void)
{
	if (fBootKeys & (1L << kElement_PowerButton))
		EmHAL::ButtonEvent (kElement_PowerButton, false);

	if (fBootKeys & (1L << kElement_DownButton))
		EmHAL::ButtonEvent (kElement_DownButton, false);

	if (fBootKeys & (1L << kElement_UpButton))
		EmHAL::ButtonEvent (kElement_UpButton, false);

	fBootKeys = 0;
}


// ---------------------------------------------------------------------------
//		� PrvCanBotherCPU
// ---------------------------------------------------------------------------

Bool PrvCanBotherCPU (void)
{
	if (Hordes::IsOn ())
		return false;

	if (EmEventPlayback::ReplayingEvents ())
		return false;

	if (EmMinimize::IsOn ())
		return false;

//	if (!Patches::UIInitialized ())
//		return false;

	// !!! Need a check for if the device is sleeping.

	return true;
}


// ---------------------------------------------------------------------------
//		� PrvWakeUpCPU
// ---------------------------------------------------------------------------

void PrvWakeUpCPU (long strID)
{
	// Make sure the app's awake.  Normally, we post events on a patch to
	// SysEvGroupWait.	However, if the Palm device is already waiting,
	// then that trap will never get called.  By calling EvtWakeup now,
	// we'll wake up the Palm device from its nap.

	EmSessionStopper	stopper (gSession, kStopOnSysCall);

	if (stopper.Stopped ())
	{
		Errors::ReportIfPalmError (strID, ::EvtWakeup ());
	}
}

#pragma mark -

// ---------------------------------------------------------------------------
//		� EmSession::GetConfiguration
// ---------------------------------------------------------------------------

Configuration EmSession::GetConfiguration (void)
{
	return fConfiguration;
}


// ---------------------------------------------------------------------------
//		� EmSession::GetFile
// ---------------------------------------------------------------------------

EmFileRef EmSession::GetFile (void)
{
	return fFile;
}


// ---------------------------------------------------------------------------
//		� EmSession::GetDevice
// ---------------------------------------------------------------------------

EmDevice EmSession::GetDevice (void)
{
	return fConfiguration.fDevice;
}


// ---------------------------------------------------------------------------
//		� EmSession::GetBreakOnSysCall
// ---------------------------------------------------------------------------

Bool EmSession::GetBreakOnSysCall (void)
{
#if HAS_OMNI_THREAD
	omni_mutex_lock	lock (fSharedLock);
#endif

	return fBreakOnSysCall;
}


// ---------------------------------------------------------------------------
//		� EmSession::IsNested
// ---------------------------------------------------------------------------

#if 0	// Inlined
Bool EmSession::IsNested (void)
{
	return fNestLevel > 0;
}
#endif


// ---------------------------------------------------------------------------
//		� EmSession::GetNeedPostLoad
// ---------------------------------------------------------------------------

Bool EmSession::GetNeedPostLoad (void)
{
	return fNeedPostLoad;
}


// ---------------------------------------------------------------------------
//		� EmSession::SetNeedPostLoad
// ---------------------------------------------------------------------------

void EmSession::SetNeedPostLoad (Bool newValue)
{
	fNeedPostLoad = newValue;
}


#pragma mark -

// ---------------------------------------------------------------------------
//		� EmSession::ScheduleSuspendException
//		� EmSession::ScheduleSuspendError
//		� EmSession::ScheduleSuspendExternal
//		� EmSession::ScheduleSuspendTimeout
//		� EmSession::ScheduleSuspendSysCall
//		� EmSession::ScheduleSuspendSubroutineReturn
// ---------------------------------------------------------------------------

void EmSession::ScheduleSuspendException (void)
{
#if HAS_OMNI_THREAD
	omni_mutex_lock	lock (fSharedLock);
#endif

	fSuspendState.fCounters.fSuspendByDebugger++;

	EmAssert (fCPU);
	fCPU->CheckAfterCycle ();
}


void EmSession::ScheduleSuspendError (void)
{
#if HAS_OMNI_THREAD
	omni_mutex_lock	lock (fSharedLock);
#endif

	fSuspendState.fCounters.fSuspendByDebugger++;

	EmAssert (fCPU);
	fCPU->CheckAfterCycle ();
}


void EmSession::ScheduleSuspendExternal (void)
{
#if HAS_OMNI_THREAD
	omni_mutex_lock	lock (fSharedLock);
#endif

	fSuspendState.fCounters.fSuspendByExternal++;
	fSuspendState.fCounters.fSuspendBySysCall = 1;

	EmAssert (fCPU);
	fCPU->CheckAfterCycle ();
}


void EmSession::ScheduleSuspendTimeout (void)
{
#if HAS_OMNI_THREAD
	omni_mutex_lock	lock (fSharedLock);
#endif

	fSuspendState.fCounters.fSuspendByTimeout = 1;

	EmAssert (fCPU);
	fCPU->CheckAfterCycle ();
}


void EmSession::ScheduleSuspendSysCall (void)
{
#if HAS_OMNI_THREAD
	omni_mutex_lock	lock (fSharedLock);
#endif

	fSuspendState.fCounters.fSuspendBySysCall = 1;

	EmAssert (fCPU);
	fCPU->CheckAfterCycle ();
}


void EmSession::ScheduleSuspendSubroutineReturn (void)
{
#if HAS_OMNI_THREAD
	omni_mutex_lock	lock (fSharedLock);
#endif

	fSuspendState.fCounters.fSuspendBySubroutineReturn = 1;

	EmAssert (fCPU);
	fCPU->CheckAfterCycle ();
}


void EmSession::ScheduleResumeExternal (void)
{
#if HAS_OMNI_THREAD
	omni_mutex_lock	lock (fSharedLock);
#endif

	// Let it go negative.  It needs to do this if we're resuming.
	// See comments in EmCPU68K::CheckForBreak.

//	if (fSuspendState.fCounters.fSuspendByExternal)
	{
		fSuspendState.fCounters.fSuspendByExternal--;
	}
}


#pragma mark -

// ---------------------------------------------------------------------------
//		� EmSession::ScheduleReset
//		� EmSession::ScheduleResetBanks
//		� EmSession::ScheduleAutoSaveState
//		� EmSession::ScheduleSaveRootState
//		� EmSession::ScheduleSaveSuspendedState
//		� EmSession::ScheduleLoadRootState
//		� EmSession::ScheduleNextGremlinFromRootState
//		� EmSession::ScheduleNextGremlinFromSuspendedState
// ---------------------------------------------------------------------------

void EmSession::ScheduleReset (EmResetType resetType)
{
	fReset = 1;
	fResetType = resetType;

	EmAssert (fCPU);
	fCPU->CheckAfterCycle ();
}


void EmSession::ScheduleResetBanks (void)
{
	fResetBanks = 1;

	EmAssert (fCPU);
	fCPU->CheckAfterCycle ();
}


void EmSession::ScheduleAutoSaveState (void)
{
	fHordeAutoSaveState = 1;

	EmAssert (fCPU);
	fCPU->CheckAfterCycle ();
}


void EmSession::ScheduleSaveRootState (void)
{
	fHordeSaveRootState = 1;

	EmAssert (fCPU);
	fCPU->CheckAfterCycle ();
}


void EmSession::ScheduleSaveSuspendedState (void)
{
	fHordeSaveSuspendState = 1;

	EmAssert (fCPU);
	fCPU->CheckAfterCycle ();
}


void EmSession::ScheduleLoadRootState (void)
{
	fHordeLoadRootState = 1;

	EmAssert (fCPU);
	fCPU->CheckAfterCycle ();
}


void EmSession::ScheduleNextGremlinFromRootState (void)
{
	fHordeNextGremlinFromRootState = 1;

	EmAssert (fCPU);
	fCPU->CheckAfterCycle ();
}


void EmSession::ScheduleNextGremlinFromSuspendedState (void)
{
	fHordeNextGremlinFromSuspendState = 1;

	EmAssert (fCPU);
	fCPU->CheckAfterCycle ();
}


void EmSession::ScheduleMinimizeLoadState (void)
{
	fMinimizeLoadState = 1;

	EmAssert (fCPU);
	fCPU->CheckAfterCycle ();
}


void EmSession::ScheduleDeferredError (EmDeferredErr* err)
{
	EmAssert (gIterating == false);

	fDeferredErrs.push_back (err);

	EmAssert (fCPU);
	fCPU->CheckAfterCycle ();
}


void EmSession::ClearDeferredErrors (void)
{
	EmAssert (gIterating == false);

	EmDeferredErrList::iterator	iter = fDeferredErrs.begin ();
	while (iter != fDeferredErrs.end ())
	{
		delete *iter;
		++iter;
	}

	fDeferredErrs.clear ();
}


#pragma mark -

// ---------------------------------------------------------------------------
//		� EmSession::AddInstructionBreakHandlers
// ---------------------------------------------------------------------------

void EmSession::AddInstructionBreakHandlers (	InstructionBreakInstaller f1,
												InstructionBreakRemover f2,
												InstructionBreakReacher f3)
{
	InstructionBreakFuncs	funcs;

	funcs.fInstaller = f1;
	funcs.fRemover = f2;
	funcs.fReacher = f3;

	fInstructionBreakFuncs.push_back (funcs);
}


// ---------------------------------------------------------------------------
//		� EmSession::AddDataBreakHandlers
// ---------------------------------------------------------------------------

void EmSession::AddDataBreakHandlers (	DataBreakInstaller f1,
										DataBreakRemover f2,
										DataBreakReacher f3)
{
	DataBreakFuncs	funcs;

	funcs.fInstaller = f1;
	funcs.fRemover = f2;
	funcs.fReacher = f3;

	fDataBreakFuncs.push_back (funcs);
}


// ---------------------------------------------------------------------------
//		� EmSession::InstallInstructionBreaks
// ---------------------------------------------------------------------------

void EmSession::InstallInstructionBreaks (void)
{
	InstructionBreakFuncList::iterator	iter = fInstructionBreakFuncs.begin ();
	
	while (iter != fInstructionBreakFuncs.end ())
	{
		iter->fInstaller ();

		++iter;
	}
}


// ---------------------------------------------------------------------------
//		� EmSession::RemoveInstructionBreaks
// ---------------------------------------------------------------------------

void EmSession::RemoveInstructionBreaks (void)
{
	InstructionBreakFuncList::iterator	iter = fInstructionBreakFuncs.begin ();
	
	while (iter != fInstructionBreakFuncs.end ())
	{
		iter->fRemover ();

		++iter;
	}
}


// ---------------------------------------------------------------------------
//		� EmSession::HandleInstructionBreak
// ---------------------------------------------------------------------------

void EmSession::HandleInstructionBreak (void)
{
	InstructionBreakFuncList::iterator	iter = fInstructionBreakFuncs.begin ();

	while (iter != fInstructionBreakFuncs.end ())
	{
		iter->fReacher ();

		++iter;
	}
}


// ---------------------------------------------------------------------------
//		� EmSession::InstallDataBreaks
// ---------------------------------------------------------------------------

void EmSession::InstallDataBreaks (void)
{
	DataBreakFuncList::iterator	iter = fDataBreakFuncs.begin ();
	
	while (iter != fDataBreakFuncs.end ())
	{
		iter->fInstaller ();

		++iter;
	}
}


// ---------------------------------------------------------------------------
//		� EmSession::RemoveDataBreaks
// ---------------------------------------------------------------------------

void EmSession::RemoveDataBreaks (void)
{
	DataBreakFuncList::iterator	iter = fDataBreakFuncs.begin ();
	
	while (iter != fDataBreakFuncs.end ())
	{
		iter->fRemover ();

		++iter;
	}
}


// ---------------------------------------------------------------------------
//		� EmSession::HandleDataBreak
// ---------------------------------------------------------------------------

void EmSession::HandleDataBreak (emuptr address, int size, Bool forRead)
{
	DataBreakFuncList::iterator	iter = fDataBreakFuncs.begin ();
	
	while (iter != fDataBreakFuncs.end ())
	{
		iter->fReacher (address, size, forRead);

		++iter;
	}
}


#pragma mark -

// ---------------------------------------------------------------------------
//		� EmSession::RunStatic
// ---------------------------------------------------------------------------

#if HAS_OMNI_THREAD

void EmSession::RunStatic (void* arg)
{
	EmAssert (arg);
	((EmSession*) arg)->Run ();
}

#endif


// ---------------------------------------------------------------------------
//		� EmSession::Run
// ---------------------------------------------------------------------------

#if HAS_OMNI_THREAD

void EmSession::Run ()
{
	EmAssert (fCPU);

	// Acquire the lock to the shared variables so that we can check fState.

	omni_mutex_lock	lock (fSharedLock);

	// Loop until we're asked to stop.

	try
	{
		while (!fStop)
		{
			if (fSuspendState.fAllCounters)
			{
				while (this->IsNested () || (fSuspendState.fAllCounters && !fStop))
				{
					if (!this->IsNested ())
					{
						fState = kSuspended;
					}

					fSharedCondition.broadcast ();

					fSharedCondition.wait ();
				}

				if (fStop)
				{
					continue;
				}
			}

//			LogAppendMsg ("EmSession::Run (after if): fState = %ld", (long) fState);

			EmAssert (fSuspendState.fAllCounters == 0);
			EmAssert (fNestLevel == 0);
			EmAssert (fState == kRunning);

			// We're no longer suspended.  Release our shared globals.

			fSharedLock.unlock ();

			// Execute the "fetch an opcode and emulate it" loop.  This
			// function returns only if requested or an error occurs.

			try
			{
				this->CallCPU ();
			}
			catch (EmExceptionReset& e)
			{
				e.Display ();
				e.DoAction ();
			}
			catch (EmExceptionTopLevelAction& e)
			{
				e.DoAction ();
			}
			catch (...)
			{
				EmAssert (false);
			}

			fSharedLock.lock ();

//			LogAppendMsg ("EmSession::Run (after CallCPU): fState = %ld", (long) fState);

			EmAssert (fState == kRunning);
			EmAssert (fNestLevel == 0);
		}
	}
	catch (...)
	{
		// We don't actually know if fSharedLock is acquired or
		// not on this exception.  Hopefully, its being unlocked
		// if it's already unlocked is OK.
	}

	// fStop is set to true
	// fSharedLock is locked

	fState = kStopped;
	fSharedCondition.broadcast ();
}

#endif


#pragma mark -

// ---------------------------------------------------------------------------
//		� EmSessionStopper::EmSessionStopper
// ---------------------------------------------------------------------------

EmSessionStopper::EmSessionStopper (EmSession* cpu, EmStopMethod how) :
	fSession (cpu),
	fHow (how),
	fStopped (false)
{
	if (fSession)
	{
		fStopped = fSession->SuspendThread (how);
	}
}


// ---------------------------------------------------------------------------
//		� EmSessionStopper::~EmSessionStopper
// ---------------------------------------------------------------------------

EmSessionStopper::~EmSessionStopper (void)
{
	if (fSession && fStopped)
	{
		fSession->ResumeThread ();
	}
}


// ---------------------------------------------------------------------------
//		� EmSessionStopper::Stopped
// ---------------------------------------------------------------------------

Bool EmSessionStopper::Stopped (void)
{
	return fStopped;
}


// ---------------------------------------------------------------------------
//		� EmSessionStopper::CanCall
// ---------------------------------------------------------------------------

Bool EmSessionStopper::CanCall (void)
{
	return	(fSession != NULL) &&
			(fHow == kStopOnSysCall) &&
			(fStopped /*== true*/);
}
