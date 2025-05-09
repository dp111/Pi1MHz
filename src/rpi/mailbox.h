#ifndef RPI_MAILBOX_INTERFACE_H
#define RPI_MAILBOX_INTERFACE_H

#define PROP_BUFFER_SIZE 8192
#define PROP_SIZE        1024

#include "base.h"
#include <stdbool.h>

#define RPI_MAILBOX0_BASE    ( PERIPHERAL_BASE + 0xB880 )
#define RPI_MAILBOX1_BASE    ( PERIPHERAL_BASE + 0xB8A0 )

/* The available mailbox channels in the BCM2835 Mailbox interface.
   See https://github.com/raspberrypi/firmware/wiki/Mailboxes for
   information */
typedef enum {
    MB0_POWER_MANAGEMENT = 0,
    MB0_FRAMEBUFFER,
    MB0_VIRTUAL_UART,
    MB0_VCHIQ,
    MB0_LEDS,
    MB0_BUTTONS,
    MB0_TOUCHSCREEN,
    MB0_UNUSED,
    MB0_TAGS_ARM_TO_VC,
    MB0_TAGS_VC_TO_ARM,
} mailbox0_channel_t;

/* These defines come from the Broadcom Videocore driver source code, see:
   brcm_usrlib/dag/vmcsx/vcinclude/bcm2708_chip/arm_control.h */
#define ARM_MS_FULL  0x80000000
#define ARM_MS_EMPTY 0x40000000
#define ARM_MS_LEVEL 0x400000FF

/* Define a structure which defines the register access to a mailbox.
   Not all mailboxes support the full register set! */
typedef struct {
    rpi_reg_rw_t Data;
    rpi_reg_ro_t reserved1[3];
    rpi_reg_ro_t Poll;
    rpi_reg_ro_t Sender;
    rpi_reg_ro_t Status;
    rpi_reg_rw_t Configuration;
    } mailbox_t;


/**
    @brief An enum of the RPI->Videocore firmware mailbox property interface
    properties. Further details are available from
    https://github.com/raspberrypi/firmware/wiki/Mailbox-property-interface
*/
typedef enum {
    /* Videocore */
    TAG_GET_FIRMWARE_VERSION = 0x1,

    /* Hardware */
    TAG_GET_BOARD_MODEL = 0x10001,
    TAG_GET_BOARD_REVISION,
    TAG_GET_BOARD_MAC_ADDRESS,
    TAG_GET_BOARD_SERIAL,
    TAG_GET_ARM_MEMORY,
    TAG_GET_VC_MEMORY,
    TAG_GET_CLOCKS,

    /* Config */
    TAG_GET_COMMAND_LINE = 0x50001,

    /* Shared resource management */
    TAG_GET_DMA_CHANNELS = 0x60001,

    /* Power */
    TAG_GET_POWER_STATE = 0x20001,
    TAG_GET_TIMING,
    TAG_SET_POWER_STATE = 0x28001,

    /* Clocks */
    TAG_GET_CLOCK_STATE = 0x30001,
    TAG_SET_CLOCK_STATE = 0x38001,
    TAG_GET_CLOCK_RATE = 0x30002,
    TAG_SET_CLOCK_RATE = 0x38002,
    TAG_GET_MAX_CLOCK_RATE = 0x30004,
    TAG_GET_MIN_CLOCK_RATE = 0x30007,
    TAG_GET_TURBO = 0x30009,
    TAG_SET_TURBO = 0x38009,

    /* Voltage */
    TAG_GET_VOLTAGE = 0x30003,
    TAG_SET_VOLTAGE = 0x38003,
    TAG_GET_MAX_VOLTAGE = 0x30005,
    TAG_GET_MIN_VOLTAGE = 0x30008,
    TAG_GET_TEMPERATURE = 0x30006,
    TAG_GET_MAX_TEMPERATURE = 0x3000A,
    TAG_ALLOCATE_MEMORY = 0x3000C,
    TAG_LOCK_MEMORY = 0x3000D,
    TAG_UNLOCK_MEMORY = 0x3000E,
    TAG_RELEASE_MEMORY = 0x3000F,
    TAG_EXECUTE_CODE = 0x30010,
    TAG_ENABLE_GPU = 0x30012,
    TAG_LAUNCH_VPU1 = 0x30013,
    TAG_GET_DISPMANX_MEM_HANDLE = 0x30014,
    TAG_GET_EDID_BLOCK = 0x30020,

    /* Framebuffer */
    TAG_ALLOCATE_BUFFER = 0x40001,
    TAG_RELEASE_BUFFER = 0x48001,
    TAG_BLANK_SCREEN = 0x40002,
    TAG_GET_PHYSICAL_SIZE = 0x40003,
    TAG_TEST_PHYSICAL_SIZE = 0x44003,
    TAG_SET_PHYSICAL_SIZE = 0x48003,
    TAG_GET_VIRTUAL_SIZE = 0x40004,
    TAG_TEST_VIRTUAL_SIZE = 0x44004,
    TAG_SET_VIRTUAL_SIZE = 0x48004,
    TAG_GET_DEPTH = 0x40005,
    TAG_TEST_DEPTH = 0x44005,
    TAG_SET_DEPTH = 0x48005,
    TAG_GET_PIXEL_ORDER = 0x40006,
    TAG_TEST_PIXEL_ORDER = 0x44006,
    TAG_SET_PIXEL_ORDER = 0x48006,
    TAG_GET_ALPHA_MODE = 0x40007,
    TAG_TEST_ALPHA_MODE = 0x44007,
    TAG_SET_ALPHA_MODE = 0x48007,
    TAG_GET_PITCH = 0x40008,
    TAG_GET_VIRTUAL_OFFSET = 0x40009,
    TAG_TEST_VIRTUAL_OFFSET = 0x44009,
    TAG_SET_VIRTUAL_OFFSET = 0x48009,
    TAG_GET_OVERSCAN = 0x4000A,
    TAG_TEST_OVERSCAN = 0x4400A,
    TAG_SET_OVERSCAN = 0x4800A,
    TAG_GET_PALETTE = 0x4000B,
    TAG_TEST_PALETTE = 0x4400B,
    TAG_SET_PALETTE = 0x4800B,
    TAG_SET_CURSOR_INFO = 0x8011,
    TAG_SET_CURSOR_STATE = 0x8010

    } rpi_mailbox_tag_t;


