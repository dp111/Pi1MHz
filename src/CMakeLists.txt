
#   Part of the Raspberry-Pi Bare Metal Tutorials
#   Copyright (c) 2015, Brian Sidebotham
#   All rights reserved.

#   Redistribution and use in source and binary forms, with or without
#   modification, are permitted provided that the following conditions are met:

#   1. Redistributions of source code must retain the above copyright notice,
#       this list of conditions and the following disclaimer.

#   2. Redistributions in binary form must reproduce the above copyright notice,
#       this list of conditions and the following disclaimer in the
#       documentation and/or other materials provided with the distribution.

#   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
#   AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
#   IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
#   ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
#   LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
#   CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
#   SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
#   INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
#   CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
#   ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
#   POSSIBILITY OF SUCH DAMAGE.

# CMake build environment for the Valvers Raspberry-Pi bare metal tutorials

# Presets (CMakePresets.json) require CMake 3.21 or newer.
cmake_minimum_required( VERSION 3.21 )

# Mark the language as C so that CMake doesn't try to test the C++
# cross-compiler's ability to compile a simple program because that will fail
project( Pi1MHz C ASM )

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu23" )

# Replace newlib's _ctype_ table lookups for toupper/tolower with 3-instruction
# ASCII range checks. Force-included so that source files keep using plain
# <ctype.h> and plain toupper()/tolower() -- see lib/ascii_ctype.h.
set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -include ${PROJECT_SOURCE_DIR}/lib/ascii_ctype.h" )

if(EXISTS ${PROJECT_SOURCE_DIR}/FIQ.s)
# comment out line below if FIQ code doesn't sit directly at the FIQ vector
add_definitions( -DFIQ_VECTOR_CODE=1 )
endif()

# Add any C compiler flags here. The C flags concerned with processor tuning
# are in the toolchain files toolchain-arm-none-eabi-bcm2835.cmake, etc.
set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -pipe" )
# The following line enables using memory form 0x00000000 onwards
set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} --param=min-pagesize=0 ")
set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wmaybe-uninitialized ")

set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wredundant-decls -Wpacked -Wundef -Wshadow -Wmissing-braces -Wmultichar")
set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wnarrowing -Woverflow -Wconversion -Wjump-misses-init")
set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wlogical-not-parentheses -Wnull-dereference -Winit-self")
set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wpointer-arith -Wwrite-strings -Wunreachable-code")
set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wdouble-promotion -Wformat-truncation -fstack-usage" )

set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wformat=2")
set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wcast-qual")
set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -pedantic")
set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wsign-conversion")
set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wsign-compare" )
set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wlong-long" )
set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wmissing-field-initializers" )

set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wunused -Wuninitialized -Wduplicated-branches ")

set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wduplicated-cond -Wnested-externs -Wfloat-conversion ")
set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Winline")
set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wmissing-include-dirs  ")
set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Woverlength-strings -Wdisabled-optimization -Wnormalized -Wformat-overflow=1 ")
set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wrestrict -Wshift-overflow=2 -Wnull-dereference -Wmain ")
set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wstringop-overflow=4 -Wstringop-overread -Wstrict-prototypes ")
set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wreturn-local-addr -Wlogical-op -fstrict-aliasing ")
set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wstrict-aliasing=3 -Wunsafe-loop-optimizations -Wformat-signedness ")
set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Walloc-zero -Wdiscarded-qualifiers -Wtrampolines -Wbad-function-cast ")
set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Waddress-of-packed-member -Wsuggest-attribute=noreturn ")
set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wdiscarded-array-qualifiers -Wmissing-noreturn ")
set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wflex-array-member-not-at-end -ftabstop=8  ")

set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wdeprecated-declarations -Wfloat-equal -Wmissing-format-attribute " )

set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wenum-int-mismatch -Wxor-used-as-pow -fstrict-flex-arrays -Warray-bounds=2")

set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Warith-conversion ")

#set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fanalyzer" )

set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -nostartfiles " )


set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} --specs=nosys.specs --specs=nano.specs" )
# -u _printf_float is intentionally NOT here: the only release-build float
# format (helpers.c, temperature display) is hand-formatted to avoid it, and
# nano newlib's printf omits float support by default, saving the dtoa/float
# machinery (~5 KB). The one caller that still needs %f -- dump_useful_info()
# in rpi/info.c -- is itself compiled only under #ifdef DEBUG, so the flag is
# added back below, only for DEBUG builds.

# current failures in frame buffer.c
# Level 2 is the highest actionable level; 3-5 are documented-noisy and
# produce false positives on ordinary screen-coordinate arithmetic.
set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wstrict-overflow=2")

set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wcast-align -Wcast-align=strict")
#-Wunused-macros

set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-unused-parameter ")

set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -flto=auto")
# Set the linker flags so that we use our "custom" linker script

