/* Copyright (C) 2013 by John Cronin <jncronin@tysos.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/* Provides an interface to the EMMC controller and commands for interacting
 * with an sd card */

/* References:
 *
 * PLSS  - SD Group Physical Layer Simplified Specification ver 3.00
 * HCSS  - SD Group Host Controller Simplified Specification ver 3.00
 *
 * Broadcom BCM2835 Peripherals Guide
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <assert.h>
#include "block.h"
#include "base.h"
#include "arm-start.h"
#include "gpio.h"
#include "info.h"

#include "systimer.h"

#define TIMEOUT_WAIT(stop_if_true, usec)     \
{ uint32_t time= usec;\
   do {                    \
      if (stop_if_true) \
         break;\
      RPI_WaitMicroSeconds(1);\
   } while ( time--); \
}

//#define DEBUG_SD
#define RESET_CONTROLLER

#ifdef DEBUG_SD
#define EMMC_DEBUG
#else
#define printf(...)
#endif

// Configuration options

// Enable 1.8V support
//#define SD_1_8V_SUPPORT

// Enable 4-bit support
#define SD_4BIT_DATA

// SD Clock Frequencies (in Hz)
#define SD_CLOCK_ID         400000
#define SD_CLOCK_NORMAL     25000000
#define SD_CLOCK_HIGH       50000000
#define SD_CLOCK_100        100000000
#define SD_CLOCK_208        208000000

// Enable SDXC maximum performance mode
// Requires 150 mA power so disabled on the RPi for now
//#define SDXC_MAXIMUM_PERFORMANCE

// Enable SDMA support
//#define SDMA_SUPPORT

// SDMA buffer address
#define SDMA_BUFFER     0x6000
#define SDMA_BUFFER_PA  (SDMA_BUFFER + 0xC0000000UL)

// Enable card interrupts
//#define SD_CARD_INTERRUPTS

// enable power cycling at the start ( wastes time, may be useful for Pi4 )
//#define SDCARD_PWR_CYCLE

// Enable EXPERIMENTAL (and possibly DANGEROUS) SD write support
#define SD_WRITE_SUPPORT

// Allow old sdhci versions (may cause errors)
#define EMMC_ALLOW_OLD_SDHCI

// The particular SDHCI implementation
#define SDHCI_IMPLEMENTATION_GENERIC        0
#define SDHCI_IMPLEMENTATION_BCM_2708       1
#define SDHCI_IMPLEMENTATION                SDHCI_IMPLEMENTATION_BCM_2708

struct sd_scr
{
    uint32_t    scr[2];
    uint32_t    sd_bus_widths;
    int         sd_version;
};

// Support for BE to LE conversion
#ifdef __GNUC__
#define byte_swap __builtin_bswap32
#else
static inline uint32_t byte_swap(uint32_t in)
{
   uint32_t b0 = in & 0xff;
   uint32_t b1 = (in >> 8) & 0xff;
   uint32_t b2 = (in >> 16) & 0xff;
   uint32_t b3 = (in >> 24) & 0xff;
   uint32_t ret = (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
   return ret;
}
#endif

#define SD_CMD_INDEX(a)    ((a) << 24)
#define SD_CMD_TYPE_NORMAL 0x0
#define SD_CMD_TYPE_SUSPEND   (1 << 22)
#define SD_CMD_TYPE_RESUME (2 << 22)
#define SD_CMD_TYPE_ABORT  (3 << 22)
#define SD_CMD_TYPE_MASK    (3 << 22)
#define SD_CMD_ISDATA      (1 << 21)
#define SD_CMD_IXCHK_EN    (1 << 20)
#define SD_CMD_CRCCHK_EN   (1 << 19)
#define SD_CMD_RSPNS_TYPE_NONE   0        /* For no response */
#define SD_CMD_RSPNS_TYPE_136 (1 << 16)   /* For response R2 (with CRC), R3,4 (no CRC) */
#define SD_CMD_RSPNS_TYPE_48  (2 << 16)   /* For responses R1, R5, R6, R7 (with CRC) */
#define SD_CMD_RSPNS_TYPE_48B (3 << 16)   /* For responses R1b, R5b (with CRC) */
#define SD_CMD_RSPNS_TYPE_MASK  (3 << 16)
#define SD_CMD_MULTI_BLOCK (1 << 5)
#define SD_CMD_DAT_DIR_HC  0
#define SD_CMD_DAT_DIR_CH  (1 << 4)
#define SD_CMD_AUTO_CMD_EN_NONE  0
#define SD_CMD_AUTO_CMD_EN_CMD12 (1 << 2)
#define SD_CMD_AUTO_CMD_EN_CMD23 (2 << 2)
#define SD_CMD_BLKCNT_EN      (1 << 1)
#define SD_CMD_DMA          1

#define SD_ERR_CMD_TIMEOUT 0
#define SD_ERR_CMD_CRC     1
#define SD_ERR_CMD_END_BIT 2
#define SD_ERR_CMD_INDEX   3
#define SD_ERR_DATA_TIMEOUT   4
#define SD_ERR_DATA_CRC    5
#define SD_ERR_DATA_END_BIT   6
#define SD_ERR_CURRENT_LIMIT  7
#define SD_ERR_AUTO_CMD12  8
#define SD_ERR_ADMA     9
#define SD_ERR_TUNING      10
#define SD_ERR_RSVD     11

#define SD_ERR_MASK_CMD_TIMEOUT     (1 << (16 + SD_ERR_CMD_TIMEOUT))
#define SD_ERR_MASK_CMD_CRC      (1 << (16 + SD_ERR_CMD_CRC))
#define SD_ERR_MASK_CMD_END_BIT     (1 << (16 + SD_ERR_CMD_END_BIT))
#define SD_ERR_MASK_CMD_INDEX    (1 << (16 + SD_ERR_CMD_INDEX))
#define SD_ERR_MASK_DATA_TIMEOUT (1 << (16 + SD_ERR_CMD_TIMEOUT))
#define SD_ERR_MASK_DATA_CRC     (1 << (16 + SD_ERR_CMD_CRC))
#define SD_ERR_MASK_DATA_END_BIT (1 << (16 + SD_ERR_CMD_END_BIT))
#define SD_ERR_MASK_CURRENT_LIMIT   (1 << (16 + SD_ERR_CMD_CURRENT_LIMIT))
#define SD_ERR_MASK_AUTO_CMD12      (1 << (16 + SD_ERR_CMD_AUTO_CMD12))
#define SD_ERR_MASK_ADMA      (1 << (16 + SD_ERR_CMD_ADMA))
#define SD_ERR_MASK_TUNING    (1 << (16 + SD_ERR_CMD_TUNING))

#define SD_COMMAND_COMPLETE     1
#define SD_TRANSFER_COMPLETE    (1U << 1)
#define SD_BLOCK_GAP_EVENT      (1U << 2)
#define SD_DMA_INTERRUPT        (1U << 3)
#define SD_BUFFER_WRITE_READY   (1U << 4)
#define SD_BUFFER_READ_READY    (1U << 5)
#define SD_CARD_INSERTION       (1U << 6)
#define SD_CARD_REMOVAL         (1U << 7)
#define SD_CARD_INTERRUPT       (1U << 8)

#define SD_RESP_NONE        SD_CMD_RSPNS_TYPE_NONE
#define SD_RESP_R1          (SD_CMD_RSPNS_TYPE_48 | SD_CMD_CRCCHK_EN)
#define SD_RESP_R1b         (SD_CMD_RSPNS_TYPE_48B | SD_CMD_CRCCHK_EN)
#define SD_RESP_R2          (SD_CMD_RSPNS_TYPE_136 | SD_CMD_CRCCHK_EN)
#define SD_RESP_R3          SD_CMD_RSPNS_TYPE_48
#define SD_RESP_R4          SD_CMD_RSPNS_TYPE_136
#define SD_RESP_R5          (SD_CMD_RSPNS_TYPE_48 | SD_CMD_CRCCHK_EN)
#define SD_RESP_R5b         (SD_CMD_RSPNS_TYPE_48B | SD_CMD_CRCCHK_EN)
#define SD_RESP_R6          (SD_CMD_RSPNS_TYPE_48 | SD_CMD_CRCCHK_EN)
#define SD_RESP_R7          (SD_CMD_RSPNS_TYPE_48 | SD_CMD_CRCCHK_EN)

#define SD_DATA_READ        (SD_CMD_ISDATA | SD_CMD_DAT_DIR_CH)
#define SD_DATA_WRITE       (SD_CMD_ISDATA | SD_CMD_DAT_DIR_HC)

#define SD_CMD_RESERVED(a)  0xffffffff

