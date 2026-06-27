
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

# CMake 2.8.10 Documentation: http://www.cmake.org/cmake/help/v2.8.10/cmake.html

cmake_minimum_required( VERSION 3.10 )

# Mark the language as C so that CMake doesn't try to test the C++
# cross-compiler's ability to compile a simple program because that will fail
project( Pi1MHz C ASM )

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS}" )
set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu23" )

if(EXISTS ${PROJECT_SOURCE_DIR}/FIQ.s)
# comment out line below if FIQ code doesn't sit directly at the FIQ vector
add_definitions( -DFIQ_VECTOR_CODE=1 )
endif()

# Add any C compiler flags here. The C flags concerned with processor tuning
# are in the toolchain files toolchain-arm-none-eabi-bcm2835.cmake, etc.
set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2" )
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

set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ffunction-sections")

set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} --specs=nosys.specs --specs=nano.specs -u _printf_float" )

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

set( CMAKE_EXE_LINKER_FLAGS "-Wl,--gc-sections -Wl,--defsym=START_ADDR=0x8000 -Wl,-T,${PROJECT_SOURCE_DIR}/rpi/rpi.ld " )

file( GLOB core_files
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
file( GLOB harddisc_emulator_files
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
file( GLOB M5000_emulator_files
   rpi/audio.c
   M5000_emulator.c
)

#FrameBuffer
file( GLOB FrameBuffer_emulator_files
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
file( GLOB discaccess_emulator_files
   discaccess_emulator.c
   AUN/aun.c
   AUN/aun.h
   AUN/aun_config.c
   AUN/aun_config.h
   AUN/aun_emulator.c
   AUN/aun_emulator.h
)

file( GLOB teletext_emulator_files
   teletext_emulator.c
   teletext_emulator.h
)

file( GLOB mouse_redirect_files
   mouseredirect.c
)

file( GLOB wifi_files
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

file( GLOB lwip_files
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


if( ${DEBUG} )

   set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DDEBUG=1 " )

endif()


    # Single source-of-truth list of everything linked into Pi1MHz. Reused below
    # so gitversion.h's build timestamp tracks changes to any of these files.
    set( Pi1MHz_sources
        ${core_files}
        ${harddisc_emulator_files}
        ${M5000_emulator_files}
        ${FrameBuffer_emulator_files}
        ${discaccess_emulator_files}
        ${teletext_emulator_files}
        ${mouse_redirect_files}
        ${wifi_files}
        ${lwip_files}
    )

    if( ${DEBUG} )

        add_executable( Pi1MHz ${Pi1MHz_sources} )

    else()

        add_executable( Pi1MHz ${Pi1MHz_sources} )

    endif()

target_link_libraries (Pi1MHz m)

# Kernel images always go to the top-level firmware/ directory, regardless of
# where the build directory is (so out-of-source build/<config> dirs work).
get_filename_component( FIRMWARE_DIR "${CMAKE_SOURCE_DIR}/../firmware" ABSOLUTE )

add_custom_command(
   TARGET Pi1MHz POST_BUILD
   COMMAND ${CMAKE_OBJCOPY} ./Pi1MHz -O binary ${FIRMWARE_DIR}/${KERNEL_NAME}
   COMMAND ls -l ${FIRMWARE_DIR}/${KERNEL_NAME}
   WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
   COMMENT "Convert the ELF output file to a binary image" )

include_directories(${CMAKE_CURRENT_BINARY_DIR})

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
