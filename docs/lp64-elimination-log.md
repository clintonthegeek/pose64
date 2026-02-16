# LP64 False Violation Investigation: Process of Elimination

Everything below has been code-reviewed and confirmed **NOT the cause** of the
false META_CHECK violations on the LP64 (64-bit) build. These violations appear
on LP64 but do NOT appear on the original 32-bit POSE with the same ROM.

## Symptom Summary

Three categories of false violations:

1. **Low memory reads** (addr 0x78 = autovector 6) — 158K+ META_VIOLATIONs per
   boot, all from ROM code (PC in 0x10xxxxxx range). Most suppressed by
   AccessOK(), but some become kLowMemAccess errors shown in dialog.

2. **Screen memory reads** (addr 0x3B00) — from ROM code or RAM-based OS code.

3. **Storage heap writes** (addrs 0x40746, 0x413C2, 0x400FC) — ROM code writing
   bytes to addresses in the SRAM-protected storage heap. Multiple different
   apps write to the SAME address — hallmark of systemic issue.

---

## Verified LP64-Safe (NOT the bug)

### 1. UAE 68K CPU register storage
- `regstruct.regs[16]` = `uae_u32[16]` = uint32 array. LP64-safe.
- `regstruct.pc` = `uae_u32` = uint32. LP64-safe.
- `regstruct.usp, isp, msp` = `uaecptr` = `emuptr` = uint32. LP64-safe.
- `m68k_dreg(r,num)` and `m68k_areg(r,num)` = macros into regs[] array. Safe.
- File: `src/core/UAE/newcpu.h:69-102`

### 2. UAE type definitions
- `uae_u32` = `uint32`, `uae_s32` = `int32`, `uaecptr` = `emuptr` = uint32.
- All emulated addresses are 32-bit regardless of host platform.
- File: `src/core/UAE/sysdeps.h`

### 3. m68k_getpc() pointer arithmetic
- `regs.pc + ((char*)regs.pc_p - (char*)regs.pc_oldp)`
- Pointer subtraction yields `ptrdiff_t` (64-bit on LP64).
- Added to `regs.pc` (uint32), result truncated to `uaecptr` (uint32).
- The ptrdiff_t is small (within one memory bank), so truncation is safe.
- File: `src/core/UAE/newcpu.h:175-178`

### 4. Memory bank dispatch system
- `EmMemBankIndex(addr)` = `(emuptr)(addr) >> 16` — uint32 operation. Safe.
- `EmMemGetBank(addr)` = array lookup by bank index. Safe.
- `EmMemCallGetFunc(func, addr)` = calls bank handler with emuptr. Safe.
- File: `src/core/Hardware/EmMemory.h:80-98`

### 5. META_CHECK macro
- Identical on 32-bit and LP64 (no conditional compilation).
- Three branches: IsPCInRAM, IsPCInMemMgr, else (ROM code).
- All branches check meta-memory bits via CanXxxOp(metaAddress).
- File: `src/core/MetaMemory.h:431-461`

### 6. CanSystemRead / CanAppRead / CanMemMgrRead
- All are inlined uint8 bit-test operations.
- `META_VALUE_8(metaAddress)` = `*(uint8*)p`. Always 1 byte. LP64-safe.
- `META_BITS_8(bits)` = compile-time constant. LP64-safe.
- File: `src/core/MetaMemory.h:289-331`

### 7. Meta-memory access bits
- `kNoAppAccess = 0x01`, `kNoSystemAccess = 0x02`, `kNoMemMgrAccess = 0x04`
- `kLowMemoryBits = 0x07`, `kGlobalsBits = 0x01`, `kScreenBits = 0x21`
- All are uint8 enum values. LP64-safe.
- File: `src/core/MetaMemory.h:224-244`

### 8. InlineGetMetaAddress
- `return (uint8*)&(gRAM_MetaMemory[address])` — indexes byte array by
  emuptr (uint32). LP64-safe (array index is just an integer).
- File: `src/core/Hardware/EmBankDRAM.cpp:97-100`

### 9. CEnableFullAccess RAII guard
- Constructor zeros `gMemAccessFlags`, increments `fgAccessCount`.
- Destructor restores `gMemAccessFlags`, decrements `fgAccessCount`.
- `AccessOK()` returns `fgAccessCount > 0`.
- `fgAccessCount` is `static long` (8 bytes on LP64 vs 4 on ILP32).
- Increment/decrement/comparison all work identically. LP64-safe.
- File: `src/core/Hardware/EmMemory.h`, `src/core/Hardware/EmMemory.cpp`

### 10. MemAccessFlags struct
- All fields are `Bool` (= `int`, 4 bytes on both platforms).
- Same struct layout on both platforms.
- `fProtect_SRAMSet` driven by DragonBall csDSelect bit 0x2000.
- File: `src/core/Hardware/EmMemory.h`

