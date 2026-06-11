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

#ifndef EmSession_h
#define EmSession_h

#include "EmDevice.h"			// EmDevice
#include "EmDlg.h"				// EmDlgItemID, EmCommonDialogFlags
#include "EmThreadSafeQueue.h"	// EmThreadSafeQueue
#include "Skins.h"				// SkinElementType

#include <atomic>				// std::atomic

#if HAS_OMNI_THREAD
#include "omnithread.h" 		// omni_thread, omni_mutex, omni_condition
#endif

class EmAction;					// forward-declared; pointer used for identity only

/*
**	EmSession is the class used to manage an emulation session.  Its
**	responsibilities are:
**	
**		* Create new session
**		* Save session to disk
**		* Load old session from disk or resources
**		* Manage the "CPU loop"
**		* Synchronize with running session
**		* Destroy session
**
**	That's the main abstraction.  However, as you can tell from the
**	interface, a number of other duties have crept in, including:
**	
**		* UI interaction
**		* Gremlins coordination
**		* Instruction and data breakpoints
*/


// ---------------------------------------------------------------------------
#pragma mark EmSuspendCounters
// ---------------------------------------------------------------------------

/*
**	The CPU loop can be suspended for any number of reasons by any number of
**	sources.  EmSuspendCounters records which clients have requested that
**	the CPU loop be suspended and for what reason.  To support nesting, it
**	keeps track of those statistics as counters instead of booleans.  If
**	it's OK by any client for the CPU loop to run, its counter is zero. 
**	Otherwise, it gets incremented to non-zero.  When the CPU loop checks to
**	see if it's OK to run, it looks at all of these counters en masse via
**	the union nature of EmSuspendState.
*/

struct EmSuspendCounters
{
	// Incremented by calls to EmSession::SuspendThread.  Decremented on
	// calls to EmSession::ResumeThread.  For executing commands, getting
	// LCD data, etc.

	int	fSuspendByUIThread			: 4;

	// Incremented when an exception or error occurs.  Decremented (or
	// cleared) when the debugger sends a Continue packet.

	int	fSuspendByDebugger			: 4;

	// Incremented on calls to HostSessionSuspend, or an implicit
	// scripting suspend occurred.  Decremented on HostSessionResume.

	int	fSuspendByExternal			: 4;

	// Set when it's time for the CPU thread to exit after running for a
	// little while.  Used on the Mac, and on other systems when calling
	// a Palm OS function that needs to occasionally return control (as
	// when installing a file with the Exchange Manager).

	int	fSuspendByTimeout			: 1;

	// Set when we're leaving because we called Execute in order to spin
	// the state up to a system call, and that state is reached.

	int	fSuspendBySysCall			: 1;

	// Set when we're leaving because we called Execute in order to call
	// a subroutine, and that subroutine returned.

	int	fSuspendBySubroutineReturn	: 1;
};


// ---------------------------------------------------------------------------
#pragma mark EmSuspendState
// ---------------------------------------------------------------------------

/*
**	EmSuspendState is a union of EmSuspendCounters and a 32-bit integer.  Its
**	purpose is to allow the testing of *all* the suspend flags at once.  If
**	any flag is set, then emulation is suspended.
*/

union EmSuspendState
{
	EmSuspendCounters	fCounters;
	uint32				fAllCounters;
};


// ---------------------------------------------------------------------------
#pragma mark EmSessionState
// ---------------------------------------------------------------------------

/*
**	The current state of the "CPU loop".  If the stateis kSuspended, the
**	reason for that suspension can usually be found in EmSuspendState.
*/

enum EmSessionState
{
	kRunning,		// The CPU thread is running.
	kStopped,		// The CPU thread is not started or has finished.
	kSuspended,		// The CPU thread is suspended at the end of a cycle.
	kBlockedOnUI	// The CPU thread is waiting for a dialog to be handled.
};


// ---------------------------------------------------------------------------
#pragma mark EmStopMethod
// ---------------------------------------------------------------------------

/*
**	An enumerated value passed to EmSessionStopper (or SuspendThread if it's
**	called directly) describing the way in which the thread should be
**	suspended.
*/

enum EmStopMethod
{
	kStopNone,		// Don't stop the CPU.

	kStopNow,		// Suspend now, no matter what.

	kStopOnCycle,	// Suspend at the end of a cycle.  This request may
					// fail if the CPU thread is blocked on the UI.