#set( CMAKE_EXE_LINKER_FLAGS "-Wl,--gc-sections -Wl,--print-gc-sections -Wl,-unresolved-symbols=ignore-in-object-files -Wl,-T,${PROJECT_SOURCE_DIR}/rpi.ld" )

set( CMAKE_EXE_LINKER_FLAGS " -Wl,--defsym=START_ADDR=0x8000 -Wl,-T,${PROJECT_SOURCE_DIR}/rpi/rpi.ld " )

set(core_files
   # Architecture-optimised string routines, built with our real -mcpu flags.
   # The prebuilt libc.a is compiled -Os for a weaker multilib than either
   # target, so its memcpy/strlen/strcmp are byte loops. See lib/armstring/README.md.
   # Each dispatch file selects its own variant; files that have nothing to
   # offer a given target emit no symbol and fall through to libc as before.
   lib/armstring/memcpy.S
   lib/armstring/strcmp.S
   lib/armstring/strlen.S
   lib/armstring/memchr.S
   # Ours: newlib has no memset.S for ARM at all, so libc's is a byte loop on
   # every target. Architecture-neutral, so this one file serves both.
   # Tested: 13152/13152 cases under qemu, see lib/armstring-pi/test/run.sh.
   lib/armstring-pi/memset.S
   # arm-mem (Ben Avison, 3-clause BSD) memcpy/memmove/memcmp for ARMv6 --
   # newlib has no ARMv6 memcpy at all and no ARM memcmp for any target.
   # rpi3 keeps newlib's NEON memcpy and the generic C memcmp; only rpi picks
   # these up. Tested: 20632/20632 cases under qemu, see lib/armstring-pi/test/run.sh.
   lib/armstring-pi/memcpy-arm.S
   lib/armstring-pi/memcmp-arm.S
   # Ours: newlib's strlen dispatch needs Thumb-2 or ARMv7-A, so ARM1176 has
   # no fast strlen at all. rpi3 keeps newlib's NEON version.
   # Tested: 4840/4840 cases under qemu, see lib/armstring-pi/test/run.sh.
   lib/armstring-pi/strlen-arm1176.S
   # newlib's own strcase* sources, recompiled here so the force-included
   # ascii_ctype.h replaces their _ctype_ table lookups. libc's prebuilt copies
   # reference the table directly; ours do not. See lib/newlib-str/README.md.
   lib/newlib-str/strcasecmp.c
   lib/newlib-str/strncasecmp.c
   lib/newlib-str/strcasestr.c
   rpi/arm-start.S
   rpi/armc-cstubs.c
   rpi/exception.c
   rpi/cache.c
   rpi/gpio.c
   rpi/auxuart.c
   rpi/mailbox.c
   rpi/systimer.c
   rpi/info.c
   rpi/audio.c
   rpi/performance.c
   rpi/rpi.c
   rpi/asm-helpers.c
   rpi/screen.c
   Pi1MHz.c
   config.c
   config.h
   ram_emulator.c
   helpers.c
   videoplayer.c
   rpi/decompress.S
   rpi/fileparser.c
   usb.c
   usb/board_millis.c
   usb/mtp_fs.c
   usb/tinyusb/src/tusb.c
   usb/tinyusb/src/common/tusb_fifo.c
   usb/tinyusb/src/device/usbd.c
   usb/tinyusb/src/class/mtp/mtp_device.c
   usb/tinyusb/src/portable/synopsys/dwc2/dcd_dwc2.c
   usb/tinyusb/src/portable/synopsys/dwc2/dwc2_common.c
)



# Harddisc emulator
set(harddisc_emulator_files
   harddisc_emulator.c
   BeebSCSI/debug.c
   BeebSCSI/filesystem.c
   BeebSCSI/scsi.c
   BeebSCSI/fcode.c
   BeebSCSI/fatfs/ff.c
   BeebSCSI/fatfs/diskio.c
   BeebSCSI/fatfs/ffunicode.c
   rpi/sdcard.c
)
include_directories(BeebSCSI)
# TinyUSB is third-party - mark as SYSTEM so its headers (e.g. board_api.h)
# do not trip our strict warnings such as -Warith-conversion / -Wconversion.
include_directories(SYSTEM usb/tinyusb/src)
include_directories(SYSTEM usb/tinyusb/hw)
include_directories(usb)

set(LWIP_ROOT wifi/lwip)

# lwIP is third-party - mark as SYSTEM so its headers do not trip our
# strict warnings (same rationale as the TinyUSB includes above).
include_directories(SYSTEM ${LWIP_ROOT}/src/include)
include_directories(wifi)
include_directories(AUN)

# M5000 emulator
set(M5000_emulator_files
   rpi/audio.c
   M5000_emulator.c
)

