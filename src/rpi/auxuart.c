#include <stdio.h>
#include <stdlib.h>
#include "rpi.h"
#include "auxuart.h"
#include "gpio.h"
#include "arm-start.h"
#include "info.h"

#define USE_IRQ

#define RPI_TX_PIN 14
#define RPI_RX_PIN 15

#if defined(USE_IRQ)

#define TX_BUFFER_SIZE 65536  /* Must be a power of 2 */

NOINIT_SECTION static char tx_buffer[TX_BUFFER_SIZE];

#include "interrupts.h"

static volatile int tx_head;
static volatile int tx_tail;

static void __attribute__((interrupt("IRQ"))) RPI_AuxMiniUartIRQHandler() {

  _data_memory_barrier();
  while (1) {

    unsigned int iir = RPI_Aux->MU_IIR;

    if (iir & AUX_MUIIR_INT_NOT_PENDING) {
      /* No more interrupts */
      break;
    }

    /* Handle RxReady interrupt */
    if (iir & AUX_MUIIR_INT_IS_RX) {
#ifdef FORWARD_IRQ_CHARACTER
      /* Forward all received characters*/
      FORWARD_IRQ_CHARACTER(RPI_Aux->MU_IO);
#else
      /* Else just echo characters */
      /* RPI_AuxMiniUartWrite(RPI_Aux->MU_IO & 0xFF); */
      RPI_Aux->MU_IO; /* read char and dump to clear irq? */
#endif
    }

    /* Handle TxEmpty interrupt */
    if (iir & AUX_MUIIR_INT_IS_TX) {
      if (tx_tail != tx_head) {
         int temp = (tx_tail + 1) & (TX_BUFFER_SIZE - 1);
        /* Transmit the character */
         tx_tail = temp;
         RPI_Aux->MU_IO = tx_buffer[temp];
      } else {
        /* Disable TxEmpty interrupt */
         RPI_Aux->MU_IER &=  (uint8_t)~AUX_MUIER_TX_INT;
      }
    }
  }
  _data_memory_barrier();

}
#endif

void RPI_AuxMiniUartInit(unsigned int baud)
{
  /*
   Setup GPIO 14 and 15 as alternative function 5 which is
   UART 1 TXD/RXD. These need to be set before enabling the UART */
  RPI_SetGpioPinFunction(RPI_TX_PIN, FS_ALT5);
  RPI_SetGpioPinFunction(RPI_RX_PIN, FS_ALT5);

  /* Enable weak pullups */

  RPI_SetGpioPull(RPI_TX_PIN, PULL_UP);
  RPI_SetGpioPull(RPI_RX_PIN, PULL_UP);

  /* As this is a mini uart the configuration is complete! Now just
   enable the uart. Note from the documentation in section 2.1.1 of
   the ARM peripherals manual:

   If the enable bits are clear you will have no access to a
   peripheral. You can not even read or write the registers */
  RPI_Aux->ENABLES = AUX_ENA_MINIUART;

  /* Disable flow control,enable transmitter and receiver! */
  RPI_Aux->MU_CNTL = 0;

  /* eight-bit mode */
  RPI_Aux->MU_LCR = AUX_MULCR_8BIT_MODE;

  RPI_Aux->MU_MCR = 0;

  /* Disable all interrupts from MU and clear the fifos */
  RPI_Aux->MU_IER = 0;
  RPI_Aux->MU_IIR = 0xC6;

  unsigned int sys_freq = get_clock_rate(CORE_CLK_ID);

  /* Transposed calculation from Section 2.2.1 of the ARM peripherals manual */
  RPI_Aux->MU_BAUD = ( sys_freq / (8 * baud)) - 1;

#ifdef USE_IRQ
   {  extern int _interrupt_vector_h;
   tx_head = tx_tail = 0;
   *((unsigned int *) &_interrupt_vector_h ) = (unsigned int )&RPI_AuxMiniUartIRQHandler;

   RPI_IRQBase->Enable_IRQs_1 = (1 << 29);
   RPI_Aux->MU_IER |= AUX_MUIER_RX_INT;
   }
#endif

  /* Disable flow control, enable transmitter and receiver! */
  RPI_Aux->MU_CNTL = AUX_MUCNTL_TX_ENABLE | AUX_MUCNTL_RX_ENABLE;
}

void RPI_AuxMiniUartWrite(char c)
{
#ifdef USE_IRQ
  int tmp_head = (tx_head + 1) & (TX_BUFFER_SIZE - 1);

  /* Test if the buffer is full */
  if (tmp_head == tx_tail) {
     if (_get_cpsr() & 0x80) {
        /* IRQ disabled: drop the character to avoid deadlock */
        return;
     } else {
        /* IRQ enabled: wait for space in buffer */
        while (tmp_head == tx_tail) {
        }
     }
  }
  /* Buffer the character */
  tx_buffer[tmp_head] = c;
  tx_head = tmp_head;

  /* Enable TxEmpty interrupt */
  RPI_Aux->MU_IER |= AUX_MUIER_TX_INT;

#else
  /* Wait until the UART has an empty space in the FIFO */
  while ((RPI_Aux->MU_LSR & AUX_MULSR_TX_EMPTY) == 0)
  {
  }
  /* Write the character to the FIFO for transmission */
  RPI_Aux->MU_IO = c;
#endif
}

void RPI_AuxMiniUartWriteForce(char c)
{
  /* Wait until the UART has an empty space in the FIFO */
  while ((RPI_Aux->MU_LSR & AUX_MULSR_TX_EMPTY) == 0)
  {
  }
  /* Write the character to the FIFO for transmission */
  RPI_Aux->MU_IO = c;
}