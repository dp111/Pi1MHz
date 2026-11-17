/*
 * Wired Ethernet through the USB Ethernet chip of a Pi 3 Model B+
 * (Microchip LAN7515, USB ID 0424:7800) - see docs/dev/ethernet-pi3-plan.md.
 *
 * A TinyUSB host class driver.  Everything runs in the main loop: the chip is
 * set up by non-blocking control transfers stepped from usb_ether_poll(), and
 * frames move on the bulk endpoints from the same loop.  lwIP's side is in
 * wifi/wifi_lwip.c, which uses this as the link when wifi.c chooses it.
 */
#ifndef USB_ETHER_H
#define USB_ETHER_H

#include <stdbool.h>
#include <stdint.h>

/* Stepped from the USB host work (usb.c), after tuh_task. */
void usb_ether_poll(void);

/* Whether the chip may be the link (wifi.c's choice).  Off - the default -
   it is not set up, or once running it stops reading the link and asking for
   frames, so a Pi on WiFi pays nothing for it. */
void usb_ether_use(bool wanted);

/* A supported chip is attached (it may still be setting up). */
bool usb_ether_present(void);
/* Set up: MAC programmed, receive and transmit running. */
bool usb_ether_ready(void);
/* The PHY reports a link (read about once a second). */
bool usb_ether_link_up(void);
/* The address the chip was given (the board's own, from the firmware). */
bool usb_ether_mac(uint8_t mac[6]);
/* Why setting the chip up failed, or "". */
const char *usb_ether_error(void);

/* Queue one Ethernet frame (no FCS: the chip adds it).  False when the
   transmit queue is full or the chip is not ready. */
bool usb_ether_send(const uint8_t *frame, uint16_t len);
/* The next received frame, FCS removed: *frame is valid until the next call.
   False when nothing is waiting. */
bool usb_ether_next_frame(const uint8_t **frame, uint16_t *len);

typedef struct {
   uint32_t tx_frames, tx_dropped, rx_frames, rx_errors, usb_errors;
} usb_ether_counts_t;
void usb_ether_counts(usb_ether_counts_t *out);

#endif
