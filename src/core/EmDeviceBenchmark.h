/* -*- mode: C++; tab-width: 4 -*- */
/* ===================================================================== *\
	Per-device benchmark clock calibration.

	Stores real-hardware and emulator-baseline benchmark data per device.
	The correction ratio (emu_mixed / real_mixed) is applied to the
	throttle clock so that "1x Realtime" matches the real device.
	The timer stays on raw MC68000 cycles (unchanged).
\* ===================================================================== */

#ifndef EmDeviceBenchmark_h
#define EmDeviceBenchmark_h

#include <cstring>

struct EmBenchmarkData
{
	int ramRdW, ramRdL, ramWrW, ramWrL;
	int romRdW, romRdL;
	int hwRegRd;
	int nopRom, nopRam;
	int lcdWr;
	int stackOps;
	int mixedCPI;
	int calib1M;
};

struct EmDeviceBenchmarkEntry
{
	const char*      deviceID;       // matches EmDevice::GetIDString()
	EmBenchmarkData  realHardware;   // measured on physical device
	EmBenchmarkData  emulatorBase;   // measured in POSE (accurate timer, no correction)
};

static const EmDeviceBenchmarkEntry kDeviceBenchmarks[] =
{
	{
		"PalmM500",
		{ 735, 888, 838, 831, 535, 684, 504, 252, 42, 210, 2693, 1214, 36 },
		{ 1910, 2590, 2498, 2569, 1910, 2497, 1982, 807, 156, 624, 7872, 3232, 121 },
	},
	// Add more devices as benchmark data arrives.
};

static const int kDeviceBenchmarkCount =
	sizeof(kDeviceBenchmarks) / sizeof(kDeviceBenchmarks[0]);

inline int32 EmDeviceBenchmark_GetEffectiveClockFreq (
	const char* deviceID, int32 systemClockFreq)
{
	for (int i = 0; i < kDeviceBenchmarkCount; ++i)
	{
		if (strcmp (kDeviceBenchmarks[i].deviceID, deviceID) == 0)
		{
			int realMixed = kDeviceBenchmarks[i].realHardware.mixedCPI;
			int emuMixed  = kDeviceBenchmarks[i].emulatorBase.mixedCPI;
			if (realMixed > 0 && emuMixed > 0)
			{
				// int64 to avoid overflow: 33161216 * 3232 > INT32_MAX
				return (int32)((int64_t) systemClockFreq * emuMixed / realMixed);
			}
			break;
		}
	}
	return systemClockFreq;  // uncalibrated: no correction
}

#endif
