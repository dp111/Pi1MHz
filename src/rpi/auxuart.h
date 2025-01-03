/*

 Part of the Raspberry-Pi Bare Metal Tutorials
 Copyright (c) 2013-2015, Brian Sidebotham
 All rights reserved.

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:

 1. Redistributions of source code must retain the above copyright notice,
 this list of conditions and the following disclaimer.

 2. Redistributions in binary form must reproduce the above copyright notice,
 this list of conditions and the following disclaimer in the
 documentation and/or other materials provided with the distribution.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 POSSIBILITY OF SUCH DAMAGE.

 */

#ifndef RPI_AUX_H
#define RPI_AUX_H

#include "base.h"

/* Although these values were originally from the BCM2835 Arm peripherals PDF
 it's clear that was rushed and has some glaring errors - so these values
 may appear to be different. These values have been changed due to data on
 the elinux BCM2835 datasheet errata:
 http://elinux.org/BCM2835_datasheet_errata */

#define AUX_BASE    ( PERIPHERAL_BASE + 0x215000 )

#define AUX_ENA_MINIUART            ( 1 << 0 )
#define AUX_ENA_SPI1                ( 1 << 1 )
#define AUX_ENA_SPI2                ( 1 << 2 )

#define AUX_IRQ_SPI2                ( 1 << 2 )
#define AUX_IRQ_SPI1                ( 1 << 1 )
#define AUX_IRQ_MU                  ( 1 << 0 )

#define AUX_MULCR_8BIT_MODE         ( 3 << 0 )  /* See errata for this value */
#define AUX_MULCR_BREAK             ( 1 << 6 )
#define AUX_MULCR_DLAB_ACCESS       ( 1 << 7 )

#define AUX_MUMCR_RTS               ( 1 << 1 )

#define AUX_MULSR_DATA_READY        ( 1 << 0 )
#define AUX_MULSR_RX_OVERRUN        ( 1 << 1 )
#define AUX_MULSR_TX_EMPTY          ( 1 << 5 )
#define AUX_MULSR_TX_IDLE           ( 1 << 6 )

#define AUX_MUMSR_CTS               ( 1 << 5 )

#define AUX_MUCNTL_RX_ENABLE        ( 1 << 0 )
#define AUX_MUCNTL_TX_ENABLE        ( 1 << 1 )
#define AUX_MUCNTL_RTS_FLOW         ( 1 << 2 )
#define AUX_MUCNTL_CTS_FLOW         ( 1 << 3 )
#define AUX_MUCNTL_RTS_FIFO         ( 3 << 4 )
#define AUX_MUCNTL_RTS_ASSERT       ( 1 << 6 )
#define AUX_MUCNTL_CTS_ASSERT       ( 1 << 7 )

#define AUX_MUSTAT_SYMBOL_AV        ( 1 << 0 )
#define AUX_MUSTAT_SPACE_AV         ( 1 << 1 )
#define AUX_MUSTAT_RX_IDLE          ( 1 << 2 )
#define AUX_MUSTAT_TX_IDLE          ( 1 << 3 )
#define AUX_MUSTAT_RX_OVERRUN       ( 1 << 4 )
#define AUX_MUSTAT_TX_FIFO_FULL     ( 1 << 5 )
#define AUX_MUSTAT_RTS              ( 1 << 6 )
#define AUX_MUSTAT_CTS              ( 1 << 7 )
#define AUX_MUSTAT_TX_EMPTY         ( 1 << 8 )
#define AUX_MUSTAT_TX_DONE          ( 1 << 9 )
#define AUX_MUSTAT_RX_FIFO_LEVEL    ( 7 << 16 )
#define AUX_MUSTAT_TX_FIFO_LEVEL    ( 7 << 24 )

/* Interrupt enables are incorrect on page 12 of:
      https://www.raspberrypi.org/wp-content/uploads/2012/02/BCM2835-ARM-Peripherals.pdf
  See errata:
      http://elinux.org/BCM2835_datasheet_errata#p12 */
#define AUX_MUIER_TX_INT            ( (1U << 1) )
#define AUX_MUIER_RX_INT            ( (1U << 0 ) | (1U << 2) )

#define AUX_MUIIR_INT_NOT_PENDING   ( 1U << 0 )
#define AUX_MUIIR_INT_IS_TX         ( 1U << 1 )
#define AUX_MUIIR_INT_IS_RX         ( 1U << 2 )

typedef struct
{
  rpi_reg_byte_ro_t IRQ;
  rpi_reg_byte_ro_t r1[3];
  rpi_reg_byte_rw_t ENABLES;
  rpi_reg_byte_ro_t r2[3];
  rpi_reg_ro_t reserved1[((0x40 - 0x04) / 4) - 1];

  rpi_reg_byte_rw_t MU_IO;
  rpi_reg_byte_ro_t r3[3];
  rpi_reg_byte_rw_t MU_IER;
  rpi_reg_byte_ro_t r4[3];
  rpi_reg_byte_rw_t MU_IIR;
  rpi_reg_byte_ro_t r5[3];
  rpi_reg_byte_rw_t MU_LCR;
  rpi_reg_byte_ro_t r6[3];
  rpi_reg_byte_rw_t MU_MCR;
  rpi_reg_byte_ro_t r7[3];
  rpi_reg_byte_ro_t MU_LSR;
  rpi_reg_byte_ro_t r8[3];
  rpi_reg_byte_ro_t MU_MSR;
  rpi_reg_byte_ro_t r9[3];
  rpi_reg_byte_rw_t MU_SCRATCH;
  rpi_reg_byte_ro_t r10[3];
  rpi_reg_byte_rw_t MU_CNTL;
  rpi_reg_byte_ro_t r11[3];
  rpi_reg_ro_t MU_STAT;
  rpi_reg_rw_t MU_BAUD;

  rpi_reg_ro_t reserved2[(0x80 - 0x68) / 4];

  rpi_reg_rw_t SPI0_CNTL0;
  rpi_reg_rw_t SPI0_CNTL1;
  rpi_reg_rw_t SPI0_STAT;
  rpi_reg_rw_t SPI0_IO;
  rpi_reg_ro_t SPI0_PEEK;

  rpi_reg_ro_t reserved3[(0xC0 - 0x94) / 4];

  rpi_reg_rw_t SPI1_CNTL0;
  rpi_reg_rw_t SPI1_CNTL1;
  rpi_reg_rw_t SPI1_STAT;
  rpi_reg_rw_t SPI1_IO;
  rpi_reg_ro_t SPI1_PEEK;
} aux_t;

extern void RPI_AuxMiniUartIRQHandler(void);
extern void RPI_AuxMiniUartInit(unsigned int baud);
extern void RPI_AuxMiniUartWrite(char c);
extern void RPI_AuxMiniUartWriteForce(char c);

static aux_t* const RPI_Aux = (aux_t*) AUX_BASE;

#endif
