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

typedef struct
{
   rpi_reg_rw_t EMMC_ARG2;
   rpi_reg_rw_t EMMC_BLKSIZECNT;
   rpi_reg_rw_t EMMC_ARG1;
   rpi_reg_rw_t EMMC_CMDTM;
   rpi_reg_rw_t EMMC_RESP0;
   rpi_reg_rw_t EMMC_RESP1; // cppcheck-suppress unusedStructMember
   rpi_reg_rw_t EMMC_RESP2; // cppcheck-suppress unusedStructMember
   rpi_reg_rw_t EMMC_RESP3; // cppcheck-suppress unusedStructMember
   rpi_reg_rw_t EMMC_DATA;
   rpi_reg_rw_t EMMC_STATUS;
   rpi_reg_rw_t EMMC_CONTROL0;
   rpi_reg_rw_t EMMC_CONTROL1;
   rpi_reg_rw_t EMMC_INTERRUPT;
   rpi_reg_rw_t EMMC_IRPT_MASK;
   rpi_reg_rw_t EMMC_IRPT_EN;
   rpi_reg_rw_t EMMC_CONTROL2;
   rpi_reg_rw_t EMMC_CAPABILITIES_0; // cppcheck-suppress unusedStructMember
   rpi_reg_rw_t EMMC_CAPABILITIES_1; // cppcheck-suppress unusedStructMember
   rpi_reg_rw_t EMMC_HWMAXAMP0; // cppcheck-suppress unusedStructMember
   rpi_reg_ro_t reserved1; // cppcheck-suppress unusedStructMember
   rpi_reg_rw_t EMMC_FORCE_IRPT; // cppcheck-suppress unusedStructMember
   rpi_reg_rw_t EMMC_DMA_STATUS; // cppcheck-suppress unusedStructMember
   rpi_reg_ro_t reserved2[6]; // cppcheck-suppress unusedStructMember
   rpi_reg_rw_t EMMC_BOOT_TIMEOUT; // cppcheck-suppress unusedStructMember
   rpi_reg_rw_t EMMC_DBG_SEL; // cppcheck-suppress unusedStructMember
   rpi_reg_ro_t reserved3[2]; // cppcheck-suppress unusedStructMember
   rpi_reg_rw_t EMMC_EXRDFIFO_CFG; // cppcheck-suppress unusedStructMember
   rpi_reg_rw_t EMMC_EXRDFIFO_EN; // cppcheck-suppress unusedStructMember
   rpi_reg_rw_t EMMC_TUNE_STEP; // cppcheck-suppress unusedStructMember
   rpi_reg_rw_t EMMC_TUNE_STEPS_STD; // cppcheck-suppress unusedStructMember
   rpi_reg_rw_t EMMC_TUNE_STEPS_DDR; // cppcheck-suppress unusedStructMember
   rpi_reg_ro_t reserved4[19]; // cppcheck-suppress unusedStructMember
   rpi_reg_rw_t EMMC_BUS_CTRL; // cppcheck-suppress unusedStructMember
   rpi_reg_ro_t reserved5[3]; // cppcheck-suppress unusedStructMember
   rpi_reg_rw_t EMMC_SPI_INT_SPT; // cppcheck-suppress unusedStructMember
   rpi_reg_ro_t reserved6[2]; // cppcheck-suppress unusedStructMember
   rpi_reg_rw_t EMMC_SLOTISR_VER; // cppcheck-suppress unusedStructMember
} rpi_emmc_t;

#define EMMC_BASE    (PERIPHERAL_BASE + 0x0300000UL)

static rpi_emmc_t* const RPI_EMMCBase = (rpi_emmc_t*) EMMC_BASE;

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

#ifdef EMMC_DEBUG
static const char *sd_versions[] = { "unknown", "1.0 and 1.01", "1.10",
    "2.00", "3.0x", "4.xx" };

static const char *err_irpts[] = { "CMD_TIMEOUT", "CMD_CRC", "CMD_END_BIT", "CMD_INDEX",
   "DATA_TIMEOUT", "DATA_CRC", "DATA_END_BIT", "CURRENT_LIMIT",
   "AUTO_CMD12", "ADMA", "TUNING", "RSVD" };
#endif

size_t sd_read(struct block_device *dev, uint8_t *buf, size_t buf_size, uint32_t block_no);
size_t sd_write(struct block_device *dev, uint8_t *buf, size_t buf_size, uint32_t block_no);

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

#define SD_RESET_CMD            (1 << 25)
#define SD_RESET_DAT            (1 << 26)
#define SD_RESET_ALL            (1 << 24)

#define SD_GET_CLOCK_DIVIDER_FAIL   0xffffffff

// Get the current base clock rate in Hz
#if SDHCI_IMPLEMENTATION == SDHCI_IMPLEMENTATION_BCM_2708
#include "info.h"
#endif

