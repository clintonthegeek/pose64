/* -*- mode: C++; tab-width: 4 -*- */
/* Qt PORT: Dialog support header */

#ifndef EmDlgQt_h
#define EmDlgQt_h

#include "EmDlg.h"

// Called from EmApplicationQt::HandleIdle to process pending dialogs
void HandleDialogs (void);
void CloseAllDialogs (void);

#endif	/* EmDlgQt_h */