	kStopOnSysCall	// Suspend on the next call to a system function.  This
					// request may fail if the CPU thread is block on the
					// UI.  The request may *hang* if the CPU thread is in
					// a state such that it never reaches a system call.
};


// ---------------------------------------------------------------------------
#pragma mark EmKeyEvent
// ---------------------------------------------------------------------------

/*
**	Struct containing all data for a key event.
*/

struct EmKeyEvent
{
	EmKeyEvent (uint16 ch) :
		fKey (ch),
		fShiftDown (0),
		fCapsLockDown (0),
		fNumLockDown (0),
		fCommandDown (0),
		fOptionDown (0),
		fControlDown (0),
		fAltDown (0),
		fWindowsDown (0)
		{}

	uint16	fKey;
	Bool	fShiftDown;
	Bool	fCapsLockDown;
	Bool	fNumLockDown;
	Bool	fCommandDown;
	Bool	fOptionDown;
	Bool	fControlDown;
	Bool	fAltDown;
	Bool	fWindowsDown;
};

typedef EmThreadSafeQueue<EmKeyEvent>	EmKeyQueue;


// ---------------------------------------------------------------------------
#pragma mark EmPenEvent
// ---------------------------------------------------------------------------

/*
**	Struct containing all data for a pen event (that is, when the user clicks
**	the mouse in the touchscreen area).
*/

struct EmPenEvent
{
	EmPenEvent (const EmPoint& point, Bool isDown) :
		fPenPoint (point),
		fPenIsDown (isDown)
	{
	}

	EmPoint	fPenPoint;
	Bool	fPenIsDown;

	bool operator==(const EmPenEvent& other) const
	{
		return fPenPoint == other.fPenPoint &&
			fPenIsDown == other.fPenIsDown;
	}
};

typedef EmThreadSafeQueue<EmPenEvent>	EmPenQueue;


// ---------------------------------------------------------------------------
#pragma mark Instruction/DataBreak
// ---------------------------------------------------------------------------

/*
**	Data types and collections for managing functions that install, remove,
**	and respond to instruction and data breaks.
**
**	Instruction breaks occur when the PC reaches an indicated memory location.
**	Data breaks occur when an indicated memory location is accessed (either
**	read or write).  An instruction or instruction data fetch does not trigger
**	a data break.  (!!! Actually, I think the latter does...should it?)
*/

typedef void	(*InstructionBreakInstaller)	(void);
typedef void	(*InstructionBreakRemover)		(void);
typedef void	(*InstructionBreakReacher)		(void);

struct InstructionBreakFuncs
{
	InstructionBreakInstaller	fInstaller;
	InstructionBreakRemover		fRemover;
	InstructionBreakReacher		fReacher;
};

typedef std::vector<InstructionBreakFuncs>	InstructionBreakFuncList;


typedef void	(*DataBreakInstaller)	(void);
typedef void	(*DataBreakRemover)		(void);
typedef void	(*DataBreakReacher)		(emuptr, int size, Bool forRead);

struct DataBreakFuncs
{
	DataBreakInstaller			fInstaller;
	DataBreakRemover			fRemover;
	DataBreakReacher			fReacher;
};

typedef std::vector<DataBreakFuncs>	DataBreakFuncList;


// ---------------------------------------------------------------------------
#pragma mark EmSession
// ---------------------------------------------------------------------------

class EmCPU;
class EmDeferredErr;
class SessionFile;
struct Configuration;

typedef std::vector<EmDeferredErr*>	EmDeferredErrList;

class EmSession;
extern EmSession*	gSession;

// Result of posting host input toward the guest.  Anything but kInputPosted
// means the event was NOT queued — callers that promised honesty (ReControl)
// must surface it (recovery-plan task 2.4).
enum EmPostInputResult
{
	kInputPosted,
	kInputDroppedGremlins,
	kInputDroppedReplay,
	kInputDroppedMinimize,
	kInputDroppedDuplicate		// pen-down identical to the previous one
};

class EmSession
{
	public:
		// -----------------------------------------------------------------------------
		// constructor / destructor
		// -----------------------------------------------------------------------------

								EmSession 			(void);
				 				~EmSession			(void);

		// -----------------------------------------------------------------------------
		// public methods
		// -----------------------------------------------------------------------------