#define SUCCESS(a)          (a->last_cmd_success)
#define FAIL(a)             (a->last_cmd_success == 0)
#define TIMEOUT(a)          (FAIL(a) && (a->last_error == 0))
#define CMD_TIMEOUT(a)      (FAIL(a) && (a->last_error & (1 << 16)))
#define CMD_CRC(a)          (FAIL(a) && (a->last_error & (1 << 17)))
#define CMD_END_BIT(a)      (FAIL(a) && (a->last_error & (1 << 18)))
#define CMD_INDEX(a)        (FAIL(a) && (a->last_error & (1 << 19)))
#define DATA_TIMEOUT(a)     (FAIL(a) && (a->last_error & (1 << 20)))
#define DATA_CRC(a)         (FAIL(a) && (a->last_error & (1 << 21)))
#define DATA_END_BIT(a)     (FAIL(a) && (a->last_error & (1 << 22)))
#define CURRENT_LIMIT(a)    (FAIL(a) && (a->last_error & (1 << 23)))
#define ACMD12_ERROR(a)     (FAIL(a) && (a->last_error & (1 << 24)))
#define ADMA_ERROR(a)       (FAIL(a) && (a->last_error & (1 << 25)))
#define TUNING_ERROR(a)     (FAIL(a) && (a->last_error & (1 << 26)))

#define SD_VER_UNKNOWN      0
#define SD_VER_1            1
#define SD_VER_1_1          2
#define SD_VER_2            3
#define SD_VER_3            4
#define SD_VER_4            5

#define SDHOST_BASE                 (PERIPHERAL_BASE + 0x202000UL)
#define SDCMD                       0x00
#define SDARG                       0x04
#define SDTOUT                      0x08
#define SDCDIV                      0x0c
#define SDRSP0                      0x10
#define SDRSP1                      0x14
#define SDRSP2                      0x18
#define SDRSP3                      0x1c
#define SDHSTS                      0x20
#define SDVDD                       0x30
#define SDEDM                       0x34
#define SDHCFG                      0x38
#define SDHBCT                      0x3c
#define SDDATA                      0x40
#define SDHBLC                      0x50

#define SDCMD_NEW_FLAG              0x8000
#define SDCMD_FAIL_FLAG             0x4000
#define SDCMD_BUSYWAIT              0x0800
#define SDCMD_NO_RESPONSE           0x0400
#define SDCMD_LONG_RESPONSE         0x0200
#define SDCMD_WRITE_CMD             0x0080
#define SDCMD_READ_CMD              0x0040
#define SDCMD_CMD_MASK              0x003f

#define SDCDIV_MAX_CDIV             0x07ff

#define SDHSTS_BUSY_IRPT            0x0400
#define SDHSTS_BLOCK_IRPT           0x0200
#define SDHSTS_SDIO_IRPT            0x0100
#define SDHSTS_REW_TIME_OUT         0x0080
#define SDHSTS_CMD_TIME_OUT         0x0040
#define SDHSTS_CRC16_ERROR          0x0020
#define SDHSTS_CRC7_ERROR           0x0010
#define SDHSTS_FIFO_ERROR           0x0008
#define SDHSTS_DATA_FLAG            0x0001
#define SDHSTS_TRANSFER_ERROR_MASK  (SDHSTS_CRC7_ERROR | SDHSTS_CRC16_ERROR | SDHSTS_REW_TIME_OUT | SDHSTS_FIFO_ERROR)
#define SDHSTS_ERROR_MASK           (SDHSTS_CMD_TIME_OUT | SDHSTS_TRANSFER_ERROR_MASK)

#define SDHCFG_BUSY_IRPT_EN         (1 << 10)
#define SDHCFG_BLOCK_IRPT_EN        (1 << 8)
#define SDHCFG_SDIO_IRPT_EN         (1 << 5)
#define SDHCFG_DATA_IRPT_EN         (1 << 4)
#define SDHCFG_SLOW_CARD            (1 << 3)
#define SDHCFG_WIDE_EXT_BUS         (1 << 2)
#define SDHCFG_WIDE_INT_BUS         (1 << 1)
#define SDHCFG_REL_CMD_LINE         (1 << 0)

#define SDEDM_FORCE_DATA_MODE       (1 << 19)
#define SDEDM_WRITE_THRESHOLD_SHIFT 9
#define SDEDM_READ_THRESHOLD_SHIFT  14
#define SDEDM_THRESHOLD_MASK        0x1f
#define SDEDM_FSM_MASK              0x0f
#define SDEDM_FSM_IDENTMODE         0x0
#define SDEDM_FSM_DATAMODE          0x1
#define SDEDM_FSM_READDATA          0x2
#define SDEDM_FSM_WRITEDATA         0x3
#define SDEDM_FSM_READWAIT          0x4
#define SDEDM_FSM_READCRC           0x5
#define SDEDM_FSM_WRITECRC          0x6
#define SDEDM_FSM_WRITEWAIT1        0x7
#define SDEDM_FSM_POWERDOWN         0x8
#define SDEDM_FSM_WRITESTART1       0xa
#define SDEDM_FSM_WRITESTART2       0xb

#define FIFO_READ_THRESHOLD         4u
#define FIFO_WRITE_THRESHOLD        4u
#define SDDATA_FIFO_PIO_BURST       8u

#ifdef EMMC_DEBUG
static const char *sd_versions[] = { "unknown", "1.0 and 1.01", "1.10",
    "2.00", "3.0x", "4.xx" };

static const char *err_irpts[] = { "CMD_TIMEOUT", "CMD_CRC", "CMD_END_BIT", "CMD_INDEX",
   "DATA_TIMEOUT", "DATA_CRC", "DATA_END_BIT", "CURRENT_LIMIT",
   "AUTO_CMD12", "ADMA", "TUNING", "RSVD" };
#endif

size_t sd_read(struct block_device *dev, uint8_t *buf, size_t buf_size, uint32_t block_no);
size_t sd_write(struct block_device *dev, const uint8_t *buf, size_t buf_size, uint32_t block_no);

static uint32_t sdhost_read(uint32_t reg);
static void sdhost_write(uint32_t reg, uint32_t value);
static void sdhost_prepare_storage_pins(void);
static void sdhost_reset_internal(void);
static int sdhost_reset_controller(void);
static uint32_t sdhost_get_base_clock_hz(void);
static int sdhost_switch_clock_rate(uint32_t target_rate);
static int sd_reset_cmd_sdhost(void);
static int sd_reset_dat_sdhost(void);
static uint32_t sdhost_translate_error(uint32_t status, bool is_data);
static int sdhost_wait_for_data_idle(bool is_read);
static int sdhost_transfer_pio(struct emmc_block_dev *dev, bool is_write);
static int sdhost_issue_raw_command(uint32_t sdcmd, uint32_t argument, uint32_t timeout, uint32_t *response0, bool has_data, uint32_t *error_out);
static int sdhost_issue_stop_command(uint32_t timeout, uint32_t *error_out);
static int sdhost_prepare_storage_controller(void);
static int sdhost_set_firmware_clock(uint32_t target_rate, uint32_t *actual_rate);
static void sdhost_probe_firmware_clock_mode(void);
static int sdhost_wait_for_request_ready(uint32_t timeout);

#ifdef DEBUG_SD
static void sdhost_log_failure(const char *phase, uint32_t opcode, uint32_t argument, struct emmc_block_dev *dev)
{
    printf(
        "SDHOST %s: cmd=%" PRIu32 " arg=%08" PRIx32 " err=%08" PRIx32
        " hsts=%08" PRIx32 " edm=%08" PRIx32 " hcfg=%08" PRIx32
        " cdiv=%08" PRIx32 " blocks=%" PRIu32 " blksz=%zu\r\n",
        phase,
        opcode,
        argument,
        dev->last_error,
        sdhost_read(SDHSTS),
        sdhost_read(SDEDM),
        sdhost_read(SDHCFG),
        sdhost_read(SDCDIV),
        dev->blocks_to_transfer,
        dev->block_size);
}
#else
static void sdhost_log_failure(const char *phase, uint32_t opcode, uint32_t argument, struct emmc_block_dev *dev)
{
    (void)phase;
    (void)opcode;
    (void)argument;
    (void)dev;
}
#endif

static uint32_t g_sdhost_storage_hcfg = SDHCFG_BUSY_IRPT_EN;
static uint32_t g_sdhost_storage_cdiv = SDCDIV_MAX_CDIV;
static bool g_sdhost_firmware_sets_cdiv;

