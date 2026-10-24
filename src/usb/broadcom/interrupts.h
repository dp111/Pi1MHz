/*
 * Broadcom BCM283x interrupt management for TinyUSB
 *
 * This file provides interrupt control functions that map to the
 * existing Pi1MHz interrupt controller API.
 */

#ifndef BROADCOM_INTERRUPTS_H
#define BROADCOM_INTERRUPTS_H

#include <stdbool.h>
#include <stdint.h>
#include "../../rpi/interrupts.h"
#include "../../rpi/arm-start.h"
#include "defines.h"

#ifdef __cplusplus
extern "C" {
#endif

/* usb.c: the controller is a polled host (PI1MHZ_TUH_POLLED).  The ARM's USB IRQ then stays
   off whatever TinyUSB asks: IRQHandler_main neither services nor masks it in host mode, and
   TinyUSB's hcd_int_enable runs on every tuh_task (osal_spin_unlock) through upstream's
   dwc2_int_set, which enables it for either role. */
extern bool BP_usb_host_polled;

/**
 * @brief Enable a specific IRQ line
 * @param irqn IRQ number to enable (0-31 for IRQs_1, 32-63 for IRQs_2)
 */
static inline void BP_EnableIRQ(uint32_t irqn)
{
    if (irqn == USB_IRQn && BP_usb_host_polled)
        return;
    if (irqn < 32) {
        RPI_GetIrqController()->Enable_IRQs_1 = (1u << irqn);
    } else if (irqn < 64) {
        RPI_GetIrqController()->Enable_IRQs_2 = (1u << (irqn - 32));
    }
}

/**
 * @brief Disable a specific IRQ line
 * @param irqn IRQ number to disable (0-31 for IRQs_1, 32-63 for IRQs_2)
 */
static inline void BP_DisableIRQ(uint32_t irqn)
{
    if (irqn < 32) {
        RPI_GetIrqController()->Disable_IRQs_1 = (1u << irqn);
    } else if (irqn < 64) {
        RPI_GetIrqController()->Disable_IRQs_2 = (1u << (irqn - 32));
    }
}

/**
 * @brief Set priority for an IRQ (no-op on BCM283x as it has no priority levels)
 * @param irqn IRQ number
 * @param priority Priority level (ignored)
 */
static inline void BP_SetPriority(uint32_t irqn, uint32_t priority)
{
    (void)irqn;
    (void)priority;
    /* BCM283x does not support interrupt priorities */
}

/**
 * @brief Clear pending interrupt
 * @param irqn IRQ number
 */
static inline void BP_ClearPendingIRQ(uint32_t irqn)
{
    (void)irqn;
    /* On BCM283x, interrupts are cleared by servicing the peripheral */
}

/**
 * @brief Enable IRQs globally
 */
static inline void BP_EnableIRQs(void)
{
    _enable_interrupts();
}

/**
 * @brief Disable IRQs globally
 */
static inline void BP_DisableIRQs(void)
{
    _disable_interrupts();
}

#ifdef __cplusplus
}
#endif

#endif /* BROADCOM_INTERRUPTS_H */