		// Methods to create a new session.  One of these methods must be called after
		// creation the EmSession object.  CreateNew creates a new session based on
		// the data in Configuration.  CreateOld reads a previously saved session from
		// the given file.  CreateBound reads a previously saved session from resources
		// bound to the file.
		//
		// After a session has been created, the client needs to call CreateThread.
		// If "true" is passed to CreateThread, the client also needs to call
		// ResumeThread.  (On the Mac, it only makes sense to pass "false".)

		void					CreateNew			(const Configuration&);
		void					CreateOld			(const EmFileRef&);
		void					CreateBound			(void);

		// Standard sub-system methods:
		//		Reset:	Resets the state.  Called on hardware resets or on
		//				calls to SysReset.  Also called from constructor.
		//		Save:	Saves the state to the given file.
		//		Load:	Loads the state from the given file.  Can assume that
		//				Reset has been called first.

		void 					Reset				(EmResetType);
		void 					Save				(SessionFile&);
		void 					Load				(SessionFile&);

		// Utility methods that create a SessionFile for the given file, and then
		// call the above Save and Load methods.  If updateFileRef is true, then
		// the EmFileRef is remembered as part of the session's "identity".

		void 					Save				(const EmFileRef&,
													 Bool updateFileRef);
		void 					Load				(const EmFileRef&);

		// Called by external thread to create and destroy the thread.  CreateThread
		// is called after the EmSession is created.  If "suspended" is true, the
		// client should also call ResumeThread.  If "suspended" is false, the
		// thread will start running immediately.  DestroyThread can be called
		// manually, but will also be called in the destructor (getting called
		// more than once is OK).

		void					CreateThread		(Bool suspended);
		void					DestroyThread		(void);

		// Called by external thread to suspend and resume the CPU thread.  Not
		// normally called directly by clients.  Instead, clients should use
		// a stack-based EmSessionStopper object.

		Bool					SuspendThread		(EmStopMethod how, int timeoutMs = 0);
		void					ResumeThread		(void);

#if HAS_OMNI_THREAD
		// Pause the thread by the given number of milliseconds.

		void					Sleep				(unsigned long msecs);

		// Sleep for up to the given number of microseconds, but wake
		// immediately if SuspendThread broadcasts fSleepCondition.
		// Used by ExecuteStoppedLoop so that kStopNow can interrupt
		// the wait without blocking the caller for the full duration.

		void					SleepInterruptible	(unsigned long usecs);

		// Return whether or not the calling function is executing in the context of
		// the CPU thread or not.  If not, it's most likely executing in the UI
		// thread -- much of Poser assumes this is the case.
		//
		// It's not clear if this function should return true or false on single-
		// threaded systems, so it's not available to those.

		Bool					InCPUThread			(void) const;
#endif

		// GetSessionState returns the state of the CPU thread, as described in the
		// definition of the EmSessionState type.  If the thread is suspended, you
		// can call GetSuspendState to find out why.  After a thread has been
		// suspended, you might need to clear the reason for it being suspended
		// by calling SetSuspendState before calling ResumeThread.
		//
		// These methods are protected by their own mutex, and can be called
		// by any thread.

		EmSessionState			GetSessionState		(void) const;
		EmSuspendState			GetSuspendState		(void) const;
		void					SetSuspendState		(const EmSuspendState&);

		// Called on the Mac at idle time to execute a little bit then leave.
		// Also called by SuspendThread when the CPU state needs to be halted
		// on a system call or when the Palm OS is idle.
		//
		// Clients should check the suspend state in order to make sure that
		// ExecuteIncremental exitted for the right reason.  If the reason was
		// fSuspendByTimeout, this flag will automatically be cleared before
		// the function returns.  If the reason was fSuspendByDebugger, then
		// the debugger will have already been contacted.
		//
		// If the suspend state is non-zero when this function is called, the
		// function merely exits immediately.

		void					ExecuteIncremental	(void);

		// Called by mechanisms that allow the calling of Palm OS system functions
		// as if they were Poser subroutines.
		//
		// Clients should check the suspend state in order to make sure that
		// ExecuteSubroutine exitted for the right reason.
		//
		// If the suspend state is non-zero when this function is called, the
		// function saves that state, clears the suspend flags, and then enters
		// the CPU loop.  The previous suspend state flags are restored on
		// exit.  If any benign suspend flags were set while executing the
		// Palm OS subroutine (for instance, fSuspendByTimeout), those flags
		// are applied to the previously-saved flags before they are restored.

		void					ExecuteSubroutine	(void);