static const uint32_t sd_commands[] = {
    SD_CMD_INDEX(0),
    SD_CMD_RESERVED(1),
    SD_CMD_INDEX(2) | SD_RESP_R2,
    SD_CMD_INDEX(3) | SD_RESP_R6,
    SD_CMD_INDEX(4),
    SD_CMD_INDEX(5) | SD_RESP_R4,
    SD_CMD_INDEX(6) | SD_RESP_R1,
    SD_CMD_INDEX(7) | SD_RESP_R1b,
    SD_CMD_INDEX(8) | SD_RESP_R7,
    SD_CMD_INDEX(9) | SD_RESP_R2,
    SD_CMD_INDEX(10) | SD_RESP_R2,
    SD_CMD_INDEX(11) | SD_RESP_R1,
    SD_CMD_INDEX(12) | SD_RESP_R1b | SD_CMD_TYPE_ABORT,
    SD_CMD_INDEX(13) | SD_RESP_R1,
    SD_CMD_RESERVED(14),
    SD_CMD_INDEX(15),
    SD_CMD_INDEX(16) | SD_RESP_R1,
    SD_CMD_INDEX(17) | SD_RESP_R1 | SD_DATA_READ,
    SD_CMD_INDEX(18) | SD_RESP_R1 | SD_DATA_READ | SD_CMD_MULTI_BLOCK | SD_CMD_BLKCNT_EN | SD_CMD_AUTO_CMD_EN_CMD12,
    SD_CMD_INDEX(19) | SD_RESP_R1 | SD_DATA_READ,
    SD_CMD_INDEX(20) | SD_RESP_R1b,
    SD_CMD_RESERVED(21),
    SD_CMD_RESERVED(22),
    SD_CMD_INDEX(23) | SD_RESP_R1,
    SD_CMD_INDEX(24) | SD_RESP_R1 | SD_DATA_WRITE,
    SD_CMD_INDEX(25) | SD_RESP_R1 | SD_DATA_WRITE | SD_CMD_MULTI_BLOCK | SD_CMD_BLKCNT_EN | SD_CMD_AUTO_CMD_EN_CMD12,
    SD_CMD_RESERVED(26),
    SD_CMD_INDEX(27) | SD_RESP_R1 | SD_DATA_WRITE,
    SD_CMD_INDEX(28) | SD_RESP_R1b,
    SD_CMD_INDEX(29) | SD_RESP_R1b,
    SD_CMD_INDEX(30) | SD_RESP_R1 | SD_DATA_READ,
    SD_CMD_RESERVED(31),
    SD_CMD_INDEX(32) | SD_RESP_R1,
    SD_CMD_INDEX(33) | SD_RESP_R1,
    SD_CMD_RESERVED(34),
    SD_CMD_RESERVED(35),
    SD_CMD_RESERVED(36),
    SD_CMD_RESERVED(37),
    SD_CMD_INDEX(38) | SD_RESP_R1b,
    SD_CMD_RESERVED(39),
    SD_CMD_RESERVED(40),
    SD_CMD_RESERVED(41),
    SD_CMD_RESERVED(42) | SD_RESP_R1,
    SD_CMD_RESERVED(43),
    SD_CMD_RESERVED(44),
    SD_CMD_RESERVED(45),
    SD_CMD_RESERVED(46),
    SD_CMD_RESERVED(47),
    SD_CMD_RESERVED(48),
    SD_CMD_RESERVED(49),
    SD_CMD_RESERVED(50),
    SD_CMD_RESERVED(51),
    SD_CMD_RESERVED(52),
    SD_CMD_RESERVED(53),
    SD_CMD_RESERVED(54),
    SD_CMD_INDEX(55) | SD_RESP_R1,
    SD_CMD_INDEX(56) | SD_RESP_R1 | SD_CMD_ISDATA,
    SD_CMD_RESERVED(57),
    SD_CMD_RESERVED(58),
    SD_CMD_RESERVED(59),
    SD_CMD_RESERVED(60),
    SD_CMD_RESERVED(61),
    SD_CMD_RESERVED(62),
    SD_CMD_RESERVED(63)
};

static const uint32_t sd_acommands[] = {
    SD_CMD_RESERVED(0),
    SD_CMD_RESERVED(1),
    SD_CMD_RESERVED(2),
    SD_CMD_RESERVED(3),
    SD_CMD_RESERVED(4),
    SD_CMD_RESERVED(5),
    SD_CMD_INDEX(6) | SD_RESP_R1,
    SD_CMD_RESERVED(7),
    SD_CMD_RESERVED(8),
    SD_CMD_RESERVED(9),
    SD_CMD_RESERVED(10),
    SD_CMD_RESERVED(11),
    SD_CMD_RESERVED(12),
    SD_CMD_INDEX(13) | SD_RESP_R1,
    SD_CMD_RESERVED(14),
    SD_CMD_RESERVED(15),
    SD_CMD_RESERVED(16),
    SD_CMD_RESERVED(17),
    SD_CMD_RESERVED(18),
    SD_CMD_RESERVED(19),
    SD_CMD_RESERVED(20),
    SD_CMD_RESERVED(21),
    SD_CMD_INDEX(22) | SD_RESP_R1 | SD_DATA_READ,
    SD_CMD_INDEX(23) | SD_RESP_R1,
    SD_CMD_RESERVED(24),
    SD_CMD_RESERVED(25),
    SD_CMD_RESERVED(26),
    SD_CMD_RESERVED(27),
    SD_CMD_RESERVED(28),
    SD_CMD_RESERVED(29),
    SD_CMD_RESERVED(30),
    SD_CMD_RESERVED(31),
    SD_CMD_RESERVED(32),
    SD_CMD_RESERVED(33),
    SD_CMD_RESERVED(34),
    SD_CMD_RESERVED(35),
    SD_CMD_RESERVED(36),
    SD_CMD_RESERVED(37),
    SD_CMD_RESERVED(38),
    SD_CMD_RESERVED(39),
    SD_CMD_RESERVED(40),
    SD_CMD_INDEX(41) | SD_RESP_R3,
    SD_CMD_INDEX(42) | SD_RESP_R1,
    SD_CMD_RESERVED(43),
    SD_CMD_RESERVED(44),
    SD_CMD_RESERVED(45),
    SD_CMD_RESERVED(46),
    SD_CMD_RESERVED(47),
    SD_CMD_RESERVED(48),
    SD_CMD_RESERVED(49),
    SD_CMD_RESERVED(50),
    SD_CMD_INDEX(51) | SD_RESP_R1 | SD_DATA_READ,
    SD_CMD_RESERVED(52),
    SD_CMD_RESERVED(53),
    SD_CMD_RESERVED(54),
    SD_CMD_RESERVED(55),
    SD_CMD_RESERVED(56),
    SD_CMD_RESERVED(57),
    SD_CMD_RESERVED(58),
    SD_CMD_RESERVED(59),
    SD_CMD_RESERVED(60),
    SD_CMD_RESERVED(61),
    SD_CMD_RESERVED(62),
    SD_CMD_RESERVED(63)
};

// The actual command indices
#define GO_IDLE_STATE           0
#define ALL_SEND_CID            2
#define SEND_RELATIVE_ADDR      3
#define SET_DSR                 4
#define IO_SET_OP_COND          5
#define SWITCH_FUNC             6
#define SELECT_CARD             7
#define DESELECT_CARD           7
#define SELECT_DESELECT_CARD    7
#define SEND_IF_COND            8
#define SEND_CSD                9
#define SEND_CID                10
#define VOLTAGE_SWITCH          11
#define STOP_TRANSMISSION       12
#define SEND_STATUS             13
#define GO_INACTIVE_STATE       15
#define SET_BLOCKLEN            16
#define READ_SINGLE_BLOCK       17
#define READ_MULTIPLE_BLOCK     18
#define SEND_TUNING_BLOCK       19
#define SPEED_CLASS_CONTROL     20
#define SET_BLOCK_COUNT         23
#define WRITE_BLOCK             24
#define WRITE_MULTIPLE_BLOCK    25
#define PROGRAM_CSD             27
#define SET_WRITE_PROT          28
#define CLR_WRITE_PROT          29
#define SEND_WRITE_PROT         30
#define ERASE_WR_BLK_START      32
#define ERASE_WR_BLK_END        33
#define ERASE                   38
#define LOCK_UNLOCK             42
#define APP_CMD                 55
#define GEN_CMD                 56

