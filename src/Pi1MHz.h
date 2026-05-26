// Pi1MHz.h

#ifndef Pi1MHz_H
#define Pi1MHz_H

#include "rpi/rpi.h"
#include "rpi/base.h"

#define RELEASENAME "v1.17"

#define PAGE_SIZE    0x100

#define Pi1MHz_MEM_BASE_GPU (PERIPHERAL_BASE_GPU | (Pi1MHz_MEM_BASE & 0x00FFFFFF) )

#define Pi1MHz_MEM_BASE ( PERIPHERAL_BASE + 0x9A0000+(1024*15))

#define Pi1MHz_VPU_RETURN (PERIPHERAL_BASE + 0x600010 )

#define Pi1MHZ_MEM_SIZE (PAGE_SIZE*2)
#define Pi1MHz_CB_BASE  0x400
#define Pi1MHz_CB_SIZE  (PAGE_SIZE*2*2*4)

/* Raspberry Pi Pinout Bottom view

      +5v  Pin 2  o o Pin 1  +3.3v
      +5v  Pin 4  o o Pin 3  IO2  D0
      GND  Pin 6  o o Pin 5  IO3  D1
   TX IO14 Pin 8  o o Pin 7  IO4  D2
   RX IO15 Pin 10 o o Pin 9  GND
   A2 IO18 Pin 12 o o Pin 11 IO17 A1
      GND  Pin 14 o o Pin 13 IO27 CLK1MHZ
   A7 IO23 Pin 16 o o Pin 15 IO22 A6
nPCFC IO24 Pin 18 o o Pin 17 +3.3v
      GND  Pin 20 o o Pin 19 IO10 RnW
nPCFD IO25 Pin 22 o o Pin 21 IO9  D7
   D6 IO8  Pin 24 o o Pin 23 IO11 nNMI
   D5 IO7  Pin 26 o o Pin 25 GND
      IO1  Pin 28 o o Pin 27 IO0  TEST_PIN
      GND  Pin 30 o o Pin 29 IO5  D3
 nIRQ IO12 Pin 32 o o Pin 31 IO6  D4
      GND  Pin 34 o o Pin 33 IO13 PWM
   A0 IO16 Pin 36 o o Pin 35 1O19 A3
   A4 IO20 Pin 38 o o Pin 37 IO26 nRST
   A5 IO21 Pin 40 o o Pin 39 GND
*/

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
//    A3 - Pin 35 – GPIO19
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

#define NOE_PIN      (0)

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

#define TEST_PIN    (1)
#define TEST_MASK   (1 << TEST_PIN)

// if test pins aren't required set this to zero.
#define TEST_PINS_OUTPUTS (1<<(TEST_PIN*3))

#define Pi1MHz_MEM_PAGE  (1<<8)
#define Pi1MHz_MEM_RNW   (1<<9)

#define WRITE_FRED   0
#define WRITE_JIM                     Pi1MHz_MEM_PAGE
#define READ_FRED    Pi1MHz_MEM_RNW
#define READ_JIM    (Pi1MHz_MEM_RNW | Pi1MHz_MEM_PAGE)

#define CLEAR_IRQ 0
#define ASSERT_IRQ 1

#define CLEAR_NMI 0
#define ASSERT_NMI 1

#define GET_DATA(gpio) ((gpio) & (DATABUS_MASK>>DATABUS_SHIFT))
#define GET_ADDR(gpio) (((gpio) & (ADDRBUS_MASK>>DATABUS_SHIFT)) >> (ADDRBUS_SHIFT-DATABUS_SHIFT))

#ifndef __ASSEMBLER__

#include <stdbool.h>
#include <stdint.h>


extern uint8_t fx_register[256];

typedef void (*callback_func_ptr)( unsigned int);
typedef void (*func_ptr_parameter)( uint8_t instance, uint8_t address);

typedef struct
{
   uint8_t Memory[PAGE_SIZE*2];
   uint8_t * JIM_ram;
   size_t page_ram_addr;
   size_t byte_ram_addr;
   uint8_t JIM_ram_size; // Pi1MHz->JIM_ram_size is in 16Mbyte steps

   uint8_t Spare[PAGE_SIZE-13];
   callback_func_ptr callback_table[PAGE_SIZE*2*2];
} Pi1MHz_t;

static Pi1MHz_t * const Pi1MHz = (Pi1MHz_t *) 0x100;

#define DISC_RAM_BASE ((uint32_t)( (Pi1MHz->JIM_ram_size - 2) * 16 * 1024 * 1024 ))

void Pi1MHz_LED(int led);
void Pi1MHz_Register_Memory(int access, uint8_t addr, callback_func_ptr function_ptr );
void Pi1MHz_Register_Poll( func_ptr function_ptr );
void Pi1MHz_SetnIRQ(bool irq);
void Pi1MHz_SetnNMI(bool nmi);

void Pi1MHz_MemoryWrite(uint32_t addr, uint8_t data);
void Pi1MHz_MemoryWrite16(uint32_t addr, uint32_t data);
void Pi1MHz_MemoryWrite32(uint32_t addr, uint32_t data);
uint8_t Pi1MHz_MemoryRead(uint32_t addr);

bool Pi1MHz_is_rst_active();

// This is an assembler function for performance
extern void Pi1MHz_MemoryWritePage(uint32_t addr, uint32_t * data);
extern void _fast_scroll(void *dst, void *src, int num_bytes);
#endif

#endif