		// Called by the CPU thread if any processor-independent "end of cycle"
		// processing needs to be carried out (for instance, Gremlins needs to
		// save a snapshot or needs to switch to a previously saved state).
		//
		// This method is also responsible for resetting the emulation state
		// (either in response to a hardware reset or a call to SysReset).
		// "checkForResetOnly" is true if *only* this responsibility should
		// be carried out and any other duties should be skipped.  If it's
		// false, then all end-of-cycle duties should be carried out.

		Bool					ExecuteSpecial		(Bool checkForResetOnly);

		Bool					CheckForBreak		(void);

		// Called by the CPU thread when it needs to display an error message.
		// Do NOT call this from any other thread.

		EmDlgItemID				BlockOnDialog		(EmDlgThreadFn fn,
													 const void* parameters);

#if HAS_OMNI_THREAD
		// Dialog-action lifetime handshake (see BlockOnDialog).  Called by
		// EmActionDialog::Do on the UI thread.  BeginDialogAction returns
		// false if the request was cancelled (the CPU stack the action's
		// parameters point into is gone) -- the action must then do nothing.
		Bool					BeginDialogAction	(EmAction* self);
		void					EndDialogAction		(void);
#endif

		// Methods for interacting with the UI:
		//
		//	PostFooEvent:
		//		Called by the UI thread to post UI events.  Clients need not
		//		synchronize with the CPU thread first.  These methods will do
		//		whatever is needed.
		//
		//	HasFooEvent:
		//		Returns true if there's an event posted to the queue.
		//
		//	PeekFooEvent:
		//		Returns the top event on the queue, but doesn't remove it
		//		from the queue.
		//
		//	GetFooEvent:
		//		Returns the top event from the queue and removes it from
		//		the queue.
		//
		// These methods are protected by their own mutex, and can be called
		// by any thread.

		// State-based button input (replaces event queue).
		// UI thread writes, CPU thread reads in CycleSlowly.
		struct ButtonChanges {
			uint32 pressed;		// bits that transitioned 0 → 1
			uint32 released;	// bits that transitioned 1 → 0
		};

		Bool					SetButtonDown		(SkinElementType);
		void					SetButtonUp			(SkinElementType);
		Bool					SetButtonTap		(SkinElementType);
		ButtonChanges			PollButtonChanges	(void);
		Bool					HasButtonActivity	(void);
		void					ClearButtonState	(void);

		EmPostInputResult		PostKeyEvent		(const EmKeyEvent&);
		Bool					HasKeyEvent			(void);
		EmKeyEvent				PeekKeyEvent		(void);
		EmKeyEvent				GetKeyEvent			(void);

		EmPostInputResult		PostPenEvent		(const EmPenEvent&);
		Bool					HasPenEvent			(void);
		EmPenEvent				PeekPenEvent		(void);
		EmPenEvent				GetPenEvent			(void);

		// Phase 2 delivery accounting (handoff §10 Q-SYNC).  "Delivered" =
		// PuppetString handed the event to the Palm OS event queue
		// (StubAppEnqueueKey/Pt returned) — the same guarantee real hardware
		// gives.  Per-queue counters: keys and pens are separate FIFOs in
		// PuppetString, and a shared counter could be satisfied by the other
		// queue's traffic.
		void					NotifyKeyEventDelivered	(void);	// CPU thread
		void					NotifyPenEventDelivered	(void);	// CPU thread
		uint64					KeyEventsPosted		(void);
		uint64					PenEventsPosted		(void);
		Bool					WaitForKeyDelivery	(uint64 targetSeq, int timeoutMs);
		Bool					WaitForPenDelivery	(uint64 targetSeq, int timeoutMs);

		void					ReleaseBootKeys		(void);

		// Method for getting information on the device we're emulating.
		// Callable at any time from any thread.

		Configuration			GetConfiguration	(void);
		EmFileRef				GetFile				(void);
		EmDevice				GetDevice			(void);

		// Methods for determining if emulation should halt at certain
		// points.  Called inside the CPU thread.

		Bool					GetBreakOnSysCall	(void);

		// Called by the CPU thread to determine if emulation code is being
		// executed as part of normal emulation, or as part of a subroutine
		// call into the Palm OS.  Certain features may be enable or disabled
		// as a result of this check (for instance, breakpoints are generally
		// disabled when Poser is making a Palm OS subroutine call).

		Bool					IsNested			(void);

		// Methods for determining if post-load operations need to take
		// place.  Called inside the CPU thread.

