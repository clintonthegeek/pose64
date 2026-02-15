/* -*- mode: C++; tab-width: 4 -*- */
/* Qt PORT v2: Window factory for Unix.
 *
 * On Unix, EmApplicationQt creates one window at startup.
 * EmWindow::NewWindow() (called by EmDocument) returns NULL
 * because the window already exists.  This matches the FLTK model.
 */

// Note: NewWindow() is defined in EmWindowQt.cpp, not here.
// This file is kept empty for compatibility with any code that
// references EmWindowUnix.cpp in includes.
//
// See EmWindowQt.cpp for the NewWindow() implementation.
