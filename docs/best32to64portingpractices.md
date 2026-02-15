# Best Practices for Porting 32-bit C/C++ Applications to 64-bit UNIX

Focused on emulators and applications where bit-width correctness is critical.

---

## Table of Contents

1. [Understanding the Data Models](#1-understanding-the-data-models)
2. [The Core Problem: What Changes at 64-bit](#2-the-core-problem-what-changes-at-64-bit)
3. [Fixed-Width Integer Types](#3-fixed-width-integer-types)
4. [Pointer and Integer Casting](#4-pointer-and-integer-casting)
5. [Emulator-Specific Concerns](#5-emulator-specific-concerns)
6. [Magic Numbers](#6-magic-numbers)
7. [Bit Shifting Operations](#7-bit-shifting-operations)
8. [Structure Packing and Alignment](#8-structure-packing-and-alignment)
9. [Serialization and Binary Data](#9-serialization-and-binary-data)
10. [Array Indexing and Loop Counters](#10-array-indexing-and-loop-counters)
11. [Printf/Scanf Format Strings](#11-printfscanf-format-strings)
12. [Sign Extension Traps](#12-sign-extension-traps)
13. [Unions with Mixed Types](#13-unions-with-mixed-types)
14. [Virtual Functions and Overloads](#14-virtual-functions-and-overloads)
15. [Compiler Flags for Detection](#15-compiler-flags-for-detection)
16. [Static Analysis Tools](#16-static-analysis-tools)
17. [Porting Checklist](#17-porting-checklist)

---

## 1. Understanding the Data Models

The fundamental difference between 32-bit and 64-bit platforms is captured in the
**data model** — the convention for how wide each C type is.

| Type      | ILP32 (32-bit) | LP64 (Linux/macOS 64-bit) | LLP64 (Windows 64-bit) |
|-----------|:--------------:|:-------------------------:|:----------------------:|
| `char`    | 8              | 8                         | 8                      |
| `short`   | 16             | 16                        | 16                     |
| `int`     | **32**         | **32**                    | **32**                 |
| `long`    | **32**         | **64**                    | **32**                 |
| `long long` | 64           | 64                        | 64                     |
| pointer   | **32**         | **64**                    | **64**                 |
| `size_t`  | 32             | 64                        | 64                     |
| `ptrdiff_t` | 32           | 64                        | 64                     |

Key takeaways:
- On **ILP32**: `int`, `long`, and pointers are all 32 bits. Vast amounts of legacy code
  relies on this equivalence.
- On **LP64** (Linux/macOS): `long` and pointers grow to 64 bits, but `int` stays 32.
  This is the source of nearly every porting bug.
- On **LLP64** (Windows): Only pointers grow; `long` stays 32. Different set of gotchas.

**For UNIX porting, you are dealing with ILP32 -> LP64.**

---

## 2. The Core Problem: What Changes at 64-bit

In ILP32, this was true:

```
sizeof(int) == sizeof(long) == sizeof(void*) == 4
```

In LP64, this is true:

```
sizeof(int) == 4
sizeof(long) == sizeof(void*) == 8
```

Every place that assumed `int` and `pointer` are the same size, or that `long` is
32 bits, is a bug waiting to happen. The two most dangerous patterns are:

1. **Storing pointers in integer types** (truncation of upper 32 bits)
2. **Assuming `long` is 32 bits** (overflow or misinterpretation)

---

## 3. Fixed-Width Integer Types

The single most important tool for portable code is `<stdint.h>` (C99) / `<cstdint>` (C++11).

### Always-available exact-width types

| Type        | Width  | Use case                                   |
|-------------|--------|--------------------------------------------|
| `int8_t`    | 8-bit  | Byte-level data, registers                 |
| `uint8_t`   | 8-bit  | Unsigned byte data                         |
| `int16_t`   | 16-bit | 16-bit register values, PalmOS Word        |
| `uint16_t`  | 16-bit | Unsigned 16-bit values                     |
| `int32_t`   | 32-bit | 32-bit register values, emulated addresses |
| `uint32_t`  | 32-bit | Unsigned 32-bit, emulated pointers         |
| `int64_t`   | 64-bit | 64-bit values                              |
| `uint64_t`  | 64-bit | Unsigned 64-bit values                     |

### Pointer-width types (change size with platform)

| Type         | Width     | Use case                                   |
|--------------|-----------|-------------------------------------------|
| `intptr_t`   | ptr-width | Signed integer guaranteed to hold a pointer|
| `uintptr_t`  | ptr-width | Unsigned integer guaranteed to hold a pointer|
| `size_t`     | ptr-width | Array sizes, allocation sizes              |
| `ptrdiff_t`  | ptr-width | Pointer arithmetic results                 |
| `ssize_t`    | ptr-width | Signed size (POSIX)                        |

### Rule of thumb

- **Emulated/simulated data** (registers, memory addresses, file formats): use
  `uint32_t`, `int32_t`, etc. — exact-width types that never change.
- **Host data** (array indices, memory sizes, pointer arithmetic): use `size_t`,
  `ptrdiff_t`, `uintptr_t` — types that scale with the platform.
- **Never use `int` or `long` for either purpose** if correctness depends on width.

---

## 4. Pointer and Integer Casting

This is the single largest source of 64-bit porting bugs.

### The broken pattern

```c
// ILP32: works because sizeof(int) == sizeof(void*)
int addr = (int)some_pointer;           // TRUNCATES on LP64!
void *ptr = (void *)some_int;           // SIGN-EXTENDS on LP64!
```

### The fix

```c
// Use uintptr_t for round-tripping pointers through integers
uintptr_t addr = (uintptr_t)some_pointer;  // Always safe
void *ptr = (void *)(uintptr_t)some_uint;  // Always safe
```

### Masking pointers

```c
// WRONG: int mask truncates the pointer
p = (char *)((int)p & PAGEMASK);

// RIGHT: use uintptr_t
p = (char *)((uintptr_t)p & PAGEMASK);
```

### When you truly need a 32-bit handle

If the value is not a real host pointer but a 32-bit handle or emulated address:

```c
uint32_t handle = (uint32_t)(uintptr_t)ptr;  // Explicit two-step cast
```

This makes the truncation visible and intentional. Verify the pointer actually
fits in 32 bits (i.e., it came from a 32-bit address space).

---

## 5. Emulator-Specific Concerns

Emulators are a special case because they maintain **two address spaces**: the
host (64-bit) and the emulated guest (often 32-bit or 16-bit).

### The classic emulator bug

Original POSE-era emulator code for the Motorola 68000 mapped 32-bit emulated
addresses directly using host pointer offsets:

```c
// 32-bit era: emulated address == host pointer offset. Fine on ILP32.
unsigned long emulated_addr = 0x00012345;
char *host_ptr = base_memory + emulated_addr;  // Works if base is low
unsigned long roundtrip = (unsigned long)host_ptr;  // Gets back 0x00012345
```

On 64-bit, `base_memory` might be at `0x7FFF00000000`. The roundtrip fails
because the host address is now 48+ bits wide and won't fit in the old type.

### The solution: separate address spaces cleanly

```c
// Emulated addresses are ALWAYS uint32_t — never host pointers
typedef uint32_t emuAddr;

// Host memory base for the emulated RAM
static uint8_t *g_emulatedRAM;  // malloc'd, host pointer

// Convert emulated address -> host pointer
static inline uint8_t *emu_to_host(emuAddr addr) {
    assert(addr < EMULATED_RAM_SIZE);
    return g_emulatedRAM + addr;
}

// Convert host pointer -> emulated address (for pointers into emulated RAM)
static inline emuAddr host_to_emu(const uint8_t *ptr) {
    ptrdiff_t offset = ptr - g_emulatedRAM;
    assert(offset >= 0 && (size_t)offset < EMULATED_RAM_SIZE);
    return (emuAddr)offset;
}
```

### Key principles for emulator porting

1. **Never store host pointers in emulated-width types.** Use `uint32_t` for
   guest addresses, real `uint8_t *` for host pointers.
2. **Never store emulated addresses in `long` or `int`.** Use `uint32_t` always.
3. **Conversion functions are your firewall.** Every crossing between address
   spaces goes through `emu_to_host()` / `host_to_emu()`. Grep for raw casts
   and replace them.
4. **Emulated registers are fixed-width.** A 68k D0 register is `uint32_t`,
   period. An A0 register is `uint32_t` (emulated address), not a host pointer.
5. **Beware of address arithmetic.** `(emulated_addr + offset)` must wrap at
   32 bits for the guest, but `(host_ptr + offset)` uses 64-bit arithmetic.

---

## 6. Magic Numbers

Hard-coded numeric constants that assume 32-bit sizes are a pervasive problem.

### Common dangerous magic numbers

| Magic Number | What it usually means         | Safe replacement                   |
|-------------|-------------------------------|-------------------------------------|
| `4`         | `sizeof(int)` or `sizeof(void*)` | `sizeof(type_in_question)`      |
| `8`         | `sizeof(double)` or `sizeof(long)` on LP64 | `sizeof(type_in_question)` |
| `0xFFFFFFFF` | All-bits-set for 32-bit      | `(uint32_t)-1` or `UINT32_MAX`    |
| `32`        | Bits in an `int`              | `CHAR_BIT * sizeof(type)`         |
| `0x7FFFFFFF` | `INT_MAX`                    | `INT32_MAX` or `INT_MAX`          |
| `0x80000000` | Sign bit of 32-bit int       | `(uint32_t)1 << 31`              |

### Example

```c
// WRONG: assumes pointers are 4 bytes
size_t table_size = num_entries * 4;

// RIGHT
size_t table_size = num_entries * sizeof(void *);
```

For emulators, distinguish between host and guest magic numbers:

```c
// Guest magic number — this is correct, guest pointers ARE 4 bytes
uint32_t guest_table_size = num_entries * 4;

// Host magic number — must use sizeof
size_t host_table_size = num_entries * sizeof(void *);
```

---

## 7. Bit Shifting Operations

Shift expressions use the type of the left operand to determine result width.

### The bug

```c
// intending to create a 64-bit mask, but "1" is int (32-bit)
size_t mask = 1 << bit_position;  // UB if bit_position >= 32!
```

The literal `1` is `int` (32 bits). Shifting it by 32 or more is undefined behavior.

### The fix

```c
// Cast or suffix the constant to the target width
size_t mask = (size_t)1 << bit_position;
uint64_t mask64 = UINT64_C(1) << bit_position;
long mask_l = 1L << bit_position;  // LP64: long is 64-bit
```

For emulator code where you want 32-bit wrapping:

```c
// Explicitly 32-bit shift for guest register operations
uint32_t result = (uint32_t)1 << (shift_amount & 31);
```

---

## 8. Structure Packing and Alignment

LP64 aligns `long` and pointer members to 8-byte boundaries, inserting padding
that didn't exist in ILP32.

### The problem

```c
struct Foo {
    int a;       // 4 bytes
                 // 4 bytes PADDING (LP64 only!)
    long b;      // 8 bytes (was 4 on ILP32)
    int c;       // 4 bytes
                 // 4 bytes PADDING
    char *d;     // 8 bytes (was 4 on ILP32)
};
// ILP32: sizeof = 16 bytes
// LP64:  sizeof = 32 bytes (doubled!)
```

### The fix: reorder members by decreasing alignment

```c
struct Foo {
    char *d;     // 8 bytes (pointer first)
    long b;      // 8 bytes
    int a;        // 4 bytes
    int c;        // 4 bytes (no padding needed, ints pack together)
};
// LP64: sizeof = 24 bytes (saved 8 bytes)
```

### For binary-compatible structures (file formats, network protocols, emulated memory)

Use fixed-width types and explicit packing:

```c
#pragma pack(push, 1)
struct GuestRecord {
    uint16_t type;
    uint32_t addr;
    uint16_t size;
};
#pragma pack(pop)
// sizeof = 8 on ALL platforms
```

**Warning:** Packed structures may cause unaligned access penalties or faults on
some architectures (notably IA-64/Itanium and some ARM). Use `memcpy` to
read/write individual fields if alignment is a concern.

### Use `offsetof`, not manual calculation

```c
// WRONG: assumes packing you may not have
size_t offset = 4 + 8;  // "skip int a and long b"

// RIGHT
size_t offset = offsetof(struct Foo, c);
```

---

## 9. Serialization and Binary Data

Binary file formats and network protocols written by 32-bit code will break if
types change size.

### The problem

```c
// 32-bit code wrote this
size_t count = 42;
fwrite(&count, sizeof(count), 1, fp);  // wrote 4 bytes on ILP32

// 64-bit code reads it
size_t count;
fread(&count, sizeof(count), 1, fp);   // reads 8 bytes on LP64 — WRONG
```

### The fix: fixed-width types for all persistent data

```c
uint32_t count = 42;
fwrite(&count, sizeof(count), 1, fp);  // always 4 bytes

uint32_t count;
fread(&count, sizeof(count), 1, fp);   // always 4 bytes
```

### Rules for binary formats

1. **Never serialize `int`, `long`, `size_t`, or pointers.** Use `uint32_t`, `int16_t`, etc.
2. **Never serialize raw structs** that contain pointers or platform-dependent types.
3. **Document byte order.** Use `htonl()`/`ntohl()` or explicit byte-swapping.
4. **Use a version field** in the format header so you can detect old-format files.

### Emulator state save/restore

Emulator save states must use fixed-width types for all CPU registers, memory
contents, and I/O state. The save format should be identical regardless of host:

```c
struct SavedCPUState {
    uint32_t d[8];      // D0-D7
    uint32_t a[8];      // A0-A7
    uint32_t pc;        // Program counter
    uint16_t sr;        // Status register
};
```

---

## 10. Array Indexing and Loop Counters

### The problem

```c
// unsigned int maxes out at ~4 billion — fine for 32-bit address space
// but can't index arrays larger than 4GB on 64-bit
for (unsigned i = 0; i < huge_array_count; i++) { ... }
```

If `huge_array_count` exceeds `UINT_MAX`, the loop never terminates.

### The fix

```c
for (size_t i = 0; i < huge_array_count; i++) { ... }
```

### When 32-bit indices are correct

In emulator code, if you're iterating over emulated memory (which is bounded to
32-bit addresses), `uint32_t` is fine and communicates intent:

```c
// Iterating emulated 68k memory — 32-bit address space
for (uint32_t addr = start; addr < end; addr += 2) {
    uint16_t word = emu_read16(addr);
    ...
}
```

---

## 11. Printf/Scanf Format Strings

Format specifiers for platform-dependent types change between ILP32 and LP64.

### Problem cases and fixes

| Type        | Wrong          | Right (portable)                      |
|-------------|---------------|---------------------------------------|
| `size_t`    | `%u` or `%lu` | `%zu`                                 |
| `ssize_t`   | `%d` or `%ld` | `%zd`                                 |
| `ptrdiff_t` | `%d` or `%ld` | `%td`                                 |
| `int64_t`   | `%lld`        | `PRId64` from `<inttypes.h>`         |
| `uint32_t`  | `%u`          | `PRIu32` from `<inttypes.h>`         |
| `void *`    | `%x`          | `%p`                                  |
| `long`      | `%d`          | `%ld`                                 |

### Using `<inttypes.h>` macros

```c
#include <inttypes.h>

uint32_t addr = 0x12345678;
uint64_t big  = 0xDEADBEEFCAFEBABE;

printf("addr = 0x%08" PRIx32 "\n", addr);
printf("big  = 0x%016" PRIx64 "\n", big);
printf("size = %zu\n", sizeof(some_struct));
```

### Buffer sizes for pointer printing

```c
// WRONG: only 11 chars for "0x" + 8 hex digits
char buf[11];
sprintf(buf, "%p", ptr);  // overflow on LP64 — needs 18+ chars

// RIGHT
char buf[24];
snprintf(buf, sizeof(buf), "%p", (void *)ptr);
```

---

## 12. Sign Extension Traps

When a smaller signed type is promoted to a larger type, the sign bit is extended.
This creates insidious bugs.

### The problem

```c
int offset = -1;
size_t big = offset;    // sign-extends to 0xFFFFFFFFFFFFFFFF on LP64!
                        // That's 18 exabytes, not 4 GB
```

### Bit-field sign extension

```c
struct {
    unsigned int base : 15;
} reg;

size_t addr = reg.base << 17;
// "base" is promoted to int (signed!) before shift.
// If bit 14 is set, the result sign-extends through the upper 32 bits.

// Fix:
size_t addr = (unsigned int)(reg.base) << 17;
// Or better:
size_t addr = (size_t)(reg.base) << 17;
```

### Mixed signed/unsigned arithmetic

```c
int a = -1;
unsigned int b = 1;
// (a + b) is unsigned on 32-bit: 0 (correct for modular arithmetic)
// But if assigned to a long on LP64, result is 0x00000000 — this is fine.
// Danger is when (a < b) is false because a is converted to UINT_MAX.
```

**Rule:** Don't mix signed and unsigned types in comparisons or arithmetic
without explicit casts. Use `-Wsign-compare` and `-Wconversion`.

---

## 13. Unions with Mixed Types

Unions containing both pointer-width and fixed-width members are a common
source of bugs.

### The problem

```c
union Value {
    char *ptr;          // 8 bytes on LP64
    unsigned int num;   // 4 bytes — only covers half the union!
};

Value v;
v.ptr = some_pointer;
unsigned int n = v.num;  // reads only low 32 bits of the pointer!
```

### The fix

```c
union Value {
    char *ptr;
    uintptr_t num;     // matches pointer size on all platforms
};
```

For emulator code, if the union represents a guest value:

```c
union GuestValue {
    uint32_t as_addr;
    uint32_t as_uint;
    int32_t  as_int;
    // No host pointers in guest unions!
};
```

---

## 14. Virtual Functions and Overloads (C++)

### Virtual function signature mismatch

```c++
class Base {
    virtual void process(size_t count);  // 64-bit param on LP64
};

class Derived : public Base {
    virtual void process(unsigned int count);  // 32-bit param — NOT an override!
};
```

On ILP32 both are 32-bit and it "works." On LP64, `Derived::process` is a
different function, not an override. Use `override` keyword (C++11) to catch this:

```c++
class Derived : public Base {
    void process(size_t count) override;  // Compiler enforces match
};
```

### Overload resolution changes

```c++
void handle(int x);
void handle(long x);

ptrdiff_t val = 42;
handle(val);
// ILP32: calls handle(int) — ptrdiff_t is int-sized
// LP64:  calls handle(long) — ptrdiff_t is long-sized
```

**Fix:** Avoid overloading on `int` vs `long`. Use fixed-width types or templates.

---

## 15. Compiler Flags for Detection

### GCC / Clang flags for 64-bit porting

```
-Wall -Wextra                  # Baseline: catches many implicit conversion issues
-Wconversion                   # Warns on implicit narrowing conversions
-Wsign-conversion              # Warns on sign changes in conversions
-Wpointer-to-int-cast          # Warns when pointer is cast to smaller integer
-Wint-to-pointer-cast          # Warns when integer is cast to pointer
-Wsign-compare                 # Warns on signed/unsigned comparison
-Wformat                       # Validates printf/scanf format strings
-Wshorten-64-to-32             # (Clang only) Warns on implicit 64->32 narrowing
```

### Oracle/Solaris lint

```
lint -errchk=longptr64         # Checks pointer-to-int assignments
lint -errchk=longptr64,signext # Also checks sign extension issues
```

### Recommended build approach

Compile with maximum warnings, treat them as errors during the porting phase:

```makefile
CFLAGS += -Wall -Wextra -Wconversion -Wsign-conversion \
          -Wpointer-to-int-cast -Wint-to-pointer-cast \
          -Wsign-compare -Werror
```

Fix every warning. Then relax `-Werror` if needed for third-party code.

---

## 16. Static Analysis Tools

### PVS-Studio

The gold standard for 64-bit porting analysis. Its Viva64 module was specifically
designed to detect 64-bit portability issues. Detects all 20 categories of
porting bugs and produces detailed diagnostics with code examples.

### Cppcheck

Free, open-source static analyzer. Detects some portability issues but is less
specialized for 64-bit porting than PVS-Studio. Good as a complementary tool.

### Clang Static Analyzer / clang-tidy

Built into LLVM/Clang. The `clang-tidy` checks include:
- `bugprone-narrowing-conversions`
- `bugprone-signed-char-misuse`
- `cppcoreguidelines-narrowing-conversions`
- `portability-*` checks

### Recommended strategy

1. **First pass:** Compile with all warnings enabled (Section 15). Fix everything.
2. **Second pass:** Run `cppcheck` and/or `clang-tidy` with portability checks.
3. **Third pass (if available):** Run PVS-Studio with 64-bit diagnostics enabled.
4. **Ongoing:** Keep warnings enabled in CI to prevent regressions.

---

## 17. Porting Checklist

### Phase 1: Audit

- [ ] Identify all uses of `int`, `long`, `unsigned`, `unsigned long` that interact
      with pointers or sizes
- [ ] Grep for casts: `(int)`, `(long)`, `(unsigned)`, `(unsigned long)` applied to
      pointers
- [ ] Grep for magic numbers: `4` (pointer size), `0xFFFFFFFF`, `0x7FFFFFFF`, `32`
- [ ] Identify all binary file formats and serialization code
- [ ] Identify all union types with mixed pointer/integer members
- [ ] Check all printf/sprintf format strings for `%d`/`%u`/`%x` used with
      `size_t`, `long`, or pointers

### Phase 2: Fix types

- [ ] Replace `int`/`long` used as pointer containers with `uintptr_t`/`intptr_t`
- [ ] Replace `int`/`long` used for sizes/indices with `size_t`/`ptrdiff_t`
- [ ] Replace `int`/`long` in serialization with `uint32_t`/`int32_t` etc.
- [ ] For emulator code: typedef emulated address types as `uint32_t`, create
      conversion functions for host<->guest address translation
- [ ] Replace magic numbers with `sizeof()` expressions or named constants
- [ ] Fix shift expressions: cast left operand to target width
- [ ] Fix format strings: use `%zu`, `%td`, `PRIu32`, `PRIx64`, `%p`

### Phase 3: Structures and alignment

- [ ] Reorder struct members for natural alignment (largest first)
- [ ] Replace `#pragma pack` with fixed-width types where possible
- [ ] Use `offsetof()` instead of manual offset calculations
- [ ] Verify union members match in size or are intentionally different

### Phase 4: Build and test

- [ ] Compile with all warning flags from Section 15
- [ ] Fix all warnings (treat as errors)
- [ ] Run static analysis tools
- [ ] Test with both small and large (>4GB) data sets where applicable
- [ ] Test serialization: write on 32-bit, read on 64-bit (and vice versa)
- [ ] For emulators: verify all guest addresses stay within 32-bit range
- [ ] For emulators: verify host pointer arithmetic uses `ptrdiff_t`/`size_t`

### Phase 5: Ongoing

- [ ] Keep warning flags enabled in the build system
- [ ] Add static analysis to CI pipeline
- [ ] Document which types represent guest vs host values in emulator code
- [ ] Use `static_assert(sizeof(type) == expected)` for critical type assumptions

---

## References

- [20 Issues of Porting C++ Code to the 64-bit Platform (PVS-Studio)](https://pvs-studio.com/en/blog/posts/cpp/a0004/)
- [Migrating C/C++ from 32-Bit to 64-Bit (InformIT)](https://www.informit.com/articles/article.aspx?p=2339636)
- [Converting 32-Bit Applications Into 64-Bit Applications (Oracle/Solaris)](https://www.oracle.com/solaris/technologies/ilp32tolpissues.html)
- [Fixed-Width Integer Types (cppreference.com)](https://en.cppreference.com/w/c/types/integer.html)
- [Portable Fixed-Width Integers in C (Barr Group)](https://barrgroup.com/blog/portable-fixed-width-integers-c)
- [GCC Warning Options](https://gcc.gnu.org/onlinedocs/gcc/Warning-Options.html)
- [PVS-Studio Static Analyzer — 64-bit Diagnostics](https://pvs-studio.com/en/blog/lessons/0008/)
- [CloudpilotEmu — PalmOS emulator ported to 64-bit](https://github.com/cloudpilot-emu/cloudpilot-emu)
- [Rules for Using Pointers (Microsoft)](https://learn.microsoft.com/en-us/windows/win32/winprog64/rules-for-using-pointers)