### 11. DragonBall VZ csDSelect register handling
- `csDSelectWrite`: `StdWrite` + read-back of uint16 register.
- `fProtect_SRAMSet = (READ_REGISTER(csDSelect) & 0x2000) != 0`
- `EmMemDoGet16` / `EmMemDoPut16` for register access. LP64-safe.
- File: `src/core/Hardware/EmRegsVZ.cpp`

### 12. Register read/write functions
- `_get_reg` / `_put_reg` have overloads for int8/16/32, uint8/16/32.
- All operate on fixed-width types. LP64-safe.
- File: `src/core/Hardware/EmRegsPrv.h`

### 13. M68KExcTableType struct
- All fields are `UInt32` (NOT pointers). `sizeof` = 256 = 0x100.
- `offsetof(M68KExcTableType, autoVec1)` = 0x64 on both platforms.
- `offsetof(M68KExcTableType, autoVec6)` = 0x78 on both platforms.
- `offsetof(M68KExcTableType, trapN[0])` = 0x80 on both platforms.
- File: `src/core/Palm/Platform/Incs/Core/Hardware/M68KHwr.h:32-65`

### 14. LowMemHdrType struct
- `M68KExcTableType vectors` + `FixedGlobalsType globals`.
- `offsetof(LowMemHdrType, globals)` = 0x100 (sizeof M68KExcTableType).
- Used as boundary between "low memory" and "system globals".
- LP64-safe because M68KExcTableType is all UInt32.
- File: `src/core/Palm/Platform/Core/System/IncsPrv/Globals.h:391-403`

### 15. Low memory / system globals boundary
- `GetLowMemoryBegin()` = 0.
- `GetLowMemoryEnd()` = 0x100 = `offsetof(LowMemHdrType, globals)`.
- `GetSysGlobalsBegin()` = 0x100.
- `GetSysGlobalsEnd()` = dynamic (sysDispatchTableP + size * sizeof(emuptr)).
- All return `emuptr` (uint32). LP64-safe.
- File: `src/core/MetaMemory.cpp:583-619`