#define IS_APP_CMD              0x80000000
#define ACMD(a)                 ((a) | IS_APP_CMD)
#define SET_BUS_WIDTH           (6 | IS_APP_CMD)
#define SD_STATUS               (13 | IS_APP_CMD)
#define SEND_NUM_WR_BLOCKS      (22 | IS_APP_CMD)
#define SET_WR_BLK_ERASE_COUNT  (23 | IS_APP_CMD)
#define SD_SEND_OP_COND         (41 | IS_APP_CMD)
#define SET_CLR_CARD_DETECT     (42 | IS_APP_CMD)
#define SEND_SCR                (51 | IS_APP_CMD)

#define SD_RESET_ALL            (1 << 24)

#if SDHCI_IMPLEMENTATION == SDHCI_IMPLEMENTATION_BCM_2708
#ifdef SDCARD_PWR_CYCLE
static int bcm_2708_power_off()
{
   RPI_PropertySetWord(TAG_SET_POWER_STATE, 0, 2);
   return 0;
}

static int bcm_2708_power_on()
{
   RPI_PropertySetWord(TAG_SET_POWER_STATE, 0, 2);
   return 0;
}

static int bcm_2708_power_cycle()
{
   bcm_2708_power_off();
   usleep(5000);
   return bcm_2708_power_on();
}
#endif
#endif

static void sd_issue_command_int(struct emmc_block_dev *dev, uint32_t cmd_reg, uint32_t argument, uint32_t timeout)
{
    uint32_t opcode = (cmd_reg >> 24) & 0x3fu;
    uint32_t sdcmd = opcode & SDCMD_CMD_MASK;
    bool has_data = (cmd_reg & SD_CMD_ISDATA) != 0u;
    bool is_write = has_data && (cmd_reg & SD_CMD_DAT_DIR_CH) == 0u;

    dev->last_cmd_success = 0;
    dev->last_interrupt = 0;
    dev->last_error = 0;

    if ((cmd_reg & SD_CMD_RSPNS_TYPE_MASK) == SD_CMD_RSPNS_TYPE_NONE)
        sdcmd |= SDCMD_NO_RESPONSE;
    else if ((cmd_reg & SD_CMD_RSPNS_TYPE_MASK) == SD_CMD_RSPNS_TYPE_136)
        sdcmd |= SDCMD_LONG_RESPONSE;
    else if ((cmd_reg & SD_CMD_RSPNS_TYPE_MASK) == SD_CMD_RSPNS_TYPE_48B)
        sdcmd |= SDCMD_BUSYWAIT;

    if (has_data)
    {
        sdcmd |= is_write ? SDCMD_WRITE_CMD : SDCMD_READ_CMD;
        sdhost_write(SDHBCT, (uint32_t) dev->block_size);
        sdhost_write(SDHBLC, dev->blocks_to_transfer);
    }
    else
    {
        sdhost_write(SDHBCT, 0);
        sdhost_write(SDHBLC, 0);
    }

    if (sdhost_issue_raw_command(sdcmd, argument, timeout, &dev->last_r0, has_data, &dev->last_error) != 0)
    {
        dev->last_interrupt = sdhost_read(SDHSTS);
        sdhost_log_failure("cmd", opcode, argument, dev);
        return;
    }

    if (has_data)
    {
        if (sdhost_transfer_pio(dev, is_write) != 0)
        {
            dev->last_interrupt = sdhost_read(SDHSTS);
            dev->last_error = sdhost_translate_error(dev->last_interrupt, true);
            sdhost_log_failure("pio", opcode, argument, dev);
            sdhost_write(SDHSTS, dev->last_interrupt & (SDHSTS_ERROR_MASK | SDHSTS_BUSY_IRPT | SDHSTS_BLOCK_IRPT | SDHSTS_SDIO_IRPT | SDHSTS_DATA_FLAG));
            return;
        }

        if ((opcode == READ_MULTIPLE_BLOCK || opcode == WRITE_MULTIPLE_BLOCK) &&
            sdhost_wait_for_data_idle(!is_write) != 0)
        {
            dev->last_interrupt = sdhost_read(SDHSTS);
            dev->last_error = SD_ERR_MASK_DATA_TIMEOUT;
            sdhost_log_failure("pre-stop", opcode, argument, dev);
            return;
        }

        if ((opcode == READ_MULTIPLE_BLOCK || opcode == WRITE_MULTIPLE_BLOCK) &&
            sdhost_issue_stop_command(timeout, &dev->last_error) != 0)
        {
            dev->last_interrupt = sdhost_read(SDHSTS);
            sdhost_log_failure("stop", opcode, argument, dev);
            return;
        }

        if (sdhost_wait_for_data_idle(!is_write) != 0)
        {
            dev->last_interrupt = sdhost_read(SDHSTS);
            dev->last_error = SD_ERR_MASK_DATA_TIMEOUT;
            sdhost_log_failure("data-idle", opcode, argument, dev);
            return;
        }
    }
    else if ((cmd_reg & SD_CMD_RSPNS_TYPE_MASK) == SD_CMD_RSPNS_TYPE_48B)
    {
        if (sdhost_wait_for_data_idle(false) != 0)
        {
            dev->last_interrupt = sdhost_read(SDHSTS);
            dev->last_error = SD_ERR_MASK_CMD_TIMEOUT;
            sdhost_log_failure("busy-idle", opcode, argument, dev);
            return;
        }
    }

    dev->last_cmd_success = 1;
    dev->last_interrupt = sdhost_read(SDHSTS);
}

static void sd_handle_interrupts(struct emmc_block_dev *dev)
{
    (void)dev;
    _data_memory_barrier();
}

static void sd_issue_command(struct emmc_block_dev *dev, uint32_t command, uint32_t argument, uint32_t timeout)
{
    // First, handle any pending interrupts
    sd_handle_interrupts(dev);

    // Stop the command issue if it was the card remove interrupt that was
    //  handled
    if(dev->card_removal)
    {
        dev->last_cmd_success = 0;
        return;
    }

    // Now run the appropriate commands by calling sd_issue_command_int()
    if(command & IS_APP_CMD)
    {
        command &= 0xff;
#ifdef EMMC_DEBUG
        printf("SD: issuing command ACMD%"PRIu32"\r\n", command);
#endif

        if(sd_acommands[command] == SD_CMD_RESERVED(0))
        {
            printf("SD: invalid command ACMD%"PRIu32"\r\n", command);
            dev->last_cmd_success = 0;
            return;
        }
        uint32_t rca = 0;
        if(dev->card_rca)
            rca = dev->card_rca << 16;
        sd_issue_command_int(dev, sd_commands[APP_CMD], rca, timeout);
        if(dev->last_cmd_success)
        {
            sd_issue_command_int(dev, sd_acommands[command], argument, timeout);
        }
    }
    else
    {
#ifdef EMMC_DEBUG
        printf("SD: issuing command CMD%"PRIu32"\r\n", command);
#endif

        if(sd_commands[command] == SD_CMD_RESERVED(0))
        {
            printf("SD: invalid command CMD%"PRIu32"\r\n", command);
            dev->last_cmd_success = 0;
            return;
        }

        sd_issue_command_int(dev, sd_commands[command], argument, timeout);
    }

#ifdef EMMC_DEBUG
    if(FAIL(dev))
    {
        printf("SD: error issuing command: interrupts %08"PRIx32": ", dev->last_interrupt);
        if(dev->last_error == 0)
            printf("TIMEOUT");
        else
        {
            for(int i = 0; i < SD_ERR_RSVD; i++)
            {
                if(dev->last_error & (1 << (i + 16)))
                {
                    printf("%s", err_irpts[i]);
                    printf(" ");
                }
            }
        }
      printf("\r\n");
    }
    else
        printf("SD: command completed successfully\r\n");
#endif
}

static uint32_t sdhost_read(uint32_t reg)
{
    return *(volatile uint32_t *)(uintptr_t)(SDHOST_BASE + reg);
}

static void sdhost_write(uint32_t reg, uint32_t value)
{
    *(volatile uint32_t *)(uintptr_t)(SDHOST_BASE + reg) = value;
}

static void sdhost_prepare_storage_pins(void)
{
    unsigned int index;

    for (index = 0u; index < 6u; ++index)
    {
        rpi_gpio_pin_t pin = (rpi_gpio_pin_t)(RPI_GPIO48 + index);

        RPI_SetGpioPinFunction(pin, FS_ALT0);
        RPI_SetGpioPull(pin, index == 0u ? PULL_NONE : PULL_UP);
    }
}