typedef enum {
    TAG_STATE_REQUEST = 0,
    TAG_STATE_RESPONSE = 1,
    } rpi_tag_state_t;


typedef enum {
    PT_OSIZE = 0,
    PT_OREQUEST_OR_RESPONSE = 1,
    } rpi_tag_buffer_offset_t;

typedef enum {
    T_OIDENT = 0,
    T_OVALUE_SIZE = 1,
    T_ORESPONSE = 2,
    T_OVALUE = 3,
    } rpi_tag_offset_t;

typedef struct {
    unsigned int tag;
    unsigned int request;
    unsigned int byte_length;
    union {
        uint32_t value_32;
        unsigned char buffer_8[PROP_SIZE];
        uint32_t buffer_32[PROP_SIZE >> 2];
    } data;
    } rpi_mailbox_property_t;


/* Clock ID values */
#define   RES_CLK_ID 0x000000000
#define  EMMC_CLK_ID 0x000000001
#define  UART_CLK_ID 0x000000002
#define   ARM_CLK_ID 0x000000003
#define  CORE_CLK_ID 0x000000004
#define   V3D_CLK_ID 0x000000005
#define  H264_CLK_ID 0x000000006
#define   ISP_CLK_ID 0x000000007
#define SDRAM_CLK_ID 0x000000008
#define PIXEL_CLK_ID 0x000000009
#define   PWM_CLK_ID 0x00000000a

#define MIN_CLK_ID  0x000000001
#define MAX_CLK_ID  0x00000000a

extern void RPI_PropertyInit( void );
extern rpi_mailbox_property_t* RPI_PropertyGetWord( rpi_mailbox_tag_t tag, uint32_t data );
extern rpi_mailbox_property_t* RPI_PropertyGetBuffer(rpi_mailbox_tag_t tag);
extern void RPI_PropertySetWord(rpi_mailbox_tag_t tag, uint32_t id, uint32_t data);
extern void RPI_PropertyAddTag( rpi_mailbox_tag_t tag, ... );
extern void RPI_PropertyStart(rpi_mailbox_tag_t tag, uint32_t length);
extern void RPI_PropertyNewTag(rpi_mailbox_tag_t tag, uint32_t length);
extern void RPI_PropertyAdd(uint32_t data);
extern void RPI_PropertyAddTwoWords(uint32_t data, uint32_t data2);
extern unsigned int RPI_PropertyProcess( bool wait );
extern void RPI_PropertyProcessNoCheck( void );
extern rpi_mailbox_property_t* RPI_PropertyGet( rpi_mailbox_tag_t tag );

#endif
