// Pi1MHz.h

#ifndef Pi1MHz_H
#define Pi1MHz_H

#include "rpi/rpi.h"

#define RELEASENAME "v1.00"

#define PAGE_SIZE    0x100

// Memory map of arrays used by FIQ handler
#define Pi1MHz_MEM_BASE 0x100
#define Pi1MHZ_MEM_SIZE (PAGE_SIZE*2)
#define Pi1MHz_CB_BASE  0x400
#define Pi1MHz_CB_SIZE  (PAGE_SIZE*2*2*4)

//CLK1MHZ- pin 13 - GPIO27
//  nRST - Pin 37 – GPIO26
// NPCFD - pin 22 - GPIO25
// NPCFC - pin 18 – GPIO24

//    RX - pin 10 - GPIO15
//    TX = pin 8  - GPIO14

//  NIRQ - pin 32 - GPIO12
//  NNMI - pin 23 - GPIO11
//   RnW - Pin 19 – GPIO10

//    A7 - Pin 16 – GPIO23
//    A6 - Pin 15 – GPIO22
//    A5 - Pin 40 – GPIO21
//    A4 - Pin 38 – GPIO20
//    A3 - Pin 33 – GPIO19
//    A2 - Pin 12 – GPIO18
//    A1 - Pin 11 – GPIO17
//    A0 – Pin 36 – GPIO16
//
//    D7 - Pin 21 – GPIO9
//    D6 - Pin 24 – GPIO8
//    D5 - Pin 26 – GPIO7
//    D4 - Pin 31 – GPIO6
//    D3 - Pin 29 – GPIO5
//    D2 – Pin 7  – GPIO4
//    D1 – Pin 5  – GPIO3
//    D0 - Pin 3  – GPIO2

// not used GPIO 0 GPIO1 These are id pins
//    TEST pin 27  GPIO0
//    AUDIO pin 13

#define CLK1MHZ_PIN  (27)
#define NRST_PIN     (26)
#define NPCFD_PIN    (25)
#define NPCFC_PIN    (24)

#define A7_PIN       (23)
#define A6_PIN       (22)
#define A5_PIN       (21)
#define A4_PIN       (20)
#define A3_PIN       (19)
#define A2_PIN       (18)
#define A1_PIN       (17)
#define A0_PIN       (16)

#define RX_PIN       (15)
#define TX_PIN       (14)

#define AUDIO_PIN    (13)

#define NIRQ_PIN     (12)
#define NNMI_PIN     (11)
#define RNW_PIN      (10)

#define D7_PIN       (9)
#define D6_PIN       (8)
#define D5_PIN       (7)
#define D4_PIN       (6)
#define D3_PIN       (5)
#define D2_PIN       (4)
#define D1_PIN       (3)
#define D0_PIN       (2)

#define CLK1MHZ_MASK (1 << CLK1MHZ_PIN)
#define NRST_MASK    (1 << NRST_PIN)
#define NPCFD_MASK   (1 << NPCFD_PIN)
#define NPCFC_MASK   (1 << NPCFC_PIN)

#define RX_MASK      (1 << RX_PIN)
#define TX_MASK      (1 << TX_PIN)

#define NIRQ_MASK    (1 << NIRQ_PIN)
#define NNMI_MASK    (1 << NNMI_PIN)
#define RNW_MASK     (1 << RNW_PIN)

#define A7_MASK      (1u << A7_PIN)
#define A6_MASK      (1u << A6_PIN)
#define A5_MASK      (1u << A5_PIN)
#define A4_MASK      (1u << A4_PIN)
#define A3_MASK      (1u << A3_PIN)
#define A2_MASK      (1u << A2_PIN)
#define A1_MASK      (1u << A1_PIN)
#define A0_MASK      (1u << A0_PIN)

#define D7_MASK      (1u << D7_PIN)
#define D6_MASK      (1u << D6_PIN)
#define D5_MASK      (1u << D5_PIN)
#define D4_MASK      (1u << D4_PIN)
#define D3_MASK      (1u << D3_PIN)
#define D2_MASK      (1u << D2_PIN)
#define D1_MASK      (1u << D1_PIN)
#define D0_MASK      (1u << D0_PIN)

#define DATABUS_MASK (D7_MASK | D6_MASK | D5_MASK | D4_MASK | D3_MASK | D2_MASK | D1_MASK | D0_MASK)
#define ADDRBUS_MASK (A7_MASK | A6_MASK | A5_MASK | A4_MASK | A3_MASK | A2_MASK | A1_MASK | A0_MASK)

#define DATABUS_TO_OUTPUTS ((1<<(D7_PIN*3))|(1<<(D6_PIN*3))|(1<<(D5_PIN*3))|(1<<(D4_PIN*3))|(1<<(D3_PIN*3))|(1<<(D2_PIN*3))|(1<<(D1_PIN*3))|(1<<(D0_PIN*3))) 

#define DATABUS_SHIFT D0_PIN
#define ADDRBUS_SHIFT A0_PIN

#define TEST_PIN     (0)
#define TEST_MASK    (1 << TEST_PIN)
#define TEST2_PIN    (1)
#define TEST2_MASK   (1 << TEST2_PIN)

// if test pins aren't required set this to zero.
#define TEST_PINS_OUTPUTS ((1<<(TEST2_PIN*3))|(1<<(TEST_PIN*3)))

#define Pi1MHz_MEM_PAGE  (1<<8)
#define Pi1MHz_MEM_RNW   (1<<9)

#define WRITE_FRED   0
#define WRITE_JIM                     Pi1MHz_MEM_PAGE
#define READ_FRED    Pi1MHz_MEM_RNW
#define READ_JIM    (Pi1MHz_MEM_RNW | Pi1MHz_MEM_PAGE)

#define CLEAR_IRQ 0
#define ASSERT_IRQ 1

#define CLEAR_NMI 0
#define ASSER_NMI 1

#define GET_DATA(gpio) ((gpio) & (DATABUS_MASK>>DATABUS_SHIFT))
#define GET_ADDR(gpio) (((gpio) & (ADDRBUS_MASK>>DATABUS_SHIFT)) >> (ADDRBUS_SHIFT-DATABUS_SHIFT))

#ifndef __ASSEMBLER__

#include <stdbool.h>
#include <stdint.h>
// JIM_ram_size is in 16Mbyte steps
extern uint32_t JIM_ram_size;

extern uint8_t * JIM_ram;

typedef void (*callback_func_ptr)( unsigned int);

void Pi1MHz_LED(int led);
void Pi1MHz_Register_Memory(int access, int addr, callback_func_ptr func_ptr );
void Pi1MHz_Register_Poll( func_ptr func_ptr );
void Pi1MHz_SetnIRQ(bool irq);
void Pi1MHz_SetnNMI(bool nmi);

extern uint8_t * const Pi1MHz_Memory;

bool Pi1MHz_is_rst_active();
#endif

#endif
