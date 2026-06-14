/* -*- mode: C++; tab-width: 4 -*- */
/* Phase 5: single home for the Palm-OS-vs-Qt macro clash defusal.  Palm's
 * DateTime.h does `#define daysInYear 365` (and monthsInYear), which collides
 * with Qt usage.  Include this AFTER the Palm headers (i.e. after EmCommon.h)
 * and BEFORE the Qt headers, exactly where the old per-file preamble sat.
 * Intentionally has no include guard. */

#undef daysInYear
#undef monthsInYear