static void sdhost_reset_internal(void)
{
    uint32_t temp;

    sdhost_write(SDVDD, 0u);
    sdhost_write(SDCMD, 0u);
    sdhost_write(SDARG, 0u);
    sdhost_write(SDTOUT, 0x00f00000u);
    sdhost_write(SDCDIV, SDCDIV_MAX_CDIV);
    sdhost_write(SDHSTS, SDHSTS_BUSY_IRPT | SDHSTS_BLOCK_IRPT | SDHSTS_SDIO_IRPT | SDHSTS_DATA_FLAG | SDHSTS_ERROR_MASK);
    sdhost_write(SDHCFG, 0u);
    sdhost_write(SDHBCT, 0u);
    sdhost_write(SDHBLC, 0u);

    temp = sdhost_read(SDEDM);
    temp &= (uint32_t)~((uint32_t)(SDEDM_THRESHOLD_MASK << SDEDM_READ_THRESHOLD_SHIFT) |
                        (uint32_t)(SDEDM_THRESHOLD_MASK << SDEDM_WRITE_THRESHOLD_SHIFT));
    temp |= (FIFO_READ_THRESHOLD << SDEDM_READ_THRESHOLD_SHIFT) |
            (FIFO_WRITE_THRESHOLD << SDEDM_WRITE_THRESHOLD_SHIFT);
    sdhost_write(SDEDM, temp);

    usleep(10000);
    sdhost_write(SDVDD, 1u);
    usleep(10000);
}

static int sdhost_reset_controller(void)
{
    g_sdhost_storage_hcfg = SDHCFG_BUSY_IRPT_EN | SDHCFG_WIDE_INT_BUS | SDHCFG_SLOW_CARD;
    g_sdhost_storage_cdiv = SDCDIV_MAX_CDIV;
    sdhost_probe_firmware_clock_mode();
    sdhost_reset_internal();
    sdhost_write(SDHCFG, g_sdhost_storage_hcfg);
    return 0;
}

static int sdhost_set_firmware_clock(uint32_t target_rate, uint32_t *actual_rate)
{
    rpi_mailbox_property_t *response;

    RPI_PropertyStart(TAG_SET_SDHOST_CLOCK, 3);
    RPI_PropertyAdd(target_rate);
    RPI_PropertyAdd(0u);
    RPI_PropertyAdd(0u);
    RPI_PropertyProcess(true);

    response = RPI_PropertyGet(TAG_SET_SDHOST_CLOCK);
    if (response == NULL)
        return -1;

    if (actual_rate != NULL)
    {
        uint32_t first = response->data.buffer_32[1];
        uint32_t second = response->data.buffer_32[2];
        *actual_rate = first > second ? first : second;
    }

    return 0;
}

static void sdhost_probe_firmware_clock_mode(void)
{
    rpi_mailbox_property_t *response;

    RPI_PropertyStart(TAG_SET_SDHOST_CLOCK, 3);
    RPI_PropertyAdd(0u);
    RPI_PropertyAdd(~0u);
    RPI_PropertyAdd(~0u);
    RPI_PropertyProcess(true);

    response = RPI_PropertyGet(TAG_SET_SDHOST_CLOCK);
    if (response == NULL)
    {
        g_sdhost_firmware_sets_cdiv = false;
        return;
    }

    g_sdhost_firmware_sets_cdiv = response->data.buffer_32[1] != ~0u;
}

static uint32_t sdhost_get_base_clock_hz(void)
{
    uint32_t base_clock = get_clock_rate(CORE_CLK_ID);

    if (base_clock == 0u)
        base_clock = 250000000u;

    return base_clock;
}

static int sdhost_switch_clock_rate(uint32_t target_rate)
{
    uint32_t base_clock;
    uint32_t input_clock;
    uint32_t divider;
    uint32_t actual_clock;

    if (target_rate == 0u)
        return -1;

    base_clock = sdhost_get_base_clock_hz();
    input_clock = target_rate;

    if (g_sdhost_firmware_sets_cdiv)
    {
        if (sdhost_set_firmware_clock(target_rate, &actual_clock) != 0)
            return -1;
        if (actual_clock == 0u)
            actual_clock = target_rate;
        sdhost_write(SDTOUT, actual_clock / 2u);
        sdhost_write(SDHCFG, g_sdhost_storage_hcfg);
        usleep(10);
        return 0;
    }

    if (input_clock < 100000u)
    {
        divider = SDCDIV_MAX_CDIV;
    }
    else
    {
        divider = base_clock / input_clock;
        if (divider < 2u)
            divider = 2u;
        if ((base_clock / divider) > input_clock)
            ++divider;
        divider -= 2u;
    }

    if (divider > SDCDIV_MAX_CDIV)
        divider = SDCDIV_MAX_CDIV;

    g_sdhost_storage_cdiv = divider;
    g_sdhost_storage_hcfg |= SDHCFG_SLOW_CARD;

    sdhost_write(SDTOUT, input_clock / 2u);
    sdhost_write(SDCDIV, g_sdhost_storage_cdiv);
    sdhost_write(SDHCFG, g_sdhost_storage_hcfg);
    usleep(10);
    return 0;
}

static int sd_reset_cmd_sdhost(void)
{
    sdhost_write(SDHCFG, g_sdhost_storage_hcfg | SDHCFG_REL_CMD_LINE);
    usleep(10);
    sdhost_write(SDHCFG, g_sdhost_storage_hcfg);
    return 0;
}

static int sd_reset_dat_sdhost(void)
{
    return sdhost_reset_controller();
}

static uint32_t sdhost_translate_error(uint32_t status, bool is_data)
{
    uint32_t error = 0u;

    (void)is_data;

    if (status & SDHSTS_CMD_TIME_OUT)
        error |= SD_ERR_MASK_CMD_TIMEOUT;
    if (status & SDHSTS_CRC7_ERROR)
        error |= SD_ERR_MASK_CMD_CRC;
    if (status & SDHSTS_CRC16_ERROR)
        error |= SD_ERR_MASK_DATA_CRC;
    if (status & SDHSTS_FIFO_ERROR)
        error |= SD_ERR_MASK_CMD_END_BIT;
    if ((error == 0u) && (status & SDHSTS_REW_TIME_OUT))
        error |= SD_ERR_MASK_DATA_TIMEOUT;

    return error;
}

static int sdhost_wait_for_data_idle(bool is_read)
{
    uint32_t edm = 0u;
    uint32_t alternate_idle = is_read ? SDEDM_FSM_READWAIT : SDEDM_FSM_WRITESTART1;
    uint32_t retries = 1000000u;

    while (retries-- > 0u)
    {
        uint32_t fsm;

        edm = sdhost_read(SDEDM);
        fsm = edm & SDEDM_FSM_MASK;
        if ((fsm == SDEDM_FSM_IDENTMODE) || (fsm == SDEDM_FSM_DATAMODE))
            return 0;
        if (fsm == alternate_idle)
        {
            sdhost_write(SDEDM, edm | SDEDM_FORCE_DATA_MODE);
            return 0;
        }
        usleep(1);
    }

    return -1;
}

static int sdhost_transfer_pio(struct emmc_block_dev *dev, bool is_write)
{
    uint32_t *cursor = (uint32_t *)dev->buf;
    uint32_t total_words = (uint32_t)((dev->block_size * dev->blocks_to_transfer) / sizeof(uint32_t));

    while (total_words > 0u)
    {
        uint32_t burst_words = SDDATA_FIFO_PIO_BURST;
        uint32_t wait_loops = 500000u;

        if (burst_words > total_words)
            burst_words = total_words;

        while (burst_words > 0u)
        {
            uint32_t edm = sdhost_read(SDEDM);
            uint32_t words = is_write ? (16u - ((edm >> 4) & 0x1fu)) : ((edm >> 4) & 0x1fu);
            uint32_t hsts;
            uint32_t fsm_state;

            if (words == 0u)
            {
                if (wait_loops-- == 0u)
                {
                    dev->last_error = SD_ERR_MASK_DATA_TIMEOUT;
                    return -1;
                }

                fsm_state = edm & SDEDM_FSM_MASK;
                if (is_write)
                {
                    if ((fsm_state != SDEDM_FSM_WRITEDATA) &&
                        (fsm_state != SDEDM_FSM_WRITESTART1) &&
                        (fsm_state != SDEDM_FSM_WRITESTART2))
                    {
                        hsts = sdhost_read(SDHSTS);
                        if ((hsts & SDHSTS_ERROR_MASK) != 0u)
                        {
                            dev->last_error = sdhost_translate_error(hsts, true);
                            return -1;
                        }
                    }
                }
                else
                {
                    if ((fsm_state != SDEDM_FSM_READDATA) &&
                        (fsm_state != SDEDM_FSM_READWAIT) &&
                        (fsm_state != SDEDM_FSM_READCRC))
                    {
                        hsts = sdhost_read(SDHSTS);
                        if ((hsts & SDHSTS_ERROR_MASK) != 0u)
                        {
                            dev->last_error = sdhost_translate_error(hsts, true);
                            return -1;
                        }
                    }
                }

                usleep(1);
                continue;
            }

            if (words > burst_words)
                words = burst_words;

            burst_words -= words;
            total_words -= words;

            while (words-- > 0u)
            {
                if (is_write)
                    sdhost_write(SDDATA, *cursor);
                else
                    *cursor = sdhost_read(SDDATA);
                ++cursor;
            }
        }
    }

    if ((sdhost_read(SDHSTS) & SDHSTS_ERROR_MASK) != 0u)
    {
        dev->last_error = sdhost_translate_error(sdhost_read(SDHSTS), true);
        return -1;
    }

    return 0;
}