# BeebSID (FastSID) — shares rpi/audio.c with M5000; mutex in Pi1MHz.c
set(BeebSID_emulator_files
   BeebSID/BeebSid.c
   BeebSID/BeebSid.h
   fastsid/beebsid_sid.c
   fastsid/fastsid.c
)
include_directories(fastsid)
set_source_files_properties(
   fastsid/fastsid.c
   PROPERTIES
   COMPILE_OPTIONS "-Wno-unused-parameter;-Wno-sign-conversion;-Wno-conversion;-Wno-float-conversion;-Wno-double-promotion;-Wno-shadow;-Wno-implicit-fallthrough"
)

#FrameBuffer
set(FrameBuffer_emulator_files
   framebuffer/framebuffer.c
   framebuffer/framebuffer.h
   framebuffer/screen_modes.c
   framebuffer/screen_modes.h
   framebuffer/primitives.c
   framebuffer/primitives.h
   framebuffer/fonts.c
   framebuffer/fonts.h
   framebuffer/teletext.c
   framebuffer/teletext.h
   rpi/armtimer.c
   rpi/interrupts.c
)

# Discaccess emulator (AUN commands 30+ share its command interface)
set(discaccess_emulator_files
   discaccess_emulator.c
   AUN/aun.c
   AUN/aun.h
   AUN/aun_config.c
   AUN/aun_config.h
   AUN/aun_emulator.c
   AUN/aun_emulator.h
)

set(teletext_emulator_files
   teletext_emulator.c
   teletext_emulator.h
)

set(mouse_redirect_files
   mouseredirect.c
)

set(wifi_files
   wifi/cyw43.c
   wifi/framebuffer_export.c
   wifi/md5.c
   wifi/md5.h
   wifi/sdio.c
   wifi/sdio_host.c
   wifi/wifi.c
   wifi/wifi_lwip.c
   wifi/wifi.h
   wifi/wifi_lwip.h
   wifi/webserver.c
   wifi/webserver.h
   wifi/netname.c
   wifi/netname.h
)

set(lwip_files
   ${LWIP_ROOT}/src/core/def.c
   ${LWIP_ROOT}/src/core/dns.c
   ${LWIP_ROOT}/src/core/inet_chksum.c
   ${LWIP_ROOT}/src/core/init.c
   ${LWIP_ROOT}/src/core/ip.c
   ${LWIP_ROOT}/src/core/mem.c
   ${LWIP_ROOT}/src/core/memp.c
   ${LWIP_ROOT}/src/core/netif.c
   ${LWIP_ROOT}/src/core/pbuf.c
   ${LWIP_ROOT}/src/core/stats.c
   ${LWIP_ROOT}/src/core/sys.c
   ${LWIP_ROOT}/src/core/tcp.c
   ${LWIP_ROOT}/src/core/tcp_in.c
   ${LWIP_ROOT}/src/core/tcp_out.c
   ${LWIP_ROOT}/src/core/timeouts.c
   ${LWIP_ROOT}/src/core/udp.c
   ${LWIP_ROOT}/src/core/ipv4/acd.c
   ${LWIP_ROOT}/src/core/ipv4/autoip.c
   ${LWIP_ROOT}/src/core/ipv4/dhcp.c
   ${LWIP_ROOT}/src/core/ipv4/etharp.c
   ${LWIP_ROOT}/src/core/ipv4/icmp.c
   ${LWIP_ROOT}/src/core/ipv4/ip4.c
   ${LWIP_ROOT}/src/core/ipv4/ip4_addr.c
   ${LWIP_ROOT}/src/core/ipv4/ip4_frag.c
   ${LWIP_ROOT}/src/netif/ethernet.c
)

set_source_files_properties(
   usb/tinyusb/src/tusb.c
   usb/tinyusb/src/common/tusb_fifo.c
   usb/tinyusb/src/device/usbd.c
   usb/tinyusb/src/class/mtp/mtp_device.c
   usb/tinyusb/src/portable/synopsys/dwc2/dcd_dwc2.c
   usb/tinyusb/src/portable/synopsys/dwc2/dwc2_common.c
   PROPERTIES
   COMPILE_OPTIONS "-Wno-arith-conversion;-Wno-strict-overflow;-Wno-attributes;-Wno-packed;-Wno-conversion;-Wno-switch-enum;-Wno-bad-function-cast"
)

