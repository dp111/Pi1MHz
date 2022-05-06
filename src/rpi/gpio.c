#include <stdint.h>
#include "gpio.h"
#include "systimer.h"

void RPI_SetGpioPinFunction(rpi_gpio_pin_t gpio, rpi_gpio_alt_function_t func)
{
  rpi_reg_rw_t* fsel_reg = &RPI_GpioBase->GPFSEL[gpio / 10];

  uint32_t fsel_copy = *fsel_reg;
  fsel_copy &= (uint32_t)~(FS_MASK << ((gpio % 10) * 3));
  fsel_copy |= (func << ((gpio % 10) * 3));
  *fsel_reg = fsel_copy;
}

rpi_gpio_alt_function_t RPI_GetGpioPinFunction(rpi_gpio_pin_t gpio)
{
  rpi_reg_rw_t* fsel_reg = &RPI_GpioBase->GPFSEL[gpio / 10];

  uint32_t fsel_copy = *fsel_reg;
  fsel_copy >>= ((gpio % 10) * 3);
  fsel_copy &= FS_MASK;

  return fsel_copy;
}

void RPI_SetGpioOutput(rpi_gpio_pin_t gpio)
{
  RPI_SetGpioPinFunction(gpio, FS_OUTPUT);
}

void RPI_SetGpioInput(rpi_gpio_pin_t gpio)
{
  RPI_SetGpioPinFunction(gpio, FS_INPUT);
}

rpi_gpio_value_t RPI_GetGpioValue(rpi_gpio_pin_t gpio)
{
  switch (gpio / 32)
  {
    case 0:
      if (RPI_GpioBase->GPLEV0 >> gpio & 1)
         return RPI_IO_HI;
      else
         return RPI_IO_LO;

    case 1:
      if (RPI_GpioBase->GPLEV1 >> (gpio - 32) & 1)
         return RPI_IO_HI;
      else
         return RPI_IO_LO;

    default:
      return RPI_IO_UNKNOWN;
  }
}

void RPI_ToggleGpio(rpi_gpio_pin_t gpio)
{
  if (RPI_GetGpioValue(gpio) == RPI_IO_HI)
    RPI_SetGpioLo(gpio);
  else
    RPI_SetGpioHi(gpio);
}

void RPI_SetGpioHi(rpi_gpio_pin_t gpio)
{
  switch (gpio / 32)
  {
    case 0:
      RPI_GpioBase->GPSET0 = (1 << gpio);
    break;

    case 1:
      RPI_GpioBase->GPSET1 = (1 << (gpio - 32));
    break;

    default:
    break;
  }
}

void RPI_SetGpioLo(rpi_gpio_pin_t gpio)
{
  switch (gpio / 32)
  {
    case 0:
      RPI_GpioBase->GPCLR0 = (1 << gpio);
    break;

    case 1:
      RPI_GpioBase->GPCLR1 = (1 << (gpio - 32));
    break;

    default:
    break;
  }
}

void RPI_SetGpioValue(rpi_gpio_pin_t gpio, rpi_gpio_value_t value)
{
  if ((value == RPI_IO_LO) || (value == RPI_IO_OFF))
    RPI_SetGpioLo(gpio);
  else if ((value == RPI_IO_HI) || (value == RPI_IO_ON))
    RPI_SetGpioHi(gpio);
}


void RPI_SetPullUps(unsigned int gpio)
{
  /* Enable weak pullups */
  RPI_GpioBase->GPPUD = 2;
  RPI_WaitMicroSeconds(2); /* wait of 150 cycles needed see datasheet */

  RPI_GpioBase->GPPUDCLK0 = gpio;
  RPI_WaitMicroSeconds(2); /* wait of 150 cycles needed see datasheet */

  RPI_GpioBase->GPPUDCLK0 =  0;
}