static int sdhost_wait_for_request_ready(uint32_t timeout)
{
    uint32_t wait_loops = timeout;

    while (wait_loops-- > 0u)
    {
        uint32_t edm = sdhost_read(SDEDM);
        uint32_t fsm = edm & SDEDM_FSM_MASK;

        if ((fsm == SDEDM_FSM_IDENTMODE) || (fsm == SDEDM_FSM_DATAMODE))
            return 0;

        usleep(1);
    }

    return -1;
}

static int sdhost_issue_raw_command(uint32_t sdcmd, uint32_t argument, uint32_t timeout, uint32_t *response0, bool has_data, uint32_t *error_out)
{
    uint32_t status;
    uint32_t wait_loops = timeout;
    uint32_t hcfg = g_sdhost_storage_hcfg;

    status = sdhost_read(SDHSTS);
    if ((status & (SDHSTS_ERROR_MASK | SDHSTS_BUSY_IRPT | SDHSTS_BLOCK_IRPT | SDHSTS_SDIO_IRPT | SDHSTS_DATA_FLAG)) != 0u)
    {
        sdhost_write(SDHSTS, status & (SDHSTS_ERROR_MASK | SDHSTS_BUSY_IRPT | SDHSTS_BLOCK_IRPT | SDHSTS_SDIO_IRPT | SDHSTS_DATA_FLAG));
    }

    if (has_data)
        hcfg |= SDHCFG_DATA_IRPT_EN | SDHCFG_BLOCK_IRPT_EN;
    else
        hcfg &= (uint32_t)~(SDHCFG_DATA_IRPT_EN | SDHCFG_BLOCK_IRPT_EN);
    sdhost_write(SDHCFG, hcfg);

    if (sdhost_wait_for_request_ready(timeout) != 0)
    {
        if (error_out != NULL)
            *error_out = SD_ERR_MASK_CMD_TIMEOUT;
        return -1;
    }

    while ((sdhost_read(SDCMD) & SDCMD_NEW_FLAG) != 0u)
    {
        if (wait_loops-- == 0u)
        {
            if (error_out != NULL)
                *error_out = SD_ERR_MASK_CMD_TIMEOUT;
            return -1;
        }
        usleep(1);
    }

    sdhost_write(SDARG, argument);
    sdhost_write(SDCMD, sdcmd | SDCMD_NEW_FLAG);

    wait_loops = timeout;
    while ((sdhost_read(SDCMD) & SDCMD_NEW_FLAG) != 0u)
    {
        if (wait_loops-- == 0u)
        {
            if (error_out != NULL)
                *error_out = SD_ERR_MASK_CMD_TIMEOUT;
            return -1;
        }
        usleep(1);
    }

    status = sdhost_read(SDHSTS);
    if (((sdhost_read(SDCMD) & SDCMD_FAIL_FLAG) != 0u) || ((status & SDHSTS_ERROR_MASK) != 0u))
    {
        if (error_out != NULL)
            *error_out = sdhost_translate_error(status, false);
        sdhost_write(SDHSTS, status & (SDHSTS_ERROR_MASK | SDHSTS_BUSY_IRPT | SDHSTS_BLOCK_IRPT | SDHSTS_SDIO_IRPT | SDHSTS_DATA_FLAG));
        return -1;
    }

    if (response0 != NULL)
        *response0 = sdhost_read(SDRSP0);
    if (error_out != NULL)
        *error_out = 0u;

    return 0;
}

static int sdhost_issue_stop_command(uint32_t timeout, uint32_t *error_out)
{
    uint32_t response0;
    uint32_t sdcmd = (uint32_t)STOP_TRANSMISSION & SDCMD_CMD_MASK;

    sdcmd |= SDCMD_BUSYWAIT;
    return sdhost_issue_raw_command(sdcmd, 0u, timeout, &response0, false, error_out);
}

static int sdhost_prepare_storage_controller(void)
{
    sdhost_prepare_storage_pins();
    if (sdhost_reset_controller() != 0)
        return -1;
    return sdhost_switch_clock_rate(SD_CLOCK_ID);
}

static void sdhost_prepare_device_state(struct emmc_block_dev *dev)
{
    memset(dev, 0, sizeof(*dev));
    dev->bd.read = sd_read;
#ifdef SD_WRITE_SUPPORT
    dev->bd.write = sd_write;
#endif
    dev->block_size = 512;
    dev->blocks_to_transfer = 1;
}

static int sd_prepare_controller(struct emmc_block_dev *dev)
{
    (void) dev;
    return sdhost_prepare_storage_controller();
}

static uint32_t sd_get_storage_base_clock_hz(const struct emmc_block_dev *dev)
{
    (void) dev;
    return sdhost_get_base_clock_hz();
}

static int sd_set_storage_clock_rate(const struct emmc_block_dev *dev, uint32_t base_clock, uint32_t target_rate)
{
    (void) dev;
    (void) base_clock;
    return sdhost_switch_clock_rate(target_rate);
}

static int sd_reset_cmd_for_dev(const struct emmc_block_dev *dev)
{
    (void) dev;
    return sd_reset_cmd_sdhost();
}

static int sd_reset_dat_for_dev(const struct emmc_block_dev *dev)
{
    (void) dev;
    return sd_reset_dat_sdhost();
}

