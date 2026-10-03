/* usb_mouse.c - a USB mouse on the Pi's port in host mode.

   TinyUSB's HID host class hands over each report from the main loop
   (tuh_task); the movement builds up here until the Beeb takes it through
   usb_mouse_latch, called in FIQ when it reads &FCAF.  The mouse is asked for
   its boot protocol report - buttons, X, Y - so any mouse works without
   parsing its report descriptor.  See usb_mouse.h for the register layout. */
#include <stdio.h>

#include "usb/tusb_config.h"
#include <tusb.h>

#include "usb_mouse.h"
#include "rpi/asm-helpers.h"

#define MOVE_MAX 8191                /* 14-bit two's complement */

/* Built up by the main loop, taken by the FIQ: the main loop changes them
   only with interrupts (FIQ included) off. */
static int32_t  s_dx, s_dy;
static uint8_t  s_buttons;           /* HID: bit 0 left, 1 right, 2 middle */
static bool     s_present;
static uint8_t  s_addr, s_instance;  /* the mouse being read */
static uint16_t s_vid, s_pid;
static uint32_t s_reports;

static int32_t clamp(int32_t v)
{
   return v > MOVE_MAX ? MOVE_MAX : v < -MOVE_MAX - 1 ? -MOVE_MAX - 1 : v;
}

void usb_mouse_latch(uint8_t out[4])
{
   int32_t dx = clamp(s_dx), dy = clamp(s_dy);
   s_dx -= dx;
   s_dy -= dy;
   out[0] = (uint8_t)dx;
   out[1] = (uint8_t)(((uint32_t)dx >> 8) & 0x3Fu) | ((s_buttons & 1u) ? 0x40u : 0u)
                                                   | ((s_buttons & 2u) ? 0x80u : 0u);
   out[2] = (uint8_t)dy;
   out[3] = (uint8_t)(((uint32_t)dy >> 8) & 0x3Fu) | ((s_buttons & 4u) ? 0x40u : 0u)
                                                   | (s_present ? 0x80u : 0u);
}

void usb_mouse_status(char *buf, size_t len)
{
   if (!s_present)
      snprintf(buf, len, "none");
   else
      snprintf(buf, len, "%04x:%04x reports %lu buttons %u",
               (unsigned)s_vid, (unsigned)s_pid, (unsigned long)s_reports,
               (unsigned)s_buttons);
}

/* ---- TinyUSB HID host callbacks (main loop, from tuh_task) -------------- */

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                      uint8_t const *desc_report, uint16_t desc_len)
{
   (void)desc_report;
   (void)desc_len;
   if (s_present || tuh_hid_interface_protocol(dev_addr, instance) != HID_ITF_PROTOCOL_MOUSE)
      return;                          /* one mouse; keyboards and the rest ignored */
   s_addr = dev_addr;
   s_instance = instance;
   (void)tuh_vid_pid_get(dev_addr, &s_vid, &s_pid);
   s_reports = 0u;
   unsigned int cpsr = _disable_interrupts_cspr();
   s_dx = s_dy = 0;
   s_buttons = 0u;
   s_present = true;
   _restore_cpsr(cpsr);
   (void)tuh_hid_receive_report(dev_addr, instance);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance)
{
   if (!s_present || dev_addr != s_addr || instance != s_instance)
      return;
   unsigned int cpsr = _disable_interrupts_cspr();
   s_present = false;
   s_buttons = 0u;
   s_dx = s_dy = 0;
   _restore_cpsr(cpsr);
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                uint8_t const *report, uint16_t len)
{
   if (s_present && dev_addr == s_addr && instance == s_instance && len >= 3u) {
      /* Boot protocol: buttons, X, Y (signed), then maybe a wheel. */
      int32_t dx = (int8_t)report[1];
      int32_t dy = (int8_t)report[2];
      unsigned int cpsr = _disable_interrupts_cspr();
      /* Never more built up than one read can carry: before *MOUSE nothing
         reads it, and the pointer would otherwise fly off on the first. */
      s_dx = clamp(s_dx + dx);
      s_dy = clamp(s_dy - dy);         /* the mouse's down is the Beeb's up */
      s_buttons = report[0] & 7u;
      _restore_cpsr(cpsr);
      s_reports++;
   }
   (void)tuh_hid_receive_report(dev_addr, instance);
}