static void sd_power_off()
{
   /* Power off the SD card */
   uint32_t control0 = RPI_EMMCBase->EMMC_CONTROL0;
   control0 &= ~(1U << 8);  // Set SD Bus Power bit off in Power Control Register
   RPI_EMMCBase->EMMC_CONTROL0 = control0;
}

static uint32_t sd_get_base_clock_hz()
{
   uint32_t base_clock;
#if SDHCI_IMPLEMENTATION == SDHCI_IMPLEMENTATION_GENERIC
   uint32_t capabilities_0 = RPI_EMMCBase->EMMC_CAPABILITIES_0;
   base_clock = ((capabilities_0 >> 8) & 0xff) * 1000000;
#elif SDHCI_IMPLEMENTATION == SDHCI_IMPLEMENTATION_BCM_2708
   base_clock = get_clock_rate(EMMC_CLK_ID);
#else
   printf("EMMC: get_base_clock_hz() is not implemented for this "
          "architecture.\r\n");
   return 0;
#endif

#ifdef EMMC_DEBUG
   printf("EMMC: base clock rate is %"PRIu32" Hz\r\n", base_clock);
#endif
   return base_clock;
}

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

// Set the clock dividers to generate a target value
static uint32_t sd_get_clock_divider(uint32_t base_clock, uint32_t target_rate)
{
    // TODO: implement use of preset value registers

    uint32_t targetted_divisor = 0;
    if(target_rate > base_clock)
        targetted_divisor = 1;
    else
    {
        targetted_divisor = base_clock / target_rate;
        uint32_t mod = base_clock % target_rate;
        if(mod)
            targetted_divisor--;
    }

    // Decide on the clock mode to use

    // HCI version 3 or greater supports 10-bit divided clock mode
    // This requires a power-of-two divider

    // Find the first bit set
    int divisor = -1;
    for(int first_bit = 31; first_bit >= 0; first_bit--)
    {
        uint32_t bit_test = (1u << first_bit);
        if(targetted_divisor & bit_test)
        {
            divisor = first_bit;
            targetted_divisor &= ~bit_test;
            if(targetted_divisor)
            {
                // The divisor is not a power-of-two, increase it
                divisor++;
            }
            break;
        }
    }

    if(divisor == -1)
        divisor = 11;
    if(divisor > 11)
        divisor = 11;

    if(divisor != 0)
        divisor = (1 << (divisor - 1));

    if(divisor >= 0x400)
        divisor = 0x3ff;

    uint32_t freq_select = divisor & 0xff;
    uint32_t upper_bits = (divisor >> 8) & 0x3;
    uint32_t ret = (freq_select << 8) | (upper_bits << 6);

#ifdef EMMC_DEBUG
    unsigned int denominator = 1;
    if(divisor != 0)
        denominator = (unsigned int )divisor * 2;
    unsigned int actual_clock = base_clock / denominator;
    printf("EMMC: base_clock: %"PRIu32", target_rate: %"PRIu32", divisor: %08i, "
            "actual_clock: %i, ret: %08"PRIx32"\r\n", base_clock, target_rate,
            divisor, actual_clock, ret);
#endif

    return ret;
}

// Switch the clock rate whilst running
static int sd_switch_clock_rate(uint32_t base_clock, uint32_t target_rate)
{
    // Decide on an appropriate divider
    uint32_t divider = sd_get_clock_divider(base_clock, target_rate);

    // Wait for the command inhibit (CMD and DAT) bits to clear
    while(RPI_EMMCBase->EMMC_STATUS & 0x3)
        usleep(1);

    // Set the SD clock off
    uint32_t control1 = RPI_EMMCBase->EMMC_CONTROL1;
    control1 &= ~(1U << 2);
    RPI_EMMCBase->EMMC_CONTROL1 = control1;
    usleep(5); // Wait 2 SCLKs

    // Write the new divider
    control1 &= ~0xffe0U;    // Clear old setting + clock generator select
    control1 |= divider;
    RPI_EMMCBase->EMMC_CONTROL1 = control1;
    usleep(5); // Wait 2 SCLKs

    // Enable the SD clock
    control1 |= (1 << 2);
    RPI_EMMCBase->EMMC_CONTROL1 = control1;

#ifdef EMMC_DEBUG
    printf("EMMC: successfully set clock rate to %"PRIu32" Hz\r\n", target_rate);
#endif
    return 0;
}

// Reset the CMD line
static int sd_reset_cmd()
{
   uint32_t control1 = RPI_EMMCBase->EMMC_CONTROL1;
   control1 |= SD_RESET_CMD;
   RPI_EMMCBase->EMMC_CONTROL1 = control1;
   TIMEOUT_WAIT(((RPI_EMMCBase->EMMC_CONTROL1 & SD_RESET_CMD) == 0), 1000000);
   if((RPI_EMMCBase->EMMC_CONTROL1 & SD_RESET_CMD) != 0)
   {
      printf("EMMC: CMD line did not reset properly\r\n");
      return -1;
   }
   return 0;
}

