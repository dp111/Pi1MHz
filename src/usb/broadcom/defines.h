/*
 * Broadcom BCM283x USB OTG definitions for TinyUSB
 *
 * This file provides USB peripheral base addresses and interrupt numbers
 * for the Broadcom BCM2835/2836/2837 SoCs used in Raspberry Pi.
 */

#ifndef BROADCOM_DEFINES_H
#define BROADCOM_DEFINES_H

#include "../../rpi/base.h"
#include "../../rpi/arm-start.h"

#ifdef __cplusplus
extern "C" {
#endif

/* USB OTG Controller Base Address */
#if (__ARM_ARCH >= 7)
    /* BCM2836/2837 (Pi 2/3) */
    #define USB_OTG_GLOBAL_BASE     (PERIPHERAL_BASE + 0x980000UL)
#else
    /* BCM2835 (Pi Zero/1) */
    #define USB_OTG_GLOBAL_BASE     (PERIPHERAL_BASE + 0x980000UL)
#endif

/* USB Interrupt Number */
#define USB_IRQn                    9

/* Memory barrier macro */
#define COMPLETE_MEMORY_READS       _data_memory_barrier()

#ifdef __cplusplus
}
#endif

#endif /* BROADCOM_DEFINES_H */