		Bool					GetNeedPostLoad		(void);
		void					SetNeedPostLoad		(Bool);

		// Schedule for the CPU thread to suspend.  Called inside the CPU thread.

		void					ScheduleSuspendException			(void);
		void					ScheduleSuspendError				(void);
		void					ScheduleSuspendExternal				(void);
		void					ScheduleSuspendTimeout				(void);
		void					ScheduleSuspendSysCall				(void);
		void					ScheduleSuspendSubroutineReturn		(void);

		void					ScheduleResumeExternal				(void);

		// Schedule for stuff to happen at the end of the current instruction
		// cycle.  Called inside the CPU thread.

		void					ScheduleReset							(EmResetType);
		void					ForceReset								(EmResetType);
		void					ScheduleResetBanks						(void);
		void					ScheduleAutoSaveState					(void);
		void					ScheduleSaveRootState					(void);
		void					ScheduleSaveSuspendedState				(void);
		void					ScheduleLoadRootState					(void);
		void					ScheduleNextGremlinFromRootState		(void);
		void					ScheduleNextGremlinFromSuspendedState	(void);
		void					ScheduleMinimizeLoadState			(void);
		void					ScheduleDeferredError					(EmDeferredErr*);

		void					ClearDeferredErrors						(void);

		// True while the deferred-error queue is being iterated (a deferred-
		// error dialog is up and the CPU thread is parked in it).  Used to
		// avoid re-entrant ScheduleDeferredError, which would push onto the
		// list mid-walk.

		static Bool				AreDeferredErrorsBeingHandled			(void);

		// Provide routines that get called when it's appropriate to install an
		// instruction break, when it's appropriate to remove an instruction break,
		// or when an instruction break has been reached.  Called by various
		// sub-systems in their Initialize methods.

		void					AddInstructionBreakHandlers	(InstructionBreakInstaller,
															 InstructionBreakRemover,
															 InstructionBreakReacher);

		// Provide routines that get called when it's appropriate to install a
		// data break, when it's appropriate to remove a data break, or when a
		// data break has been reached.  Called by various sub-systems in their
		// Initialize methods.

		void					AddDataBreakHandlers		(DataBreakInstaller,
															 DataBreakRemover,
															 DataBreakReacher);

		// Call the installed routines to install/remove the breakpoints.

		void					InstallInstructionBreaks	(void);
		void					RemoveInstructionBreaks		(void);
		void					HandleInstructionBreak		(void);

		void					InstallDataBreaks			(void);
		void					RemoveDataBreaks			(void);
		void					HandleDataBreak				(emuptr, int size, Bool forRead);

	private:
		// Initialize the state of the session and all of it's sub-systems based
		// on the given configuration.  The configuration is also saved and can
		// later be queried if needed.

		void					Initialize					(const Configuration&);

		// Dispose of all of our resources, as well as all of our sub-systems'.
		// Called from the destructor (after first stopping the CPU thread).

		void					Dispose						(void);

#if HAS_OMNI_THREAD
		// -----------------------------------------------------------------------------
		// omni_thread overrides
		// -----------------------------------------------------------------------------

		static void				RunStatic			(void* arg);
		void					Run					(void);
#endif

		void					CallCPU				(void);

	private:
		friend class EmCPU;
		friend class EmCPU68K;	// Accesses fSuspendState and fStop directly.

	private:
		Configuration			fConfiguration;
		EmFileRef				fFile;

		EmCPU*					fCPU;

#if HAS_OMNI_THREAD
		// Accessed from external thread only.

		omni_thread*			fThread;

		// Accessed from any thread.

		mutable omni_mutex		fSharedLock;
		mutable omni_condition	fSharedCondition;

		omni_mutex				fSleepLock;
		omni_condition			fSleepCondition;

		// Delivery accounting (Phase 2 honest ACK).  Dedicated lock — do NOT
		// fold into fSharedLock (lock-ordering risk vs. the suspend machinery).
		// "Delivered" = PuppetString handed the event to the Palm OS event
		// queue.  Keys and pens are separate FIFOs → separate counters.
		omni_mutex				fDeliveryLock;
		omni_condition			fDeliveryCondition;
		uint64					fKeyPostedSeq{0};
		uint64					fKeyDeliveredSeq{0};
		uint64					fPenPostedSeq{0};
		uint64					fPenDeliveredSeq{0};
#endif