// Reset the DAT line
static int sd_reset_dat()
{
   uint32_t control1 = RPI_EMMCBase->EMMC_CONTROL1;
   control1 |= SD_RESET_DAT;
   RPI_EMMCBase->EMMC_CONTROL1 = control1;
   TIMEOUT_WAIT((RPI_EMMCBase->EMMC_CONTROL1 & SD_RESET_DAT) == 0, 1000000);
   if((RPI_EMMCBase->EMMC_CONTROL1 & SD_RESET_DAT) != 0)
   {
      printf("EMMC: DAT line did not reset properly\r\n");
      return -1;
   }
   return 0;
}

static void sd_issue_command_int(struct emmc_block_dev *dev, uint32_t cmd_reg, uint32_t argument, uint32_t timeout)
{
    dev->last_cmd_success = 0;

    // This is as per HCSS 3.7.1.1/3.7.2.2

    // Check Command Inhibit
    while(RPI_EMMCBase->EMMC_STATUS & 0x1)
        usleep(1);

    // Is the command with busy?
    if((cmd_reg & SD_CMD_RSPNS_TYPE_MASK) == SD_CMD_RSPNS_TYPE_48B)
    {
        // With busy

        // Is is an abort command?
        if((cmd_reg & SD_CMD_TYPE_MASK) != SD_CMD_TYPE_ABORT)
        {
            // Not an abort command

            // Wait for the data line to be free
            while(RPI_EMMCBase->EMMC_STATUS & 0x2)
                usleep(1);
        }
    }

    // Is this a DMA transfer?
    int is_sdma = 0;
    if((cmd_reg & SD_CMD_ISDATA) && (dev->use_sdma))
    {
#ifdef EMMC_DEBUG
        printf("SD: performing SDMA transfer, current INTERRUPT: %08"PRIx32"\r\n",
               RPI_EMMCBase->EMMC_INTERRUPT);
#endif
        is_sdma = 1;
    }

    if(is_sdma)
    {
        // Set system address register (ARGUMENT2 in RPi)

        // We need to define a 4 kiB aligned buffer to use here
        // Then convert its virtual address to a bus address
        RPI_EMMCBase->EMMC_ARG2 = SDMA_BUFFER_PA;
    }

    // Set block size and block count
    // For now, block size = 512 bytes, block count = 1,
    //  host SDMA buffer boundary = 4 kiB
    if(dev->blocks_to_transfer > 0xffff)
    {
        printf("SD: blocks_to_transfer too great (%"PRIu32")\r\n",
               dev->blocks_to_transfer);
        dev->last_cmd_success = 0;
        return;
    }
    uint32_t blksizecnt = dev->block_size | (dev->blocks_to_transfer << 16);
    RPI_EMMCBase->EMMC_BLKSIZECNT = blksizecnt;

    // Set argument 1 reg
    RPI_EMMCBase->EMMC_ARG1 = argument;

    if(is_sdma)
    {
        // Set Transfer mode register
        cmd_reg |= SD_CMD_DMA;
    }

    // Set command reg
    RPI_EMMCBase->EMMC_CMDTM = cmd_reg;

    // usleep(2000);//*****

    // Wait for command complete interrupt
    TIMEOUT_WAIT(RPI_EMMCBase->EMMC_INTERRUPT & 0x8001, timeout);
    uint32_t irpts = RPI_EMMCBase->EMMC_INTERRUPT;

    // Clear command complete status
    RPI_EMMCBase->EMMC_INTERRUPT = 0xffff0001;

    // Test for errors
    if((irpts & 0xffff0001) != 0x1)
    {
#ifdef EMMC_DEBUG
        printf("SD: error occurred whilst waiting for command complete interrupt\r\n");
#endif
        dev->last_error = irpts & 0xffff0000;
        dev->last_interrupt = irpts;
        return;
    }

    // usleep(2000); //***

    // Get response data
    switch(cmd_reg & SD_CMD_RSPNS_TYPE_MASK)
    {
        case SD_CMD_RSPNS_TYPE_48:
        case SD_CMD_RSPNS_TYPE_48B:
            dev->last_r0 = RPI_EMMCBase->EMMC_RESP0;
            break;

        case SD_CMD_RSPNS_TYPE_136:
            dev->last_r0 = RPI_EMMCBase->EMMC_RESP0;
         //   dev->last_r1 = RPI_EMMCBase->EMMC_RESP1;
         //   dev->last_r2 = RPI_EMMCBase->EMMC_RESP2;
         //   dev->last_r3 = RPI_EMMCBase->EMMC_RESP3;
            break;
    }

    // If with data, wait for the appropriate interrupt
    if((cmd_reg & SD_CMD_ISDATA) && (is_sdma == 0))
    {
        uint32_t wr_irpt;
        int is_write = 0;
        if(cmd_reg & SD_CMD_DAT_DIR_CH)
            wr_irpt = (1 << 5);     // read
        else
        {
            is_write = 1;
            wr_irpt = (1 << 4);     // write
        }

        unsigned int cur_block = 0;
        uint32_t *cur_buf_addr = (uint32_t *)dev->buf;
        while(cur_block < dev->blocks_to_transfer)
        {
#ifdef EMMC_DEBUG
         if(dev->blocks_to_transfer > 1)
            printf("SD: multi block transfer, awaiting block %u ready\r\n",
            cur_block);
#endif
            TIMEOUT_WAIT(RPI_EMMCBase->EMMC_INTERRUPT & (wr_irpt | 0x8000), timeout);
            irpts = RPI_EMMCBase->EMMC_INTERRUPT;
            RPI_EMMCBase->EMMC_INTERRUPT = 0xffff0000 | wr_irpt;

            if((irpts & (0xffff0000 | wr_irpt)) != wr_irpt)
            {
#ifdef EMMC_DEBUG
            printf("SD: error occurred whilst waiting for data ready interrupt\r\n");
#endif
                dev->last_error = irpts & 0xffff0000;
                dev->last_interrupt = irpts;
                return;
            }

            // Transfer the block
            uint32_t cur_byte_no = 0;
            while(cur_byte_no < dev->block_size)
            {
                if(is_write)
            {
                  RPI_EMMCBase->EMMC_DATA = *cur_buf_addr;
            }
                else
            {
                  *cur_buf_addr = RPI_EMMCBase->EMMC_DATA;
            }
                cur_byte_no += sizeof( RPI_EMMCBase->EMMC_DATA);
                cur_buf_addr++;
            }

#ifdef EMMC_DEBUG
         printf("SD: block %u transfer complete\r\n", cur_block);
#endif

            cur_block++;
        }
    }

    // Wait for transfer complete (set if read/write transfer or with busy)
    if((((cmd_reg & SD_CMD_RSPNS_TYPE_MASK) == SD_CMD_RSPNS_TYPE_48B) ||
       (cmd_reg & SD_CMD_ISDATA)) && (is_sdma == 0))
    {
        // First check command inhibit (DAT) is not already 0
        if((RPI_EMMCBase->EMMC_STATUS & 0x2) == 0)
            RPI_EMMCBase->EMMC_INTERRUPT = 0xffff0002;
        else
        {
            TIMEOUT_WAIT(RPI_EMMCBase->EMMC_INTERRUPT & 0x8002, timeout);
            irpts = RPI_EMMCBase->EMMC_INTERRUPT;
            RPI_EMMCBase->EMMC_INTERRUPT = 0xffff0002;

            // Handle the case where both data timeout and transfer complete
            //  are set - transfer complete overrides data timeout: HCSS 2.2.17
            if(((irpts & 0xffff0002) != 0x2) && ((irpts & 0xffff0002) != 0x100002))
            {
#ifdef EMMC_DEBUG
                printf("SD: error occurred whilst waiting for transfer complete interrupt\r\n");
#endif
                dev->last_error = irpts & 0xffff0000;
                dev->last_interrupt = irpts;
                return;
            }
            RPI_EMMCBase->EMMC_INTERRUPT = 0xffff0002;
        }
    }
    else if (is_sdma)
    {
        // For SDMA transfers, we have to wait for either transfer complete,
        //  DMA int or an error

        // First check command inhibit (DAT) is not already 0
        if((RPI_EMMCBase->EMMC_STATUS & 0x2) == 0)
            RPI_EMMCBase->EMMC_INTERRUPT = 0xffff000a;
        else
        {
            TIMEOUT_WAIT(RPI_EMMCBase->EMMC_INTERRUPT & 0x800a, timeout);
            irpts = RPI_EMMCBase->EMMC_INTERRUPT;
            RPI_EMMCBase->EMMC_INTERRUPT = 0xffff000a;

            // Detect errors
            if((irpts & 0x8000) && ((irpts & 0x2) != 0x2))
            {
#ifdef EMMC_DEBUG
                printf("SD: error occurred whilst waiting for transfer complete interrupt\r\n");
#endif
                dev->last_error = irpts & 0xffff0000;
                dev->last_interrupt = irpts;
                return;
            }

            // Detect DMA interrupt without transfer complete
            // Currently not supported - all block sizes should fit in the
            //  buffer
            if((irpts & 0x8) && ((irpts & 0x2) != 0x2))
            {
#ifdef EMMC_DEBUG
                printf("SD: error: DMA interrupt occurred without transfer complete\r\n");
#endif
                dev->last_error = irpts & 0xffff0000;
                dev->last_interrupt = irpts;
                return;
            }

            // Detect transfer complete
            if(irpts & 0x2)
            {
#ifdef EMMC_DEBUG
                printf("SD: SDMA transfer complete\r\n");
#endif
                // Transfer the data to the user buffer
                memcpy(dev->buf, (const void *)SDMA_BUFFER, dev->block_size);
            }
            else
            {
                // Unknown error
#ifdef EMMC_DEBUG
                if(irpts == 0)
                    printf("SD: timeout waiting for SDMA transfer to complete\r\n");
                else
                    printf("SD: unknown SDMA transfer error\r\n");

                printf("SD: INTERRUPT: %08"PRIx32", STATUS %08"PRIx32"\r\n", irpts,
                       RPI_EMMCBase->EMMC_STATUS);
#endif

                if((irpts == 0) && ((RPI_EMMCBase->EMMC_STATUS & 0x3) == 0x2))
                {
                    // The data transfer is ongoing, we should attempt to stop
                    //  it
#ifdef EMMC_DEBUG
                    printf("SD: aborting transfer\r\n");
#endif
                    RPI_EMMCBase->EMMC_CMDTM = sd_commands[STOP_TRANSMISSION];

#ifdef EMMC_DEBUG
                    // pause to let us read the screen
                    usleep(2000000);
#endif
                }
                dev->last_error = irpts & 0xffff0000;
                dev->last_interrupt = irpts;
                return;
            }
        }
    }

    // Return success
    dev->last_cmd_success = 1;
}