static int sd_card_init(struct block_device **dev)
{
    uint32_t base_clock;

    // Prepare the device structure
   struct emmc_block_dev *ret;
   if(*dev == NULL)
      ret = (struct emmc_block_dev *)malloc(sizeof(struct emmc_block_dev));
   else
      ret = (struct emmc_block_dev *)*dev;

   assert(ret);

    sdhost_prepare_device_state(ret);

    if (sd_prepare_controller(ret) != 0)
        return -1;

    base_clock = sd_get_storage_base_clock_hz(ret);

#ifdef EMMC_DEBUG
   printf("EMMC: device structure created\r\n");
#endif
#ifdef RESET_CONTROLLER
   // Send CMD0 to the card (reset to idle state)
   sd_issue_command(ret, GO_IDLE_STATE, 0, 500000);
   if(FAIL(ret))
   {
        printf("SD: no CMD0 response\r\n");
        return -1;
   }

   // Send CMD8 to the card
   // Voltage supplied = 0x1 = 2.7-3.6V (standard)
   // Check pattern = 10101010b (as per PLSS 4.3.13) = 0xAA
#ifdef EMMC_DEBUG
    printf("SD: note a timeout error on the following command (CMD8) is normal "
           "and expected if the SD card version is less than 2.0\r\n");
#endif
   sd_issue_command(ret, SEND_IF_COND, 0x1aa, 500000);
   int v2_later = 0;
   if(TIMEOUT(ret))
        v2_later = 0;
    else if(CMD_TIMEOUT(ret))
    {
        if(sd_reset_cmd_for_dev(ret) == -1)
            return -1;
        v2_later = 0;
    }
    else if(FAIL(ret))
    {
        printf("SD: failure sending CMD8 (%08"PRIx32")\r\n", ret->last_interrupt);
        return -1;
    }
    else
    {
        if((ret->last_r0 & 0xfff) != 0x1aa)
        {
            printf("SD: unusable card\n");
#ifdef EMMC_DEBUG
            printf("SD: CMD8 response %08"PRIx32"\r\n", ret->last_r0);
#endif
            return -1;
        }
        else
            v2_later = 1;
    }
#if 0
    // Here we are supposed to check the response to CMD5 (HCSS 3.6)
    // It only returns if the card is a SDIO card
#ifdef EMMC_DEBUG
    printf("SD: note that a timeout error on the following command (CMD5) is "
           "normal and expected if the card is not a SDIO card.\r\n");
#endif
    sd_issue_command(ret, IO_SET_OP_COND, 0, 10000);
    if(!TIMEOUT(ret))
    {
        if(CMD_TIMEOUT(ret))
        {
            if(sd_reset_cmd_for_dev(ret) == -1)
                return -1;
        }
        else
        {
            printf("SD: SDIO card detected - not currently supported\r\n");
#ifdef EMMC_DEBUG
            printf("SD: CMD5 returned %08"PRIx32"\r\n", ret->last_r0);
#endif
            return -1;
        }
    }
#endif
    // Call an inquiry ACMD41 (voltage window = 0) to get the OCR
#ifdef EMMC_DEBUG
    printf("SD: sending inquiry ACMD41\r\n");
#endif
    sd_issue_command(ret, ACMD(41), 0, 500000);
    if(FAIL(ret))
    {
        printf("SD: inquiry ACMD41 failed\r\n");
        return -1;
    }
#ifdef EMMC_DEBUG
    printf("SD: inquiry ACMD41 returned %08"PRIx32"\r\n", ret->last_r0);
#endif

   // Call initialization ACMD41
   int card_is_busy = 1;
   while(card_is_busy)
   {
       uint32_t v2_flags = 0;
       if(v2_later)
       {
           // Set SDHC support
           v2_flags |= (1 << 30);

           // Set 1.8v support
#ifdef SD_1_8V_SUPPORT
           if(!ret->failed_voltage_switch)
                v2_flags |= (1 << 24);
#endif

            // Enable SDXC maximum performance
#ifdef SDXC_MAXIMUM_PERFORMANCE
            v2_flags |= (1 << 28);
#endif
       }

       sd_issue_command(ret, ACMD(41), 0x00ff8000 | v2_flags, 500000);

       if(FAIL(ret))
       {
           printf("SD: error issuing ACMD41\r\n");
           return -1;
       }

       if((ret->last_r0 >> 31) & 0x1)
       {
           // Initialization is complete
           //card_ocr = (ret->last_r0 >> 8) & 0xffff;
           ret->card_supports_sdhc = ((ret->last_r0) >> 30) & 0x1;

#ifdef SD_1_8V_SUPPORT
           if(!ret->failed_voltage_switch)
                ret->card_supports_18v = (ret->last_r0 >> 24) & 0x1;
#endif

           card_is_busy = 0;
       }
       else
       {
           // Card is still busy
#ifdef EMMC_DEBUG
            printf("SD: card is busy, retrying\r\n");
#endif
            usleep(1000);
       }
   }

#ifdef EMMC_DEBUG
   printf("SD: card identified: OCR: %04"PRIx32", 1.8v support: %"PRIu32", SDHC support: %"PRIu32"\r\n",
       (ret->last_r0 >> 8) & 0xffff, (uint32_t)ret->card_supports_18v, (uint32_t)ret->card_supports_sdhc);
#endif


   // Switch to 1.8V mode if possible
   if(0)// PiDoesn't support 1.8v //ret->card_supports_18v)
   {
        // A small wait before the voltage switch
        usleep(1000);

#ifdef EMMC_DEBUG
        printf("SD: switching to 1.8V mode\r\n");
#endif
       return -1;
   }

#ifdef EMMC_DEBUG
//   printf("SD: card CID: %08"PRIu32"%08"PRIu32"%08"PRIu32"%08"PRIu32"\r\n", ret->last_r3, ret->last_r2, ret->last_r1, ret->last_r0);
#endif

    // Send CMD2 to read the card CID before requesting an RCA with CMD3.
    sd_issue_command(ret, ALL_SEND_CID, 0, 500000);
    if(FAIL(ret))
    {
          printf("SD: error sending ALL_SEND_CID\r\n");
          if (!dev) free(ret);
          return -1;
    }

  // ret->bd.device_id[0] = ret->last_r0;
  // ret->bd.device_id[1] = ret->last_r1;
  // ret->bd.device_id[2] = ret->last_r2;
  // ret->bd.device_id[3] = ret->last_r3;
  // ret->bd.dev_id_len = 4 * sizeof(uint32_t);

   // Send CMD3 to enter the data state
   sd_issue_command(ret, SEND_RELATIVE_ADDR, 0, 500000);
   if(FAIL(ret))
    {
        printf("SD: error sending SEND_RELATIVE_ADDR\r\n");
        if (!dev) free(ret);
        return -1;
    }

   uint32_t cmd3_resp = ret->last_r0;
#ifdef EMMC_DEBUG
   printf("SD: CMD3 response: %08"PRIu32"\r\n", cmd3_resp);
#endif

   ret->card_rca = (cmd3_resp >> 16) & 0xffff;
   uint32_t crc_error = (cmd3_resp >> 15) & 0x1;
   uint32_t illegal_cmd = (cmd3_resp >> 14) & 0x1;
   uint32_t error = (cmd3_resp >> 13) & 0x1;
   uint32_t status;// = (cmd3_resp >> 9) & 0xf;
   uint32_t ready = (cmd3_resp >> 8) & 0x1;

   if(crc_error)
   {
      printf("SD: CRC error\r\n");
      if (!dev) free(ret);
      return -1;
   }

   if(illegal_cmd)
   {
      printf("SD: illegal command\r\n");
      if (!dev) free(ret);
      return -1;
   }

   if(error)
   {
      printf("SD: generic error\r\n");
      if (!dev) free(ret);
      return -1;
   }

   if(!ready)
   {
      printf("SD: not ready for data\r\n");
      if (!dev) free(ret);
      return -1;
   }

#ifdef EMMC_DEBUG
   printf("SD: RCA: %04"PRIx32"\r\n", ret->card_rca);
#endif

   // Now select the card (toggles it to transfer state)
   sd_issue_command(ret, SELECT_CARD, ret->card_rca << 16, 500000);
   if(FAIL(ret))
   {
       printf("SD: error sending CMD7\r\n");
       if (!dev) free(ret);
       return -1;
   }

   uint32_t cmd7_resp = ret->last_r0;
   status = (cmd7_resp >> 9) & 0xf;

   if((status != 3) && (status != 4))
   {
      printf("SD: invalid status (%"PRIu32")\r\n", status);
      if (!dev) free(ret);
      return -1;
   }

   // If not an SDHC card, ensure BLOCKLEN is 512 bytes
   if(!ret->card_supports_sdhc)
   {
       sd_issue_command(ret, SET_BLOCKLEN, 512, 500000);
       if(FAIL(ret))
       {
           printf("SD: error sending SET_BLOCKLEN\r\n");
           if (!dev) free(ret);
           return -1;
       }
   }

    sdhost_write(SDHBCT, 0x200u);
    sdhost_write(SDHBLC, 1u);

   // Get the cards SCR register
   ret->scr = (struct sd_scr *)malloc(sizeof(struct sd_scr));
   ret->buf = &ret->scr->scr[0];
   ret->block_size = 8;
   ret->blocks_to_transfer = 1;
   sd_issue_command(ret, SEND_SCR, 0, 500000);
   ret->block_size = 512;
   if(FAIL(ret))
   {
       printf("SD: error sending SEND_SCR\r\n");
       free(ret->scr);
       if (!dev) free(ret);
       printf("******************************************\r\n");
       printf("* Reinitializing SD Card Driver          *\r\n");
       printf("******************************************\r\n");
       return sd_card_init((struct block_device **)&ret);
   }

   // Determine card version
   // Note that the SCR is big-endian
   uint32_t scr0 = byte_swap(ret->scr->scr[0]);
   ret->scr->sd_version = SD_VER_UNKNOWN;
   uint32_t sd_spec = (scr0 >> (56 - 32)) & 0xf;
   uint32_t sd_spec3 = (scr0 >> (47 - 32)) & 0x1;
   uint32_t sd_spec4 = (scr0 >> (42 - 32)) & 0x1;
   ret->scr->sd_bus_widths = (scr0 >> (48 - 32)) & 0xf;
   if(sd_spec == 0)
        ret->scr->sd_version = SD_VER_1;
    else if(sd_spec == 1)
        ret->scr->sd_version = SD_VER_1_1;
    else if(sd_spec == 2)
    {
        if(sd_spec3 == 0)
            ret->scr->sd_version = SD_VER_2;
        else
        {
            if(sd_spec4 == 0)
                ret->scr->sd_version = SD_VER_3;
            else
                ret->scr->sd_version = SD_VER_4;
        }
    }

#ifdef EMMC_DEBUG
    printf("SD: &scr: %p\r\n", (void *)&ret->scr->scr[0]);
    printf("SD: SCR[0]: %08"PRIu32", SCR[1]: %08"PRIu32"\r\n", ret->scr->scr[0], ret->scr->scr[1]);;
    printf("SD: SCR: %08"PRIu32"%08"PRIu32"\r\n", byte_swap(ret->scr->scr[0]), byte_swap(ret->scr->scr[1]));
    printf("SD: SCR: version %s, bus_widths %01"PRIu32"\r\n", sd_versions[ret->scr->sd_version],
           ret->scr->sd_bus_widths);
#endif

   // Keep identification timing through RCA assignment and SCR read, then
   // switch to the normal SDR12 transfer clock for the remaining setup.
   sd_set_storage_clock_rate(ret, base_clock, SD_CLOCK_NORMAL);

    if(ret->scr->sd_bus_widths & 0x4)
    {
        // Set 4-bit transfer mode (ACMD6)
        // See HCSS 3.4 for the algorithm
#ifdef SD_4BIT_DATA
#ifdef EMMC_DEBUG
        printf("SD: switching to 4-bit data mode\r\n");
#endif

        // Disable card interrupt in host
        // Send ACMD6 to change the card's bit mode
        sd_issue_command(ret, SET_BUS_WIDTH, 0x2, 500000);
        if(FAIL(ret))
            {printf("SD: switch to 4-bit data mode failed\r\n");}
        else
        {
            g_sdhost_storage_hcfg |= SDHCFG_WIDE_EXT_BUS | SDHCFG_WIDE_INT_BUS | SDHCFG_SLOW_CARD;
            sdhost_write(SDHCFG, g_sdhost_storage_hcfg);

#ifdef EMMC_DEBUG
                printf("SD: switch to 4-bit complete\r\n");
#endif
        }
#endif
    }
#ifdef EMMC_DEBUG
   printf("SD: found a valid version %s SD card\r\n", sd_versions[ret->scr->sd_version]);
   printf("SD: setup successful (status %"PRIu32")\r\n", status);
#endif
#endif
   // Reset interrupt register
   sdhost_write(SDHSTS, SDHSTS_BUSY_IRPT | SDHSTS_BLOCK_IRPT | SDHSTS_SDIO_IRPT | SDHSTS_DATA_FLAG | SDHSTS_ERROR_MASK);

   *dev = (struct block_device *)ret;
   return 0;
}