### 16. HwrSleep headpatch / tailpatch
- Headpatch: `MarkTotalAccess` on [0x64, 0x80) — clears restriction bits.
- Tailpatch: `MarkLowMemory` on [0x64, 0x80) — restores restriction bits.
- Uses `offsetof(M68KExcTableType, ...)` — correct values (see #13).
- Uses `sizeof(emuptr)` = 4 on both platforms. LP64-safe.
- File: `src/core/Patches/EmPatchModuleSys.cpp:1148-1179` (head),
  `2665-2690` (tail)

### 17. MemKernelInit tailpatch meta-memory marking
- `MarkLowMemory(0, 0x100)` — correct range.
- `MarkSystemGlobals(0x100, ...)` — correct start.
- `MarkHeapHeader(heap0start, heap0end)` — uses emuptr. Safe.
- MarkSystemGlobals is called AFTER MarkLowMemory. They don't overlap
  (low memory = [0, 0x100), globals = [0x100, ...)).
- Address 0x78 is in [0, 0x100) → gets kLowMemoryBits = 0x07.
- File: `src/core/Patches/EmPatchModuleMemMgr.cpp:947-960`

### 18. Memory bank sizes
- `gDynamicHeapSize` = 0x40000 (256KB). Correct.
- `gRAMBank_Size` = 0x800000 (8MB). Correct.
- Both are emuptr/uint32. LP64-safe.
- Verified at runtime via debug fprintf in SetBankHandlers.
- File: `src/core/Hardware/EmBankDRAM.cpp`

### 19. CheckNewPC / IsPCInRAM
- `gPCInRAM = newPC < EmBankROM::GetMemoryStart()`
- Both are `emuptr` (uint32). Comparison is LP64-safe.
- `EmBankROM::GetMemoryStart()` returns `gROMMemoryStart` (emuptr).
- File: `src/core/Hardware/EmMemory.cpp`

### 20. get_disp_ea_000 (68K effective address computation)
- Computes displacement-based effective addresses.
- Uses 32-bit registers and 16-bit displacements.
- Sign extension from int16 to int32 — correct on LP64.
- File: `src/core/UAE/newcpu.h`

### 21. Deterministic canary test results
- Custom PRC with 5 controlled tests (T1-T5).
- T2 (read 0x78) and T3 (read 0x3B00) fire violations on BOTH 32-bit and
  LP64 — proving META_CHECK works correctly for USER app code.
- All 5 tests report OK.
- This proves the address is NOT being miscomputed for user app code.
- File: `docs/deterministic-canary.md`, `/home/clinton/dev/palmtest/canary/`

### 22. GetMacsbugInfo function
- Reads Macsbug function names from ROM memory.
- Uses `EmMemGet8` (returns uint8), `EmMemGet16` (returns uint16).
- Name buffer operations are all char/uint8. LP64-safe.
- `emuptr` arithmetic for name pointers. LP64-safe.
- File: `src/core/EmPalmFunction.cpp:1775-1869`

### 23. MacsbugNameLength function
- Determines Macsbug symbol format (fixed 8, fixed 16, variable).
- All operations on uint8 values from `EmMemGet8`. LP64-safe.
- File: `src/core/EmPalmFunction.cpp:1906-1954`

### 24. PrvLocalIDToPtr
- `(local & 0xFFFFFFFE) + cardInfo.baseP` — emuptr arithmetic. LP64-safe.
- File: `src/core/MetaMemory.cpp:3703-3715`

### 25. PrvGetRAMDatabaseDirectory
- Reads from emulated low memory, returns emuptr. LP64-safe.
- File: `src/core/MetaMemory.cpp:3723-3733`

### 26. EmPalmHeap::GetHeapByPtr round-trip
- emuptr → uintptr_t → MemPtr (void*) → uintptr_t → emuptr.
- Round-trip preserves the 32-bit emulated address. LP64-safe (ugly but works).
- File: `src/core/EmPalmHeap.cpp:242-264`

### 27. cpuop_func return type
- `typedef unsigned long cpuop_func(uae_u32)` — returns `unsigned long`.
- 8 bytes on LP64 vs 4 on ILP32. But return value is instruction cycle count,
  used only for profiling. Not security-critical. LP64-safe.
- File: `src/core/UAE/newcpu.h:56`

---

## Still Under Investigation (possible causes)

### A. AllowForBugs / InXxx() function name lookup
**Leading theory.** `AllowForBugs` uses `InPrvSystemTimerProc()`,
`InCrc16CalcBlock()`, etc. to decide whether to suppress known PalmOS ROM bugs.
These functions use `EmFunctionRange::InRange()` → `GetRange()` →
`FindFunctionName()` → `FindFunctionEnd()` → `GetMacsbugInfo()`.

If ANY step in this chain fails on LP64 (even though individual functions look
safe), ALL exemptions fail, and violations that should be suppressed get
reported. On 32-bit POSE, the same violations fire but AllowForBugs suppresses
them.

**Status:** Individual functions reviewed (see #22, #23). Need to test whether
InPrvSystemTimerProc() actually returns true on LP64 by adding instrumentation.

### B. InRAMOSComponent / PrvSearchForCodeChunk
For screen memory violations: `kScreenBits = 0x21` does NOT include
`kNoSystemAccess`. So ROM code (else branch of META_CHECK) should NOT trigger
screen violations. The violations must come from the IsPCInRAM branch, meaning
the code is in RAM. Then `InRAMOSComponent` must return false for this RAM-based
OS code, causing it to be treated as an app.

If `PrvSearchForCodeChunk` fails to find the code chunk on LP64 (e.g., due to
heap walking issues), InRAMOSComponent returns false, and the violation fires.

**Status:** Code reviewed, no obvious LP64 bug found. Need instrumentation.

### C. Address corruption in SRAM violations
Multiple different apps writing to the SAME storage heap address (0x400FC) is
a strong signal of address corruption. Either the emulated 68K instruction is
computing the wrong effective address, or the memory dispatch is mangling it.

**Status:** Register dumps captured but not yet analyzed in detail.

### D. Crash at EmSession::ExecuteSpecial+190
Occurs when hitting Continue on violation dialogs. rax=0x10000004a (33-bit
value — 0x1_0000_004A). This smells like a 32-bit emulated address that
somehow picked up a high bit.

**Status:** Not yet investigated.

---

## Key Insight

The same META_CHECK code runs on both platforms. The same meta-memory marking
runs on both platforms. The meta-memory bits at address 0x78 are kLowMemoryBits
(0x07) on both platforms. ROM code reading 0x78 fails CanSystemRead on BOTH
platforms. ProbableCause fires on BOTH platforms.

The difference must be in how the violation is SUPPRESSED after ProbableCause
fires. On 32-bit POSE, either:
1. `GetWhatHappened` → `AccessOK()` returns true more often (fgAccessCount > 0)
2. `AllowForBugs` successfully suppresses the violation
3. Some other suppression mechanism works on 32-bit but not LP64

The most likely cause is **AllowForBugs** (#A above), because it depends on
function name lookup which involves complex code (Macsbug name parsing, ROM
scanning, heap walking) that could have subtle LP64 issues even though
individual functions look safe.