static void sd_handle_card_interrupt(struct emmc_block_dev *dev)
{
    // Handle a card interrupt

#ifdef EMMC_DEBUG
    uint32_t status = RPI_EMMCBase->EMMC_STATUS;

    printf("SD: card interrupt\r\n");
    printf("SD: controller status: %08"PRIx32"\r\n", status);
#endif

    // Get the card status
    if(dev->card_rca)
    {
        sd_issue_command_int(dev, sd_commands[SEND_STATUS], dev->card_rca << 16,
                         500000);
        if(FAIL(dev))
        {
#ifdef EMMC_DEBUG
            printf("SD: unable to get card status\r\n");
#endif
        }
        else
        {
#ifdef EMMC_DEBUG
            printf("SD: card status: %08"PRIx32"\r\n", dev->last_r0);
#endif
        }
    }
    else
    {
#ifdef EMMC_DEBUG
        printf("SD: no card currently selected\r\n");
#endif
    }
}

static void sd_handle_interrupts(struct emmc_block_dev *dev)
{
    uint32_t irpts = RPI_EMMCBase->EMMC_INTERRUPT;
    uint32_t reset_mask = 0;

    if(irpts & SD_COMMAND_COMPLETE)
    {
#ifdef EMMC_DEBUG
        printf("SD: spurious command complete interrupt\r\n");
#endif
        reset_mask |= SD_COMMAND_COMPLETE;
    }

    if(irpts & SD_TRANSFER_COMPLETE)
    {
#ifdef EMMC_DEBUG
        printf("SD: spurious transfer complete interrupt\r\n");
#endif
        reset_mask |= SD_TRANSFER_COMPLETE;
    }

    if(irpts & SD_BLOCK_GAP_EVENT)
    {
#ifdef EMMC_DEBUG
        printf("SD: spurious block gap event interrupt\r\n");
#endif
        reset_mask |= SD_BLOCK_GAP_EVENT;
    }

    if(irpts & SD_DMA_INTERRUPT)
    {
#ifdef EMMC_DEBUG
        printf("SD: spurious DMA interrupt\r\n");
#endif
        reset_mask |= SD_DMA_INTERRUPT;
    }

    if(irpts & SD_BUFFER_WRITE_READY)
    {
#ifdef EMMC_DEBUG
        printf("SD: spurious buffer write ready interrupt\r\n");
#endif
        reset_mask |= SD_BUFFER_WRITE_READY;
        sd_reset_dat();
    }

    if(irpts & SD_BUFFER_READ_READY)
    {
#ifdef EMMC_DEBUG
        printf("SD: spurious buffer read ready interrupt\r\n");
#endif
        reset_mask |= SD_BUFFER_READ_READY;
        sd_reset_dat();
    }

    if(irpts & SD_CARD_INSERTION)
    {
#ifdef EMMC_DEBUG
        printf("SD: card insertion detected\r\n");
#endif
        reset_mask |= SD_CARD_INSERTION;
    }

    if(irpts & SD_CARD_REMOVAL)
    {
#ifdef EMMC_DEBUG
        printf("SD: card removal detected\r\n");
#endif
        reset_mask |= SD_CARD_REMOVAL;
        dev->card_removal = 1;
    }

    if(irpts & SD_CARD_INTERRUPT)
    {
#ifdef EMMC_DEBUG
        printf("SD: card interrupt detected\r\n");
#endif
        sd_handle_card_interrupt(dev);
        reset_mask |= SD_CARD_INTERRUPT;
    }

    if(irpts & 0x8000)
    {
#ifdef EMMC_DEBUG
        printf("SD: spurious error interrupt: %08"PRIx32"\r\n", irpts);
#endif
        reset_mask |= 0xffff0000;
    }

    RPI_EMMCBase->EMMC_INTERRUPT = reset_mask;
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
        printf("SD: issuing command ACMD%"PRIi32"\r\n", command);
#endif

        if(sd_acommands[command] == SD_CMD_RESERVED(0))
        {
            printf("SD: invalid command ACMD%"PRIi32"\r\n", command);
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
        printf("SD: issuing command CMD%"PRIi32"\r\n", command);
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
                    printf(err_irpts[i]);
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

int sd_card_init(struct block_device **dev)
{
#ifdef EMMC_DEBUG
    // Check the sanity of the sd_commands and sd_acommands structures
    if(sizeof(sd_commands) != (64 * sizeof(uint32_t)))
    {
        printf("EMMC: fatal error, sd_commands of incorrect size: %zu"
               " expected %zu\r\n", sizeof(sd_commands),
               64 * sizeof(uint32_t));
        return -1;
    }
    if(sizeof(sd_acommands) != (64 * sizeof(uint32_t)))
    {
        printf("EMMC: fatal error, sd_acommands of incorrect size: %zu"
               " expected %zu\r\n", sizeof(sd_acommands),
               64 * sizeof(uint32_t));
        return -1;
    }
#endif
// don't bother power cycling the SDCARD it just wastes time.
#ifdef SDCARD_PWR_CYCLE
#if SDHCI_IMPLEMENTATION == SDHCI_IMPLEMENTATION_BCM_2708
   // Power cycle the card to ensure its in its startup state
   //if(bcm_2708_power_cycle() != 0)
   //{
   //   printf("EMMC: BCM2708 controller did not power cycle successfully\r\n");
  // }
  bcm_2708_power_cycle();
#ifdef EMMC_DEBUG
   //else
      printf("EMMC: BCM2708 controller power-cycled\r\n");
#endif
#endif
#endif
#ifdef EMMC_DEBUG
   // Read the controller version

   uint32_t ver = RPI_EMMCBase->EMMC_SLOTISR_VER;
   uint32_t sdversion = (ver >> 16) & 0xff;

   uint32_t vendor = ver >> 24;
   uint32_t slot_status = ver & 0xff;

   printf("EMMC: vendor %"PRIx32", sdversion %"PRIx32", slot_status %"PRIx32"\r\n", vendor, sdversion, slot_status);

   if(sdversion < 2)
   {
      printf("EMMC: only SDHCI versions >= 3.0 are supported\r\n");
      return -1;
   }
#endif

#ifdef RESET_CONTROLLER

   // Reset the controller
#ifdef EMMC_DEBUG
   printf("EMMC: resetting controller\r\n");
#endif
   uint32_t control1 = RPI_EMMCBase->EMMC_CONTROL1;
   control1 |= (1U << 24);
   // Disable clock
   control1 &= ~(1U << 2);
   control1 &= ~(1U << 0);
   RPI_EMMCBase->EMMC_CONTROL1 = control1;
   TIMEOUT_WAIT((RPI_EMMCBase->EMMC_CONTROL1 & (0x7 << 24)) == 0, 1000000);
   if((RPI_EMMCBase->EMMC_CONTROL1 & (0x7 << 24)) != 0)
   {
      printf("EMMC: controller did not reset properly\r\n");
      return -1;
   }
#ifdef EMMC_DEBUG
   printf("EMMC: control0: %08"PRIx32", control1: %08"PRIx32", control2: %08"PRIx32"\r\n",
         RPI_EMMCBase->EMMC_CONTROL0,
         RPI_EMMCBase->EMMC_CONTROL1,
         RPI_EMMCBase->EMMC_CONTROL2);

   // Read the capabilities registers
   uint32_t capabilities_0 = RPI_EMMCBase->EMMC_CAPABILITIES_0;
   uint32_t capabilities_1 = RPI_EMMCBase->EMMC_CAPABILITIES_1;

   printf("EMMC: capabilities: %08"PRIx32"%08"PRIx32"\r\n", capabilities_1, capabilities_0);
#endif

   // Check for a valid card
#ifdef EMMC_DEBUG
   printf("EMMC: checking for an inserted card\r\n");
#endif
    TIMEOUT_WAIT(RPI_EMMCBase->EMMC_STATUS & (1 << 16), 500000);
   uint32_t status_reg = RPI_EMMCBase->EMMC_STATUS;
   if((status_reg & (1 << 16)) == 0)
   {
      printf("EMMC: no card inserted\r\n");
      return -1;
   }
#ifdef EMMC_DEBUG
   printf("EMMC: status: %08"PRIx32"\r\n", status_reg);
#endif

   // Clear control2
   RPI_EMMCBase->EMMC_CONTROL2 = 0;
   // Get the base clock rate
   uint32_t base_clock = sd_get_base_clock_hz();
#if 0
   if(base_clock == 0)
   {
       printf("EMMC: assuming clock rate to be 100MHz\r\n");
       base_clock = 100000000;
   }
#endif
   // Set clock rate to something slow
#ifdef EMMC_DEBUG
   printf("EMMC: setting clock rate\r\n");
#endif
   control1 = RPI_EMMCBase->EMMC_CONTROL1;
   control1 |= 1;       // enable clock

   // Set to identification frequency (400 kHz)
   uint32_t f_id = sd_get_clock_divider(base_clock, SD_CLOCK_ID);
   control1 |= f_id;

  // control1 |= (7 << 16);     // data timeout = TMCLK * 2^10
   control1 &= ~(0x0FU << 16); // mask timeout bits
   control1 |=  (0x0BU << 16); //data timeout = TMCLK * 2^(x+13)
   RPI_EMMCBase->EMMC_CONTROL1 = control1;
   TIMEOUT_WAIT(RPI_EMMCBase->EMMC_CONTROL1 & 0x2, 0x1000000);
   if((RPI_EMMCBase->EMMC_CONTROL1 & 0x2) == 0)
   {
      printf("EMMC: controller's clock did not stabilise within 1 second\r\n");
      return -1;
   }
#ifdef EMMC_DEBUG
   printf("EMMC: control0: %08"PRIx32", control1: %08"PRIx32"\r\n",
         RPI_EMMCBase->EMMC_CONTROL0,
         RPI_EMMCBase->EMMC_CONTROL1);
#endif

   // Enable the SD clock
#ifdef EMMC_DEBUG
   printf("EMMC: enabling SD clock\r\n");
#endif
   control1 = RPI_EMMCBase->EMMC_CONTROL1;
   control1 |= 4;
   RPI_EMMCBase->EMMC_CONTROL1 = control1;
   usleep(6);
#ifdef EMMC_DEBUG
   printf("EMMC: SD clock enabled\r\n");
#endif

   // Mask off sending interrupts to the ARM
   RPI_EMMCBase->EMMC_IRPT_EN = 0;
   // Reset interrupts
   RPI_EMMCBase->EMMC_INTERRUPT = 0xffffffff;
   // Have all interrupts sent to the INTERRUPT register
   uint32_t irpt_mask = (~SD_CARD_INTERRUPT);
#ifdef SD_CARD_INTERRUPTS
   irpt_mask |= SD_CARD_INTERRUPT;
#endif
   RPI_EMMCBase->EMMC_IRPT_MASK = irpt_mask;

#ifdef EMMC_DEBUG
   printf("EMMC: interrupts disabled\r\n");
#endif
   usleep(6);
#endif // end if RESET controller
    // Prepare the device structure
   struct emmc_block_dev *ret;
   if(*dev == NULL)
      ret = (struct emmc_block_dev *)malloc(sizeof(struct emmc_block_dev));
   else
      ret = (struct emmc_block_dev *)*dev;

   assert(ret);

   memset(ret, 0, sizeof(struct emmc_block_dev));
 //  ret->bd.driver_name = "emmc";
 //  ret->bd.device_name = "emmc0";
//   ret->bd.block_size = 512;
   ret->bd.read = sd_read;
#ifdef SD_WRITE_SUPPORT
    ret->bd.write = sd_write;
#endif
  // these aren't actually used anywhere
  //  ret->bd.supports_multiple_block_read = 1;
  //  ret->bd.supports_multiple_block_write = 1;
  //  ret->base_clock = base_clock;

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
        if(sd_reset_cmd() == -1)
            return -1;
        RPI_EMMCBase->EMMC_INTERRUPT = SD_ERR_MASK_CMD_TIMEOUT;
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
            if(sd_reset_cmd() == -1)
                return -1;
            RPI_EMMCBase->EMMC_INTERRUPT = SD_ERR_MASK_CMD_TIMEOUT;
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
         (ret->last_r0 >> 8) & 0xffff, ret->card_supports_18v, ret->card_supports_sdhc);
#endif

    // At this point, we know the card is definitely an SD card, so will definitely
   //  support SDR12 mode which runs at 25 MHz
    sd_switch_clock_rate(base_clock, SD_CLOCK_NORMAL);


   // Switch to 1.8V mode if possible
   if(0)// PiDoesn't support 1.8v //ret->card_supports_18v)
   {
        // A small wait before the voltage switch
        usleep(1000);

#ifdef EMMC_DEBUG
        printf("SD: switching to 1.8V mode\r\n");
#endif
       // As per HCSS 3.6.1

       // Send VOLTAGE_SWITCH
       sd_issue_command(ret, VOLTAGE_SWITCH, 0, 500000);
       if(FAIL(ret))
       {
#ifdef EMMC_DEBUG
            printf("SD: error issuing VOLTAGE_SWITCH\r\n");
#endif
           ret->failed_voltage_switch = 1;
           sd_power_off();
           return sd_card_init((struct block_device **)&ret);
       }

       // Disable SD clock
       control1 = RPI_EMMCBase->EMMC_CONTROL1;
       control1 &= ~(1U << 2);
       RPI_EMMCBase->EMMC_CONTROL1 = control1;

       // Check DAT[3:0]
       status_reg = RPI_EMMCBase->EMMC_STATUS;
       uint32_t dat30 = (status_reg >> 20) & 0xf;
       if(dat30 != 0)
       {
#ifdef EMMC_DEBUG
            printf("SD: DAT[3:0] did not settle to 0\r\n");
#endif
           ret->failed_voltage_switch = 1;
           sd_power_off();
           return sd_card_init((struct block_device **)&ret);
       }

       // Set 1.8V signal enable to 1
       uint32_t control0 = RPI_EMMCBase->EMMC_CONTROL0;
       control0 |= (1 << 8);
       RPI_EMMCBase->EMMC_CONTROL0 = control0;

       // Wait 5 ms
       usleep(5000);

       // Check the 1.8V signal enable is set
       control0 = RPI_EMMCBase->EMMC_CONTROL0;
       if(((control0 >> 8) & 0x1) == 0)
       {
#ifdef EMMC_DEBUG
            printf("SD: controller did not keep 1.8V signal enable high\r\n");
#endif
           ret->failed_voltage_switch = 1;
         sd_power_off();
           return sd_card_init((struct block_device **)&ret);
       }

       // Re-enable the SD clock
       control1 = RPI_EMMCBase->EMMC_CONTROL1;
       control1 |= (1 << 2);
       RPI_EMMCBase->EMMC_CONTROL1 = control1;

       // Wait 1 ms
       usleep(10000);

       // Check DAT[3:0]
       status_reg = RPI_EMMCBase->EMMC_STATUS;
       dat30 = (status_reg >> 20) & 0xf;
       if(dat30 != 0xf)
       {
#ifdef EMMC_DEBUG
            printf("SD: DAT[3:0] did not settle to 1111b (%01"PRIx32")\r\n", dat30);
#endif
           ret->failed_voltage_switch = 1;
           sd_power_off();
           return sd_card_init((struct block_device **)&ret);
       }

#ifdef EMMC_DEBUG
        printf("SD: voltage switch complete\r\n");
#endif
   }

   // Send CMD2 to get the cards CID
   sd_issue_command(ret, ALL_SEND_CID, 0, 500000);
   if(FAIL(ret))
   {
       printf("SD: error sending ALL_SEND_CID\r\n");
       return -1;
   }

#ifdef EMMC_DEBUG
//   printf("SD: card CID: %08"PRIu32"%08"PRIu32"%08"PRIu32"%08"PRIu32"\r\n", ret->last_r3, ret->last_r2, ret->last_r1, ret->last_r0);
#endif

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

   uint32_t controller_block_size = RPI_EMMCBase->EMMC_BLKSIZECNT;
   controller_block_size &= (~0xfffU);
   controller_block_size |= 0x200U;
   RPI_EMMCBase->EMMC_BLKSIZECNT = controller_block_size;

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
       sd_power_off();
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

    if(ret->scr->sd_bus_widths & 0x4)
    {
        // Set 4-bit transfer mode (ACMD6)
        // See HCSS 3.4 for the algorithm
#ifdef SD_4BIT_DATA
#ifdef EMMC_DEBUG
        printf("SD: switching to 4-bit data mode\r\n");
#endif

        // Disable card interrupt in host
        uint32_t old_irpt_mask = RPI_EMMCBase->EMMC_IRPT_MASK;
        uint32_t new_iprt_mask = old_irpt_mask & ~(1U << 8);
        RPI_EMMCBase->EMMC_IRPT_MASK = new_iprt_mask;

        // Send ACMD6 to change the card's bit mode
        sd_issue_command(ret, SET_BUS_WIDTH, 0x2, 500000);
        if(FAIL(ret))
            {printf("SD: switch to 4-bit data mode failed\r\n");}
        else
        {
            // Change bit mode for Host
            uint32_t control0 = RPI_EMMCBase->EMMC_CONTROL0;
            control0 |= 0x2;
            RPI_EMMCBase->EMMC_CONTROL0 = control0;

            // Re-enable card interrupt in host
            RPI_EMMCBase->EMMC_IRPT_MASK = old_irpt_mask;

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
   RPI_EMMCBase->EMMC_INTERRUPT = 0xffffffff;

   *dev = (struct block_device *)ret;
   return 0;
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
      sd_reset_dat();
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
size_t sd_write(struct block_device *dev, uint8_t *buf, size_t buf_size, uint32_t block_no)
{
   // Check the status of the card
   struct emmc_block_dev *edev = (struct emmc_block_dev *)dev;
    if(sd_ensure_data_mode(edev) != 0)
        return 0;

#ifdef EMMC_DEBUG
   printf("SD: write() card ready, writing to block %"PRIu32"\r\n", block_no);
#endif

    if(sd_do_data_command(edev, 1, buf, buf_size, block_no) < 0)
        return 0;

#ifdef EMMC_DEBUG
   printf("SD: write read successful\r\n");
#endif

   return buf_size;
}
#endif
