cmake_minimum_required(VERSION 3.19)
project(QtPOSE VERSION 2.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_AUTOMOC ON)

# Qt6
find_package(Qt6 6.6 REQUIRED COMPONENTS Core Widgets)
qt_standard_project_setup()

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Hardware
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Hardware/TRG
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Palm/Device
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Palm/Platform
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Palm/Platform/Core/Hardware/IncsPrv
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Palm/Platform/Core/System/IncsPrv
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Palm/Platform/Incs
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Palm/Platform/Incs/Core
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Palm/Platform/Incs/Core/Hardware
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Palm/Platform/Incs/Core/System
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Palm/Platform/Incs/Core/UI
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Palm/Platform/Incs/Libraries
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Gzip
    ${CMAKE_CURRENT_SOURCE_DIR}/core/jpeg
    ${CMAKE_CURRENT_SOURCE_DIR}/core/omnithread
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Patches
    ${CMAKE_CURRENT_SOURCE_DIR}/core/UAE
    ${CMAKE_CURRENT_SOURCE_DIR}/platform
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
)

# Compiler definitions
add_definitions(
    -DPLATFORM_UNIX=1
    -D__PALMOS_TRAPS__=0
    -DEMULATION_LEVEL=EMULATION_UNIX
    -DHAS_PROFILING=0
    -DNDEBUG
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

# Collect all core source files
file(GLOB_RECURSE CORE_SOURCES
    "core/*.cpp"
    "core/*.c"
)

# Exclude bundled Gzip — symbols collide with system zlib
list(FILTER CORE_SOURCES EXCLUDE REGEX "core/Gzip/.*\\.(c|cpp)$")

# Exclude Palm SDK implementation files (headers only, except Crc.c)
list(FILTER CORE_SOURCES EXCLUDE REGEX "core/Palm/.*\\.cpp$")
list(FILTER CORE_SOURCES EXCLUDE REGEX "core/Palm/.*\\.c$")

# Add back essential Palm OS implementations
list(APPEND CORE_SOURCES "core/Palm/Platform/Core/System/Src/Crc.c")

# Exclude all omnithread platform implementations
list(FILTER CORE_SOURCES EXCLUDE REGEX "core/omnithread/.*\\.(c|cpp)$")

# Exclude duplicate JPEG memory manager implementations
list(FILTER CORE_SOURCES EXCLUDE REGEX "core/jpeg/jmemmac\\.c$")

# Exclude ALL UAE files (add back explicitly)
list(FILTER CORE_SOURCES EXCLUDE REGEX "core/UAE/.*\\.(c|cpp)$")

# UAE CPU emulator sources
set(UAE_SOURCES
    core/UAE/cpudefs.c
    core/UAE/cpuemu.c
    core/UAE/cpustbl.c
    core/UAE/readcpu.cpp
)

# Platform abstraction layer — Qt implementations (v2, no bridge thread)
set(PLATFORM_SOURCES
    platform/EmApplicationQt.cpp
    platform/EmApplicationQt.h
    platform/EmWindowQt.cpp
    platform/EmWindowQt.h
    platform/EmDlgQt.cpp
    platform/EmDlgQt.h
    platform/EmDirRefUnix.cpp
    platform/EmFileRefUnix.cpp
    platform/EmPixMapUnix.cpp
    platform/EmTransportSerialUnix.cpp
    platform/EmTransportUSBUnix.cpp
    platform/Platform_Unix.cpp
    platform/EmDocumentUnix.cpp
    platform/ResStrings.cpp
)

# UI sources
set(UI_SOURCES
    ui/main.cpp
)

# Main executable
qt_add_executable(qtpose
    ${CORE_SOURCES}
    ${UAE_SOURCES}
    ${PLATFORM_SOURCES}
    ${UI_SOURCES}
)

# Print source file counts for debugging
list(LENGTH CORE_SOURCES CORE_COUNT)
list(LENGTH UAE_SOURCES UAE_COUNT)
message(STATUS "Core sources: ${CORE_COUNT} files")
message(STATUS "UAE sources: ${UAE_COUNT} files")

target_link_libraries(qtpose PRIVATE
    Qt6::Core
    Qt6::Widgets
    pthread
    m
    z
    X11
)

# Install
install(TARGETS qtpose RUNTIME DESTINATION bin)
