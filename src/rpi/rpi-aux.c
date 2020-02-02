#include <stdio.h>
#include <stdlib.h>
#include "rpi.h"
#include "rpi-aux.h"
#include "rpi-gpio.h"
#include "arm-start.h"
#include "info.h"

#define USE_IRQ

#define RPI_TX_PIN 14
#define RPI_RX_PIN 15

#if defined(USE_IRQ)

#define TX_BUFFER_SIZE 65536  /* Must be a power of 2 */

#include "rpi-interrupts.h"

static char *tx_buffer;
static volatile int tx_head;
static volatile int tx_tail;

static void __attribute__((interrupt("IRQ"))) RPI_AuxMiniUartIRQHandler() {

  _data_memory_barrier();
  while (1) {

    int iir = RPI_Aux->MU_IIR;

    if (iir & AUX_MUIIR_INT_NOT_PENDING) {
      /* No more interrupts */
      break;
    }

    /* Handle RxReady interrupt */
    if (iir & AUX_MUIIR_INT_IS_RX) {
#if 0
      /* Forward all received characters to the debugger */
      debugger_rx_char(RPI_Aux->MU_IO & 0xFF);
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
         RPI_Aux->MU_IER &= ~AUX_MUIER_TX_INT;
      }
    }
  }
  _data_memory_barrier();

}
#endif

void RPI_AuxMiniUartInit(int baud)
{
  /* Data memory barrier need to be places between accesses to different peripherals

     See page 7 of the BCM2853 manual

   Setup GPIO 14 and 15 as alternative function 5 which is
   UART 1 TXD/RXD. These need to be set before enabling the UART */
  RPI_SetGpioPinFunction(RPI_TX_PIN, FS_ALT5);
  RPI_SetGpioPinFunction(RPI_RX_PIN, FS_ALT5);

  /* Enable weak pullups */

  RPI_SetPullUps((1u << RPI_TX_PIN) | (1u << RPI_RX_PIN));

  _data_memory_barrier();

  /* As this is a mini uart the configuration is complete! Now just
   enable the uart. Note from the documentation in section 2.1.1 of
   the ARM peripherals manual:

   If the enable bits are clear you will have no access to a
   peripheral. You can not even read or write the registers */
  RPI_Aux->ENABLES = AUX_ENA_MINIUART;

  _data_memory_barrier();

  /* Disable flow control,enable transmitter and receiver! */
  RPI_Aux->MU_CNTL = 0;

  /* eight-bit mode */
  RPI_Aux->MU_LCR = AUX_MULCR_8BIT_MODE;

  RPI_Aux->MU_MCR = 0;

  /* Disable all interrupts from MU and clear the fifos */
  RPI_Aux->MU_IER = 0;
  RPI_Aux->MU_IIR = 0xC6;

  int sys_freq = get_clock_rate(CORE_CLK_ID);

  _data_memory_barrier();
  /* Transposed calculation from Section 2.2.1 of the ARM peripherals manual */
  RPI_Aux->MU_BAUD = ( sys_freq / (8 * baud)) - 1;

#ifdef USE_IRQ
   {  extern int _interrupt_vector_h;
   tx_buffer = malloc(TX_BUFFER_SIZE);
   tx_head = tx_tail = 0;
   *((unsigned int *) &_interrupt_vector_h ) = (unsigned int )&RPI_AuxMiniUartIRQHandler;
   _data_memory_barrier();

   RPI_IRQBase->Enable_IRQs_1 = (1 << 29);
   _data_memory_barrier();
   RPI_Aux->MU_IER |= AUX_MUIER_RX_INT;
   }
#endif

  /* Disable flow control,enable transmitter and receiver! */
  RPI_Aux->MU_CNTL = AUX_MUCNTL_TX_ENABLE | AUX_MUCNTL_RX_ENABLE;

  _data_memory_barrier();
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

  _data_memory_barrier();

  /* Enable TxEmpty interrupt */
  RPI_Aux->MU_IER |= AUX_MUIER_TX_INT;

  _data_memory_barrier();

#else
  _data_memory_barrier();
  /* Wait until the UART has an empty space in the FIFO */
  while ((RPI_Aux->MU_LSR & AUX_MULSR_TX_EMPTY) == 0)
  {
  }
  /* Write the character to the FIFO for transmission */
  RPI_Aux->MU_IO = c;
  _data_memory_barrier();
#endif
}