		// ----------------------------------------------------------------------
		// The variables in this section are protected by the mutex, and should
		// not be accessed without first acquiring it.
		// ----------------------------------------------------------------------

#if HAS_OMNI_THREAD
		// Set to true if the thread should quit.  If set by an external
		// thread, the CPU thread will notice the request, but not immediately.
		// If set internally, the CPU thread should also set SPCFLAG_EXIT of
		// regs.spcflags in order for it to get noticed.

		Bool					fStop;

		enum EmDialogActionState
		{
			kDlgActionNone,			// no dialog action outstanding
			kDlgActionQueued,		// posted, not yet picked up by the UI thread
			kDlgActionRunning,		// UI thread is inside RunDialog
			kDlgActionDone,			// UI thread finished; result written
			kDlgActionCancelled		// CPU thread gave up while still queued
		};

		EmDialogActionState		fDialogActionState;	// guarded by fSharedLock
		EmAction*				fDialogAction;		// identity only -- never dereferenced
#endif

		// Set to non-zero if the thread should pause.  If set by an external
		// thread, the CPU thread will notice the request, but not immediately.
		// If set internally, the CPU thread should also set SPCFLAG_SUSPEND of
		// regs.spcflags in order for it to get noticed.

		EmSuspendState			fSuspendState;

		// Set to kStopped when the thread is about to quit.
		//
		// Set to kSuspended when the thread is about to suspend.
		//
		// Set to kBlockedOnUI when thread is waiting for UI thread
		// to display a dialog.  UI thread should set this
		// to false after the dialog is displayed and dismissed.
		//
		// Otherwise, set to kRunning.

		EmSessionState			fState;

		// These values are read in any thread at any time, but should only
		// be changed after stopping the CPU thread.  They're set up by the
		// ExecuteUntilFoo methods.

		Bool					fBreakOnSysCall;
		int						fNestLevel;

		Bool					fNeedPostLoad;

		// These values indicate that certain end-of-cycle actions
		// should take place.

		Bool					fReset;
		Bool					fResetBanks;
		Bool					fHordeAutoSaveState;
		Bool					fHordeSaveRootState;
		Bool					fHordeSaveSuspendState;
		Bool					fHordeLoadRootState;
		Bool					fHordeNextGremlinFromRootState;
		Bool					fHordeNextGremlinFromSuspendState;
		Bool					fMinimizeLoadState;

		EmDeferredErrList		fDeferredErrs;

		EmResetType				fResetType;

	private:
		std::atomic<uint32>		fButtonState{0};		// pressed buttons (UI writes)
		std::atomic<uint32>		fButtonTaps{0};			// tap requests (auto-release)
		std::atomic<uint32>		fButtonReleaseRequests{0}; // pending releases (UI writes)
		uint32					fButtonPrevState = 0;	// CPU-thread only
		uint32					fButtonAutoRelease = 0;	// CPU-thread only
		uint32					fButtonCooldown = 0;	// CPU-thread only

		EmKeyQueue				fKeyQueue;
		EmPenQueue				fPenQueue;
		omni_mutex				fPenEventLock;	// guards fLastPenEvent across writer threads

		EmPenEvent				fLastPenEvent;
		uint32					fBootKeys;

	public:
		std::atomic<int>			fEmulationSpeed{1};		// percent: 100=1x, 200=2x, 400=4x, 0=max
																	// (ctor loads kPrefKeyEmulationSpeed, migrating the
																	//  legacy 1/2/4/8 multiplier encoding to percent)
		std::atomic<int32>			fEffectiveClockFreq{0};	// 0 = lazy-init on first throttle call

	private:
		InstructionBreakFuncList	fInstructionBreakFuncs;
		DataBreakFuncList			fDataBreakFuncs;
};


// ---------------------------------------------------------------------------
#pragma mark EmSessionStopper
// ---------------------------------------------------------------------------

/*
**	Stack object used to briefly stop the emulator.
*/

class EmSessionStopper
{
	public:
								EmSessionStopper	(EmSession*, EmStopMethod how, int timeoutMs = 0);
								~EmSessionStopper 	(void);

		Bool					Stopped				(void);
		Bool					CanCall				(void);

	private:
		EmSession*				fSession;
		int						fHow;
		int						fTimeoutMs;
		Bool					fStopped;
};


// Inlined because it's called from EmCPU68K::Execute on every opcode.

inline Bool EmSession::IsNested (void)
{
	return fNestLevel > 0;
}

#endif /* EmSession_h */
