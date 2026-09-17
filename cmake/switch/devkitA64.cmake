# Nintendo Switch (Horizon / aarch64 / Tegra X1) cross toolchain, devkitPro devkitA64.
#
# Horizon is not a UNIX to CMake, so CMAKE_SYSTEM_NAME is Generic and every
# platform test that keys off UNIX has to be told about this target explicitly
# (see REX_SWITCH below and the branches it selects in the SDK's CMake).

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_VERSION 1)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# A Generic system has no runnable executables to probe with, so let CMake
# settle for building a static library when it tests the compiler.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(REX_SWITCH ON CACHE BOOL "Build for Nintendo Switch / libnx")
set(SKATE3_SWITCH ON CACHE BOOL "Build the Skate 3 app for Nintendo Switch")

if(NOT DEFINED DEVKITPRO)
    if(DEFINED ENV{DEVKITPRO})
        set(DEVKITPRO "$ENV{DEVKITPRO}" CACHE PATH "devkitPro root")
    else()
        set(DEVKITPRO "/opt/devkitpro" CACHE PATH "devkitPro root")
    endif()
endif()

set(_DKA64 "${DEVKITPRO}/devkitA64")
set(_DKA64_BIN "${_DKA64}/bin")
set(_PREFIX "aarch64-none-elf-")

set(CMAKE_C_COMPILER   "${_DKA64_BIN}/${_PREFIX}gcc" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER "${_DKA64_BIN}/${_PREFIX}g++" CACHE FILEPATH "" FORCE)
set(CMAKE_ASM_COMPILER "${_DKA64_BIN}/${_PREFIX}gcc" CACHE FILEPATH "" FORCE)
# gcc-ar/gcc-ranlib rather than the plain tools: they load the LTO plugin, which
# a later -flto experiment needs and which costs nothing without it.
set(CMAKE_AR      "${_DKA64_BIN}/${_PREFIX}gcc-ar"     CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB  "${_DKA64_BIN}/${_PREFIX}gcc-ranlib" CACHE FILEPATH "" FORCE)
set(CMAKE_NM      "${_DKA64_BIN}/${_PREFIX}nm"         CACHE FILEPATH "" FORCE)
set(CMAKE_OBJCOPY "${_DKA64_BIN}/${_PREFIX}objcopy"    CACHE FILEPATH "" FORCE)
set(CMAKE_OBJDUMP "${_DKA64_BIN}/${_PREFIX}objdump"    CACHE FILEPATH "" FORCE)
set(CMAKE_STRIP   "${_DKA64_BIN}/${_PREFIX}strip"      CACHE FILEPATH "" FORCE)
set(CMAKE_C_COMPILER_AR       "${CMAKE_AR}"     CACHE FILEPATH "" FORCE)
set(CMAKE_C_COMPILER_RANLIB   "${CMAKE_RANLIB}" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER_AR     "${CMAKE_AR}"     CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER_RANLIB "${CMAKE_RANLIB}" CACHE FILEPATH "" FORCE)

# -mtune, not -mcpu, at the toolchain level: the third-party libraries build
# against the baseline, and the recompiled guest gets -mcpu=cortex-a57 with
# outline atomics off from the SDK's own flag block, where the reasoning lives.
#
# -mtp=soft is not optional. Horizon does not let userspace read TPIDRRO_EL0
# the way Linux does, so libnx routes the thread pointer through a call; every
# object in the link has to agree, including the driver.
#
# -ftls-model=local-exec sidesteps the AArch64 linker relaxation that corrupts
# TLS access on secondary threads in a static PIE this large.
set(_SWITCH_ARCH "-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -ftls-model=local-exec -fPIE")
set(_SWITCH_DEFS "-D__SWITCH__ -DNX -D_GNU_SOURCE -DVK_USE_PLATFORM_VI_NN")
# Sections per function and per datum so --gc-sections can drop what the guest
# never calls, and so a profile can reorder .text.<symbol> at link time.
set(_SWITCH_SECTIONS "-ffunction-sections -fdata-sections")
# GCC warns about an AArch64 va_list ABI change nobody on this target can act on.
set(_SWITCH_WARN "-Wno-psabi")

set(CMAKE_C_FLAGS_INIT   "${_SWITCH_SECTIONS} ${_SWITCH_ARCH} ${_SWITCH_DEFS} ${_SWITCH_WARN}")
set(CMAKE_CXX_FLAGS_INIT "${_SWITCH_SECTIONS} ${_SWITCH_ARCH} ${_SWITCH_DEFS} ${_SWITCH_WARN}")
set(CMAKE_ASM_FLAGS_INIT "${_SWITCH_ARCH} ${_SWITCH_DEFS}")

# --no-relax for the TLS relaxation above. --gc-sections pairs with the
# per-function sections. No --allow-multiple-definition here: NVK's two Rust
# archives each carry a copy of the Rust runtime and genuinely need it, so it
# is applied to the one target that links them rather than to everything.
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "-specs=${DEVKITPRO}/libnx/switch.specs -Wl,--no-relax -Wl,--gc-sections")

# CMake fills CMAKE_DL_LIBS with "dl" for Generic after this file is read.
# libnx has no libdl and nothing here dlopens: Vulkan is linked statically.
set(CMAKE_DL_LIBS "")

include_directories(SYSTEM
    "${_DKA64}/include"
    "${DEVKITPRO}/libnx/include"
    "${DEVKITPRO}/portlibs/switch/include")
link_directories(
    "${_DKA64}/lib"
    "${DEVKITPRO}/libnx/lib"
    "${DEVKITPRO}/portlibs/switch/lib")

set(CMAKE_FIND_ROOT_PATH
    "${_DKA64}" "${DEVKITPRO}/libnx" "${DEVKITPRO}/portlibs/switch")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(ENV{PKG_CONFIG} "${DEVKITPRO}/portlibs/switch/bin/${_PREFIX}pkg-config")
set(ENV{PKG_CONFIG_PATH} "${DEVKITPRO}/portlibs/switch/lib/pkgconfig")
