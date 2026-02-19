cmake_minimum_required(VERSION 3.19)
project(POSE64 VERSION 0.9.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_AUTOMOC ON)

# Debug build: cmake .. -DPOSE_DEBUG=ON
# Enables: debug symbols, ASAN (leak detection), UBSAN, exception/register history
# Keeps NDEBUG defined (assertions stay off)
# Phase 2: cmake .. -DPOSE_DEBUG=ON -DPOSE_ASSERTIONS=ON (removes NDEBUG, enables all EmAssert)
option(POSE_DEBUG "Enable debug symbols, ASAN, UBSAN, and diagnostic history" OFF)
option(POSE_ASSERTIONS "Enable EmAssert assertions (remove NDEBUG). Requires POSE_DEBUG=ON" OFF)

# Qt6
find_package(Qt6 6.6 REQUIRED COMPONENTS Core Widgets)
qt_standard_project_setup()

include(GNUInstallDirs)

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/Hardware
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/Hardware/TRG
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/Palm/Device
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/Palm/Platform
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/Palm/Platform/Core/Hardware/IncsPrv
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/Palm/Platform/Core/System/IncsPrv
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/Palm/Platform/Incs
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/Palm/Platform/Incs/Core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/Palm/Platform/Incs/Core/Hardware
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/Palm/Platform/Incs/Core/System
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/Palm/Platform/Incs/Core/UI
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/Palm/Platform/Incs/Libraries
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/Gzip
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/omnithread
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/Patches
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/UAE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
)

# Compiler definitions (common to all builds)
add_definitions(
    -DPLATFORM_UNIX=1
    -D__PALMOS_TRAPS__=0
    -DEMULATION_LEVEL=EMULATION_UNIX
    -DHAS_PROFILING=0
    -D_REENTRANT
    -DNoNanoSleep
    -DPthreadDraftVersion=10
    -DHAVE_ENDIAN_H=1
    -DHAVE_DIRENT_H=1
    -DSTDC_HEADERS=1
    -DHAVE_FCNTL_H=1
    -DHAVE_LIMITS_H=1
    -DHAVE_STRINGS_H=1
    -DHAVE_SYS_TIME_H=1
    -DHAVE_UNISTD_H=1
    -DHAVE_TYPE_SOCKLEN_T=1
    # Temporarily disable JPEG for initial port
    -DDISABLE_JPEG_SUPPORT=1
)

# Debug vs Release definitions
if(POSE_DEBUG)
    message(STATUS ">>> POSE_DEBUG build: ASAN + UBSAN + debug symbols enabled")
    # _DEBUG enables exception history and register history buffers
    add_definitions(-D_DEBUG)
    if(POSE_ASSERTIONS)
        message(STATUS ">>> POSE_ASSERTIONS: EmAssert enabled (NDEBUG removed)")
        # No NDEBUG — all 1194 EmAssert() calls are active
    else()
        message(STATUS ">>> Assertions OFF (NDEBUG defined) — safe for known false-positive issues")
        add_definitions(-DNDEBUG)
    endif()
    # Qt debug output enabled (qDebug, qWarning work)
else()
    # Release build: assertions off, Qt debug off
    add_definitions(-DNDEBUG)
    add_definitions(-DQT_NO_DEBUG)
endif()

# Compiler flags — CRITICAL: no -fpermissive, no -Wno-int-to-pointer-cast
add_compile_options(
    -fexceptions
    -Wall
    -Wno-multichar
    -Wno-unknown-pragmas
    -Wno-conversion
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-write-strings>
    # 64-BIT SAFETY: Pointer-int cast warnings are ERRORS
    $<$<COMPILE_LANGUAGE:CXX>:-Werror=int-to-pointer-cast>
    # Note: -Werror=pointer-to-int-cast is C-only (not valid for C++);
    # C++ uses -fpermissive for this, which we leave disabled
    $<$<COMPILE_LANGUAGE:C>:-Werror=pointer-to-int-cast>
)

# Debug instrumentation flags
if(POSE_DEBUG)
    add_compile_options(
        -g3                      # Full debug symbols (includes macro defs)
        -O1                      # Light optimization (keeps ASAN fast, GDB readable)
        -fno-omit-frame-pointer  # Complete stack traces
        -fsanitize=address       # AddressSanitizer: OOB, use-after-free, leaks
        -fsanitize=undefined     # UBSan: integer overflow, null deref, alignment
    )
    add_link_options(
        -fsanitize=address
        -fsanitize=undefined
    )
