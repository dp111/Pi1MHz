/* Host tests for usb_mouse.c: what the Beeb reads at &FCAC-&FCAF.
 *
 * The movement is handed over in whole steps of 4 and the rest is kept for
 * the next read.  "The rest" has to be what lies between the step and zero
 * in both directions: rounding a negative build-up down instead turns a
 * mouse's at-rest jitter of -1 into a 4-unit jump left or down.  Also the
 * 14-bit clamp and packing, the reset when the mouse goes, and taking up a
 * second mouse when the one being read is unplugged.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "usb/tusb_config.h"
#include <tusb.h>
#include "usb_mouse.h"

/* The TinyUSB callbacks under test (declared by hid_host.h in the build). */
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                      uint8_t const *desc_report, uint16_t desc_len);
void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance);
void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                uint8_t const *report, uint16_t len);

/* ---- the stub HID host ------------------------------------------------- */

stub_hid_itf_t stub_hid_itf[CFG_TUH_HID];

static stub_hid_itf_t *itf(uint8_t daddr, uint8_t idx)
{
   if (daddr == 0u || idx >= CFG_TUH_HID || stub_hid_itf[idx].daddr != daddr)
      return NULL;
   return &stub_hid_itf[idx];
}

uint8_t tuh_hid_interface_protocol(uint8_t dev_addr, uint8_t idx)
{
   stub_hid_itf_t *p = itf(dev_addr, idx);
   return p ? p->protocol : 0u;
}

bool tuh_hid_mounted(uint8_t dev_addr, uint8_t idx)
{
   stub_hid_itf_t *p = itf(dev_addr, idx);
   return p && p->mounted;
}

bool tuh_hid_receive_report(uint8_t dev_addr, uint8_t idx)
{
   stub_hid_itf_t *p = itf(dev_addr, idx);
   if (p == NULL)
      return false;
   p->receives++;
   return true;
}

bool tuh_vid_pid_get(uint8_t dev_addr, uint16_t *vid, uint16_t *pid)
{
   *vid = 0x046d;
   *pid = (uint16_t)(0xc000u + dev_addr);
   return true;
}

/* As hid_host.c: the slot is filled and marked mounted, then the callback. */
static void plug(uint8_t daddr, uint8_t idx, uint8_t protocol)
{
   stub_hid_itf[idx] = (stub_hid_itf_t){ daddr, protocol, true, 0 };
   tuh_hid_mount_cb(daddr, idx, NULL, 0u);
}

/* As hidh_close: the callback runs while the slot still holds the device,
   and the slot is cleared after it. */
static void unplug(uint8_t daddr)
{
   for (uint8_t i = 0; i < CFG_TUH_HID; i++)
      if (stub_hid_itf[i].daddr == daddr) {
         tuh_hid_umount_cb(daddr, i);
         memset(&stub_hid_itf[i], 0, sizeof stub_hid_itf[i]);
      }
}

/* ---- helpers ----------------------------------------------------------- */

static int failures, checks;

#define CHECK(cond, ...) do { \
      checks++; \
      if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                     printf(__VA_ARGS__); printf("\n"); } \
   } while (0)

/* Mouse report: buttons, X right, Y down (boot protocol). */
static void move(uint8_t daddr, uint8_t idx, int x, int y, uint8_t buttons)
{
   uint8_t r[3] = { buttons, (uint8_t)(int8_t)x, (uint8_t)(int8_t)y };
   tuh_hid_report_received_cb(daddr, idx, r, sizeof r);
}

typedef struct { int dx, dy; bool left, right, middle, present; } latched_t;

static int32_t sext14(uint32_t v) { return (int32_t)(v << 18) >> 18; }

static latched_t latch(void)
{
   uint8_t o[4];
   usb_mouse_latch(o);
   latched_t l;
   l.dx = sext14((uint32_t)o[0] | ((uint32_t)(o[1] & 0x3Fu) << 8));
   l.dy = sext14((uint32_t)o[2] | ((uint32_t)(o[3] & 0x3Fu) << 8));
   l.left = (o[1] & 0x40u) != 0u;
   l.right = (o[1] & 0x80u) != 0u;
   l.middle = (o[3] & 0x40u) != 0u;
   l.present = (o[3] & 0x80u) != 0u;
   return l;
}

/* ---- tests ------------------------------------------------------------- */

/* A build-up of v in X (and -v in Y, the mouse's down being the Beeb's up)
   is read as want, and then, after a further fill of `then`, as want2. */
static void step_case(int v, int want, int then, int want2)
{
   plug(1, 0, HID_ITF_PROTOCOL_MOUSE);
   move(1, 0, v, -v, 0);
   latched_t l = latch();
   CHECK(l.dx == want && l.dy == want, "build-up %d: read X %d Y %d, want %d", v, l.dx, l.dy, want);
   move(1, 0, then, -then, 0);
   l = latch();
   CHECK(l.dx == want2 && l.dy == want2, "build-up %d then %d: read X %d Y %d, want %d",
         v, then, l.dx, l.dy, want2);
   unplug(1);
}

static void test_steps(void)
{
   /* Below one step either way: nothing sent, all of it kept. */
   step_case( 1,  0,  3,  4);
   step_case(-1,  0, -3, -4);
   step_case( 3,  0,  1,  4);
   step_case(-3,  0, -1, -4);
   /* Exactly one step. */
   step_case( 4,  4,  0,  0);
   step_case(-4, -4,  0,  0);
   /* One step and one over: the one stays on the same side of zero. */
   step_case( 5,  4,  3,  4);
   step_case(-5, -4, -3, -4);
   /* ...so jitter around rest sends nothing in either direction. */
   step_case(-1,  0,  1,  0);
   step_case( 1,  0, -1,  0);
}