set_source_files_properties(
   ${LWIP_ROOT}/src/core/def.c
   ${LWIP_ROOT}/src/core/dns.c
   ${LWIP_ROOT}/src/core/inet_chksum.c
   ${LWIP_ROOT}/src/core/init.c
   ${LWIP_ROOT}/src/core/ip.c
   ${LWIP_ROOT}/src/core/mem.c
   ${LWIP_ROOT}/src/core/memp.c
   ${LWIP_ROOT}/src/core/netif.c
   ${LWIP_ROOT}/src/core/pbuf.c
   ${LWIP_ROOT}/src/core/stats.c
   ${LWIP_ROOT}/src/core/sys.c
   ${LWIP_ROOT}/src/core/tcp.c
   ${LWIP_ROOT}/src/core/tcp_in.c
   ${LWIP_ROOT}/src/core/tcp_out.c
   ${LWIP_ROOT}/src/core/timeouts.c
   ${LWIP_ROOT}/src/core/udp.c
   ${LWIP_ROOT}/src/core/ipv4/acd.c
   ${LWIP_ROOT}/src/core/ipv4/autoip.c
   ${LWIP_ROOT}/src/core/ipv4/dhcp.c
   ${LWIP_ROOT}/src/core/ipv4/etharp.c
   ${LWIP_ROOT}/src/core/ipv4/icmp.c
   ${LWIP_ROOT}/src/core/ipv4/ip4.c
   ${LWIP_ROOT}/src/core/ipv4/ip4_addr.c
   ${LWIP_ROOT}/src/core/ipv4/ip4_frag.c
   ${LWIP_ROOT}/src/netif/ethernet.c
   PROPERTIES
   COMPILE_OPTIONS "-Wno-duplicated-branches;-Wno-arith-conversion;-Wno-sign-conversion;-Wno-strict-overflow;-Wno-attributes;-Wno-packed;-Wno-conversion"
)


if( DEBUG )

   set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DDEBUG=1 " )
   # dump_useful_info() (rpi/info.c) is only compiled in under DEBUG and is the
   # only remaining caller of printf's float formatting -- see the comment
   # next to --specs=nano.specs above.
   set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -u _printf_float" )

endif()


# Single source-of-truth list of everything linked into Pi1MHz. Reused below so
# gitversion.h's build timestamp tracks changes to any of these files.
set( Pi1MHz_sources
    ${core_files}
    ${harddisc_emulator_files}
    ${M5000_emulator_files}
    ${BeebSID_emulator_files}
    ${FrameBuffer_emulator_files}
    ${discaccess_emulator_files}
    ${teletext_emulator_files}
    ${mouse_redirect_files}
    ${wifi_files}
    ${lwip_files}
)

# These are cold (config/filename handling, ~12 call sites). We recompile them
# only to get the _ctype_ table lookups out, not for speed, so build them for
# size -- at -O2 newlib's two-way strcasestr unrolls into ~2.5KB.
# They are vendored unmodified (see lib/newlib-str/README.md), so silence the
# strict warnings their upstream style trips (-Wcast-qual, -Wconversion) --
# same rationale as the SYSTEM include treatment for TinyUSB/lwIP.
set_source_files_properties(
   lib/newlib-str/strcasecmp.c
   lib/newlib-str/strncasecmp.c
   lib/newlib-str/strcasestr.c
   PROPERTIES COMPILE_OPTIONS "-Os;-Wno-cast-qual;-Wno-conversion" )

add_executable( Pi1MHz ${Pi1MHz_sources} )

target_link_libraries( Pi1MHz PRIVATE m )

# Kernel images always go to the top-level firmware/ directory, regardless of
# where the build directory is (so out-of-source build/<config> dirs work).
get_filename_component( FIRMWARE_DIR "${CMAKE_SOURCE_DIR}/../firmware" ABSOLUTE )

add_custom_command(
   TARGET Pi1MHz POST_BUILD
   COMMAND ${CMAKE_OBJCOPY} ./Pi1MHz -O binary ${FIRMWARE_DIR}/${KERNEL_NAME}
   COMMAND ls -l ${FIRMWARE_DIR}/${KERNEL_NAME}
   WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
   COMMENT "Convert the ELF output file to a binary image" )

# Generate scripts/gitversion.h with the git commit id (+ -dirty marker) and a
# build timestamp. The header - and therefore the embedded BUILD_DATE - is
# regenerated only when a source file that goes into Pi1MHz changes; an
# unchanged tree rebuilds nothing. Pure CMake, so no bash dependency.

set( GITVERSION_HEADER ${CMAKE_CURRENT_SOURCE_DIR}/scripts/gitversion.h )

add_custom_command(
   OUTPUT ${GITVERSION_HEADER}
   COMMAND ${CMAKE_COMMAND}
           -DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}
           -DOUT=${GITVERSION_HEADER}
           -P ${CMAKE_CURRENT_SOURCE_DIR}/scripts/gitversion.cmake
   DEPENDS ${Pi1MHz_sources}
   COMMENT "Generating gitversion.h"
   VERBATIM
)

# A target wrapper so the header is brought up to date before any Pi1MHz source
# (which #includes it) is compiled.
add_custom_target( gitversion DEPENDS ${GITVERSION_HEADER} )
add_dependencies( Pi1MHz gitversion )