int sdhost_init_device(struct block_device **dev)
{
    return sd_card_init(dev);
}

static int sd_ensure_data_mode(struct emmc_block_dev *edev)
{
   if(edev->card_rca == 0)
   {
      // Try again to initialise the card
      int ret = sd_card_init((struct block_device **)&edev);
      if(ret != 0)
         return ret;
   }

#ifdef EMMC_DEBUG
   printf("SD: ensure_data_mode() obtaining status register for card_rca %08"PRIu32": ",
      edev->card_rca);
#endif

    sd_issue_command(edev, SEND_STATUS, edev->card_rca << 16, 500000);
    if(FAIL(edev))
    {
        printf("SD: ensure_data_mode() error sending CMD13\r\n");
        edev->card_rca = 0;
        return -1;
    }

   uint32_t status = edev->last_r0;
   uint32_t cur_state = (status >> 9) & 0xf;
#ifdef EMMC_DEBUG
   printf("status %"PRIu32"\r\n", cur_state);
#endif
   if(cur_state == 3)
   {
      // Currently in the stand-by state - select it
      sd_issue_command(edev, SELECT_CARD, edev->card_rca << 16, 500000);
      if(FAIL(edev))
      {
         printf("SD: ensure_data_mode() no response from CMD17\r\n");
         edev->card_rca = 0;
         return -1;
      }
   }
   else if(cur_state == 5)
   {
      // In the data transfer state - cancel the transmission
      sd_issue_command(edev, STOP_TRANSMISSION, 0, 500000);
      if(FAIL(edev))
      {
         printf("SD: ensure_data_mode() no response from CMD12\r\n");
         edev->card_rca = 0;
         return -1;
      }

      // Reset the data circuit
    sd_reset_dat_for_dev(edev);
   }
   else if(cur_state != 4)
   {
      // Not in the transfer state - re-initialise
      int ret = sd_card_init((struct block_device **)&edev);
      if(ret != 0)
         return ret;
   }

   // Check again that we're now in the correct mode
   if(cur_state != 4)
   {
#ifdef EMMC_DEBUG
      printf("SD: ensure_data_mode() rechecking status: ");
#endif
        sd_issue_command(edev, SEND_STATUS, edev->card_rca << 16, 500000);
        if(FAIL(edev))
      {
         printf("SD: ensure_data_mode() no response from CMD13\r\n");
         edev->card_rca = 0;
         return -1;
      }
      status = edev->last_r0;
      cur_state = (status >> 9) & 0xf;

#ifdef EMMC_DEBUG
      printf("%"PRIu32"\r\n", cur_state);
#endif

      if(cur_state != 4)
      {
         printf("SD: unable to initialise SD card to "
               "data mode (state %"PRIu32")\r\n", cur_state);
         edev->card_rca = 0;
         return -1;
      }
   }

   return 0;
}

#ifdef SDMA_SUPPORT
// We only support DMA transfers to buffers aligned on a 4 kiB boundary
static int sd_suitable_for_dma(void *buf)
{
    if((uintptr_t)buf & 0xfff)
        return 0;
    else
        return 1;
}
#endif

static int sd_do_data_command(struct emmc_block_dev *edev, int is_write, uint8_t *buf, size_t buf_size, uint32_t block_no)
{
   // PLSS table 4.20 - SDSC cards use byte addresses rather than block addresses
   if(!edev->card_supports_sdhc)
      block_no *= 512;

   // This is as per HCSS 3.7.2.1
   if(buf_size < edev->block_size)
   {
       printf("SD: do_data_command() called with buffer size (%zu) less than "
            "block size (%zu)\r\n", buf_size, edev->block_size);
        return -1;
   }

   edev->blocks_to_transfer = buf_size / edev->block_size;
   if(buf_size % edev->block_size)
   {
       printf("SD: do_data_command() called with buffer size (%zu) not an "
            "exact multiple of block size (%zu)\r\n", buf_size, edev->block_size);
        return -1;
   }
   edev->buf = buf;

   // Decide on the command to use
   unsigned int command;
   if(is_write)
   {
       if(edev->blocks_to_transfer > 1)
            command = WRITE_MULTIPLE_BLOCK;
        else
            command = WRITE_BLOCK;
   }
   else
    {
        if(edev->blocks_to_transfer > 1)
            command = READ_MULTIPLE_BLOCK;
        else
            command = READ_SINGLE_BLOCK;
    }

   int retry_count = 0;
   int max_retries = 3;
   while(retry_count < max_retries)
   {
#ifdef SDMA_SUPPORT
       // use SDMA for the first try only
       if((retry_count == 0) && sd_suitable_for_dma(buf))
            edev->use_sdma = 1;
        else
        {
#ifdef EMMC_DEBUG
            printf("SD: retrying without SDMA\r\n");
#endif
            edev->use_sdma = 0;
        }
#else
        edev->use_sdma = 0;
#endif

        sd_issue_command(edev, command, block_no, 5000000);

        if(SUCCESS(edev))
            break;
        else
        {
            printf("SD: error sending CMD%ui, ", command);
            printf("error = %08"PRIu32".  ", edev->last_error);
            retry_count++;
            if(retry_count < max_retries)
                {printf("Retrying...\r\n");}
        }
   }
   if(retry_count == max_retries)
    {
        printf("Giving up.\r\n");
        edev->card_rca = 0;
        return -1;
    }

    return 0;
}

size_t sd_read(struct block_device *dev, uint8_t *buf, size_t buf_size, uint32_t block_no)
{
   // Check the status of the card
   struct emmc_block_dev *edev = (struct emmc_block_dev *)dev;

    if(sd_ensure_data_mode(edev) != 0)
        return 0;

#ifdef EMMC_DEBUG
   printf("SD: read() card ready, reading from block %"PRIx32"\r\n", block_no);
#endif

    if(sd_do_data_command(edev, 0, buf, buf_size, block_no) < 0)
        return 0;

#ifdef EMMC_DEBUG
   printf("SD: data read successful\r\n");
#endif

   return buf_size;
}

#ifdef SD_WRITE_SUPPORT
size_t sd_write(struct block_device *dev, const uint8_t *buf, size_t buf_size, uint32_t block_no)
{
   // Check the status of the card
   struct emmc_block_dev *edev = (struct emmc_block_dev *)dev;
    if(sd_ensure_data_mode(edev) != 0)
        return 0;

#ifdef EMMC_DEBUG
   printf("SD: write() card ready, writing to block %"PRIu32"\r\n", block_no);
#endif

    if(sd_do_data_command(edev, 1, (uint8_t *)(uintptr_t)buf, buf_size, block_no) < 0)
        return 0;

#ifdef EMMC_DEBUG
    printf("SD: data write successful\r\n");
#endif

   return buf_size;
}
#endif