static void test_clamp(void)
{
   plug(1, 0, HID_ITF_PROTOCOL_MOUSE);
   for (int i = 0; i < 100; i++)
      move(1, 0, 127, -127, 0);      /* 12700 > 8191 */
   latched_t l = latch();
   CHECK(l.dx == 8188 && l.dy == 8188, "clamp +: X %d Y %d, want 8188", l.dx, l.dy);
   move(1, 0, 1, -1, 0);             /* the 3 left over, plus 1 */
   l = latch();
   CHECK(l.dx == 4 && l.dy == 4, "clamp + remainder: X %d Y %d, want 4", l.dx, l.dy);
   unplug(1);

   plug(1, 0, HID_ITF_PROTOCOL_MOUSE);
   for (int i = 0; i < 100; i++)
      move(1, 0, -128, 127, 0);
   l = latch();
   CHECK(l.dx == -8192 && l.dy == -8192, "clamp -: X %d Y %d, want -8192", l.dx, l.dy);
   l = latch();
   CHECK(l.dx == 0 && l.dy == 0, "clamp - remainder: X %d Y %d, want 0", l.dx, l.dy);
   unplug(1);
}

static void test_packing(void)
{
   uint8_t o[4];

   plug(1, 0, HID_ITF_PROTOCOL_MOUSE);
   move(1, 0, -8, 12, 1);            /* left; X -8, Y -12 */
   usb_mouse_latch(o);
   CHECK(o[0] == 0xF8 && o[1] == (0x3F | 0x40), "pack X -8 + left: %02X %02X", o[0], o[1]);
   CHECK(o[2] == 0xF4 && o[3] == (0x3F | 0x80), "pack Y -12 + present: %02X %02X", o[2], o[3]);

   move(1, 0, 0, 0, 2 | 4);          /* right and middle, no movement */
   usb_mouse_latch(o);
   CHECK(o[0] == 0 && o[1] == 0x80, "pack right: %02X %02X", o[0], o[1]);
   CHECK(o[2] == 0 && o[3] == (0x40 | 0x80), "pack middle + present: %02X %02X", o[2], o[3]);

   for (int i = 0; i < 100; i++)
      move(1, 0, 127, 0, 0);
   usb_mouse_latch(o);               /* +8188 = 0x1FFC: bits 8-13 are 0x1F */
   CHECK(o[0] == 0xFC && o[1] == 0x1F, "pack X +8188: %02X %02X", o[0], o[1]);
   for (int i = 0; i < 100; i++)
      move(1, 0, -128, 0, 0);
   usb_mouse_latch(o);               /* the 3 left + clamp = -8192 = 0x2000 */
   CHECK(o[0] == 0x00 && o[1] == 0x20, "pack X -8192: %02X %02X", o[0], o[1]);
   unplug(1);
}

static void test_unmount_resets(void)
{
   plug(1, 0, HID_ITF_PROTOCOL_MOUSE);
   move(1, 0, 6, -10, 7);
   unplug(1);
   latched_t l = latch();
   CHECK(l.dx == 0 && l.dy == 0 && !l.left && !l.right && !l.middle && !l.present,
         "after unplug: X %d Y %d L%d R%d M%d P%d", l.dx, l.dy, l.left, l.right, l.middle, l.present);

   /* A keyboard is not taken for a mouse. */
   plug(2, 1, HID_ITF_PROTOCOL_KEYBOARD);
   CHECK(!latch().present, "a keyboard was taken for a mouse");
   unplug(2);

   /* Nothing carried over to the next mouse. */
   plug(1, 0, HID_ITF_PROTOCOL_MOUSE);
   l = latch();
   CHECK(l.present && l.dx == 0 && l.dy == 0 && !l.left, "fresh mouse: P%d X %d Y %d", l.present, l.dx, l.dy);
   unplug(1);
}

static void test_second_mouse(void)
{
   plug(1, 0, HID_ITF_PROTOCOL_MOUSE);
   plug(2, 1, HID_ITF_PROTOCOL_MOUSE);   /* while the first is read: ignored */
   move(2, 1, 8, 0, 0);
   latched_t l = latch();
   CHECK(l.dx == 0, "second mouse moved the pointer while the first was read: X %d", l.dx);

   stub_hid_itf[1].receives = 0;
   unplug(1);                             /* the first goes: the second is taken up */
   l = latch();
   CHECK(l.present, "second mouse not taken up after the first was unplugged");
   CHECK(stub_hid_itf[1].receives > 0, "second mouse never asked for a report");
   move(2, 1, 8, -4, 1);
   l = latch();
   CHECK(l.dx == 8 && l.dy == 4 && l.left, "second mouse: X %d Y %d L%d, want 8 4 1", l.dx, l.dy, l.left);

   unplug(2);
   CHECK(!latch().present, "present after the last mouse went");
}

int main(void)
{
   test_steps();
   test_clamp();
   test_packing();
   test_unmount_resets();
   test_second_mouse();
   printf("%d checks, %d failed\n", checks, failures);
   if (failures == 0)
      printf("USB MOUSE TESTS PASSED\n");
   return failures ? 1 : 0;
}