endif()

# Collect all core source files
file(GLOB_RECURSE CORE_SOURCES
    "src/core/*.cpp"
    "src/core/*.c"
)

# Exclude bundled Gzip — symbols collide with system zlib
list(FILTER CORE_SOURCES EXCLUDE REGEX "src/core/Gzip/.*\\.(c|cpp)$")

# Exclude Palm SDK implementation files (headers only, except Crc.c)
list(FILTER CORE_SOURCES EXCLUDE REGEX "src/core/Palm/.*\\.cpp$")
list(FILTER CORE_SOURCES EXCLUDE REGEX "src/core/Palm/.*\\.c$")

# Add back essential Palm OS implementations
list(APPEND CORE_SOURCES "src/core/Palm/Platform/Core/System/Src/Crc.c")

# Exclude all omnithread platform implementations
list(FILTER CORE_SOURCES EXCLUDE REGEX "src/core/omnithread/.*\\.(c|cpp)$")

# Exclude duplicate JPEG memory manager implementations
list(FILTER CORE_SOURCES EXCLUDE REGEX "src/core/jpeg/jmemmac\\.c$")

# Exclude ALL UAE files (add back explicitly)
list(FILTER CORE_SOURCES EXCLUDE REGEX "src/core/UAE/.*\\.(c|cpp)$")

# UAE CPU emulator sources
set(UAE_SOURCES
    src/core/UAE/cpudefs.c
    src/core/UAE/cpuemu.c
    src/core/UAE/cpustbl.c
    src/core/UAE/readcpu.cpp
)

# Platform abstraction layer — Qt implementations
set(PLATFORM_SOURCES
    src/platform/EmApplicationQt.cpp
    src/platform/EmApplicationQt.h
    src/platform/EmWindowQt.cpp
    src/platform/EmWindowQt.h
    src/platform/EmDlgQt.cpp
    src/platform/EmDlgQt.h
    src/platform/EmDirRefUnix.cpp
    src/platform/EmFileRefUnix.cpp
    src/platform/EmPixMapUnix.cpp
    src/platform/EmTransportSerialUnix.cpp
    src/platform/EmTransportUSBUnix.cpp
    src/platform/Platform_Unix.cpp
    src/platform/EmDocumentUnix.cpp
    src/platform/ResStrings.cpp
)

# UI sources
set(UI_SOURCES
    src/ui/main.cpp
)

# Main executable
qt_add_executable(pose64
    ${CORE_SOURCES}
    ${UAE_SOURCES}
    ${PLATFORM_SOURCES}
    ${UI_SOURCES}
)

# Qt resources (default skin, etc.)
qt_add_resources(pose64 "default_skin"
    PREFIX "/"
    BASE src/ui
    FILES src/ui/DefaultLarge.png
)

# Print source file counts for debugging
list(LENGTH CORE_SOURCES CORE_COUNT)
list(LENGTH UAE_SOURCES UAE_COUNT)
message(STATUS "Core sources: ${CORE_COUNT} files")
message(STATUS "UAE sources: ${UAE_COUNT} files")

target_link_libraries(pose64 PRIVATE
    Qt6::Core
    Qt6::Widgets
    pthread
    m
    z
    X11
)

# Install
install(TARGETS pose64 RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY Skins/ DESTINATION ${CMAKE_INSTALL_DATADIR}/pose64/Skins)
install(FILES data/ca.vibekoder.pose64.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/ca.vibekoder.pose64.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES data/ca.vibekoder.pose64.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps)
install(FILES data/icons/256x256.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps RENAME ca.vibekoder.pose64.png)
install(FILES data/icons/128x128.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/128x128/apps RENAME ca.vibekoder.pose64.png)
install(FILES data/icons/64x64.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/64x64/apps RENAME ca.vibekoder.pose64.png)
install(FILES data/icons/48x48.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/48x48/apps RENAME ca.vibekoder.pose64.png)
install(FILES data/icons/32x32.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/32x32/apps RENAME ca.vibekoder.pose64.png)
install(FILES data/icons/24x24.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/24x24/apps RENAME ca.vibekoder.pose64.png)
install(FILES data/icons/16x16.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/16x16/apps RENAME ca.vibekoder.pose64.png)
