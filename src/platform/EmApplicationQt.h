/* -*- mode: C++; tab-width: 4 -*- */
/* Qt PORT: Qt-based EmApplication subclass.
 *
 * Follows EmApplicationFltk as the reference implementation.
 * Key differences from v1:
 *   - Properly subclasses EmApplication (calls Startup/Shutdown)
 *   - No bridge thread -- QTimer drives HandleIdle on UI thread
 *   - Clipboard support via Qt clipboard API
 */

#ifndef EmApplicationQt_h
#define EmApplicationQt_h

#include "EmApplication.h"		// EmApplication
#include "EmStructs.h"			// ByteList

class EmWindowQt;
class QTimer;

class EmApplicationQt : public EmApplication
{
	public:
								EmApplicationQt		(void);
		virtual					~EmApplicationQt	(void);

	public:
		virtual Bool			Startup				(int argc, char** argv);
		void					Run					(void);
		virtual void			Shutdown			(void);
		void					HandleIdle			(void);

	private:
		void					PrvCreateWindow		(int argc, char** argv);
		Bool					PrvIdleClipboard	(void);

	private:
		EmWindowQt*				fAppWindow;
		ByteList				fClipboardData;
};

extern EmApplicationQt*		gHostApplication;

#endif	// EmApplicationQt_h
