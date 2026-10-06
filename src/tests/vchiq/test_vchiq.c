/* Host tests for rpi/vchiq.c: the kernel.now hand-over of the VCHIQ
 * connection, and the close handshake it relies on.
 *
 * On hardware the VideoCore is not reset by a kernel.now and ignores a
 * second TAG_VCHIQ_INIT: the incoming kernel's CONNECT is never answered
 * (recorded in docs/dev/h264-hardware-decode.md - a hardware observation,
 * not re-measured here).  So the outgoing kernel closes its services and
 * hands the live connection on (vchiq_handover), and the incoming one takes
 * it over (vchiq_adopt + vchiq_init) without a word to the VideoCore.  What
 * can go wrong is ARM-side bookkeeping: the stream positions, the slot
 * queues, the ports.  That is what is pinned here, against a simulated
 * VideoCore that speaks the same slot protocol from shared memory and
 * behaves as the hardware was seen to: one INIT, the rest ignored.
 *
 * Each "kernel" is a forked child with vchiq.c's statics fresh, as after a
 * real jump; the shared block and the simulated VideoCore live in one
 * MAP_SHARED mapping below 4 GB (vchiq.c keeps addresses in 32 bits), so
 * they outlive each kernel just as GPU memory and the VideoCore do.  The
 * file is included, not linked: the test reads the client's wire structs.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "vchiq.c"

/* ---- the simulated VideoCore (master), in shared memory ------------------ */

#define ARENA_SIZE   (4u * 1024u * 1024u)
#define VC_SERVICES  8
#define VC_PORT0     100u                /* the VC's own port numbers */

typedef struct {
   uint8_t *block;                    /* the slot memory it serves; NULL until INIT */
   int      inits;                    /* TAG_VCHIQ_INITs seen */
   int32_t  rx_pos;                   /* its read position in our stream */
   bool     connected;
   bool     ignore_close;             /* a VC that never answers a CLOSE */
   struct { bool open; uint32_t armport; } svc[VC_SERVICES];
   int      closes_seen;              /* CLOSEs from us */
   int      closes_sent;              /* CLOSEs to us (answers) */
   uint32_t alloc_next;               /* bump allocator over the arena */
   uint32_t result;                   /* what a kernel hands the next */
} sim_t;

uint8_t fake_periph[0x10000];
static uint8_t *arena;
static sim_t   *sim;
static uint32_t now_us;

static uint8_t *vc_slot(int32_t i) { return sim->block + (uint32_t)i * VCHIQ_SLOT_SIZE; }
static vchiq_slot_zero_t *vc_zero(void) { return (vchiq_slot_zero_t *)sim->block; }

/* The VC sends: pads to a new slot when needed, takes a slot from its own
   queue (the ones we recycled back), publishes tx_pos. */
static void vc_send(uint32_t msgid, const void *payload, uint32_t size)
{
   vchiq_shared_state_t *m = &vc_zero()->master;
   uint32_t stride = CALC_STRIDE(size);
   uint32_t pos = (uint32_t)m->tx_pos;
   uint32_t space = VCHIQ_SLOT_SIZE - (pos & VCHIQ_SLOT_MASK);
   if (stride > space) {
      vchiq_header_t *pad = (vchiq_header_t *)(vc_slot(m->slot_queue[(pos / VCHIQ_SLOT_SIZE) & VCHIQ_SLOT_QUEUE_MASK])
                                               + (pos & VCHIQ_SLOT_MASK));
      pad->size = space - VCHIQ_HEADER_SIZE;
      pad->msgid = (int32_t)VCHIQ_MAKE_MSG(VCHIQ_MSG_PADDING, 0, 0);
      pos += space;
   }
   if ((pos & VCHIQ_SLOT_MASK) == 0 && pos == (uint32_t)m->slot_queue_recycle * VCHIQ_SLOT_SIZE) {
      printf("FAIL: simulated VC out of slots - the ARM is not recycling\n");
      exit(99);
   }
   vchiq_header_t *h = (vchiq_header_t *)(vc_slot(m->slot_queue[(pos / VCHIQ_SLOT_SIZE) & VCHIQ_SLOT_QUEUE_MASK])
                                          + (pos & VCHIQ_SLOT_MASK));
   h->size = size;
   if (size)
      memcpy((uint8_t *)h + VCHIQ_HEADER_SIZE, payload, size);
   h->msgid = (int32_t)msgid;
   __sync_synchronize();
   m->tx_pos = (int32_t)(pos + stride);
}

/* The VC reads our stream and answers, recycling our slots as it goes. */
static void vc_step(void)
{
   if (!sim || !sim->block)
      return;
   vchiq_slot_zero_t *z = vc_zero();
   while (sim->rx_pos != z->slave.tx_pos) {
      uint32_t pos = (uint32_t)sim->rx_pos;
      int32_t idx = z->slave.slot_queue[(pos / VCHIQ_SLOT_SIZE) & VCHIQ_SLOT_QUEUE_MASK];
      vchiq_header_t *h = (vchiq_header_t *)(vc_slot(idx) + (pos & VCHIQ_SLOT_MASK));
      uint32_t msgid = (uint32_t)h->msgid, size = h->size;
      uint32_t type = VCHIQ_MSG_TYPE(msgid);
      uint32_t src = VCHIQ_MSG_SRCPORT(msgid), dst = VCHIQ_MSG_DSTPORT(msgid);
      const uint8_t *payload = (const uint8_t *)h + VCHIQ_HEADER_SIZE;

      if (type == VCHIQ_MSG_CONNECT) {
         sim->connected = true;
         vc_send(VCHIQ_MAKE_MSG(VCHIQ_MSG_CONNECT, 0, 0), NULL, 0);
      } else if (type == VCHIQ_MSG_OPEN) {
         for (int i = 0; i < VC_SERVICES; i++)
            if (!sim->svc[i].open) {
               sim->svc[i].open = true;
               sim->svc[i].armport = src;
               vc_send(VCHIQ_MAKE_MSG(VCHIQ_MSG_OPENACK, VC_PORT0 + (uint32_t)i, src), NULL, 0);
               break;
            }
      } else if (type == VCHIQ_MSG_CLOSE) {
         sim->closes_seen++;
         int i = (int)dst - (int)VC_PORT0;
         if (i >= 0 && i < VC_SERVICES && sim->svc[i].open && !sim->ignore_close) {
            sim->svc[i].open = false;
            sim->closes_sent++;
            vc_send(VCHIQ_MAKE_MSG(VCHIQ_MSG_CLOSE, dst, src), NULL, 0);
         }
      } else if (type == VCHIQ_MSG_DATA) {
         int i = (int)dst - (int)VC_PORT0;
         if (i >= 0 && i < VC_SERVICES && sim->svc[i].open && sim->svc[i].armport == src)
            vc_send(VCHIQ_MAKE_MSG(VCHIQ_MSG_DATA, dst, src), payload, size);   /* echo */
      }
      sim->rx_pos = (int32_t)(pos + CALC_STRIDE(size));
      if (((uint32_t)sim->rx_pos & VCHIQ_SLOT_MASK) == 0) {
         z->slave.slot_queue[(uint32_t)z->slave.slot_queue_recycle & VCHIQ_SLOT_QUEUE_MASK] = idx;
         __sync_synchronize();
         z->slave.slot_queue_recycle++;
      }
   }
}

/* ---- the stub platform ---------------------------------------------------- */

/* Time passes only when someone looks, and the VC works while it does. */
uint32_t RPI_GetSystemTime(void)
{
   now_us += 50u;
   vc_step();
   return now_us;
}

uint32_t screen_allocate_buffer(uint32_t size, uint32_t *handle)
{
   uint32_t at = (sim->alloc_next + 4095u) & ~4095u;
   if (at + size > ARENA_SIZE)
      return 0u;
   sim->alloc_next = at + size;
   memset(arena + at, 0, size);
   *handle = 0x1000u + at;
   return (uint32_t)(uintptr_t)(arena + at);
}
void screen_release_buffer(uint32_t handle) { (void)handle; }

static rpi_mailbox_property_t mbox;
static uint32_t mbox_arg;
void RPI_PropertyStart(rpi_mailbox_tag_t tag, uint32_t length) { mbox.tag = tag; (void)length; }
void RPI_PropertyAdd(uint32_t data) { mbox_arg = data; }

/* TAG_VCHIQ_INIT: the first one attaches the VC to that block (and the VC
   sets up its own half); every later one is accepted and ignored. */
void RPI_PropertyProcess(bool wait)
{
   (void)wait;
   mbox.data.buffer_32[0] = 0u;
   if (mbox.tag != TAG_VCHIQ_INIT)
      return;
   sim->inits++;
   if (sim->block)
      return;
   uint32_t phys = mbox_arg & ~0x40000000u;     /* undo vchiq_bus_addr */
   for (uint32_t off = 0; off < ARENA_SIZE; off += 4096u)
      if ((uint32_t)(uintptr_t)(arena + off) == phys || ((uint32_t)(uintptr_t)(arena + off) & ~0x40000000u) == phys) {
         sim->block = arena + off;
         break;
      }
   vchiq_slot_zero_t *z = vc_zero();
   uint32_t n = 0;
   for (int32_t i = z->master.slot_first; i <= z->master.slot_last; i++)
      z->master.slot_queue[n++] = i;
   z->master.slot_queue_recycle = (int32_t)n;
   z->master.initialised = 1;
}
rpi_mailbox_property_t *RPI_PropertyGet(rpi_mailbox_tag_t tag) { (void)tag; return &mbox; }

/* ---- the ARM-side client under test ---------------------------------------- */

static int failures, checks;

#define CHECK(cond, ...) do { \
      checks++; \
      if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                     printf(__VA_ARGS__); printf("\n"); } \
   } while (0)

static int got, got_bytes;
static void on_data(const void *data, unsigned int size) { (void)data; got++; got_bytes += (int)size; }
static const vchiq_callbacks_t cbs = { .on_data = on_data };

/* n messages of size bytes out, each echoed back: crosses many slots both
   ways, so only a client whose positions and queues are right gets through. */
static bool round_trips(int service, int n, unsigned size)
{
   static uint8_t msg[2000];
   for (int i = 0; i < n; i++) {
      int want = got + 1;
      uint32_t t0 = RPI_GetSystemTime();
      while (!vchiq_queue_message(service, msg, size))
         if (RPI_GetSystemTime() - t0 > 100000u) return false;
      while (got < want) {
         vchiq_poll();
         if (RPI_GetSystemTime() - t0 > 100000u) return false;
      }
   }
   return true;
}

static uint32_t arm_port_of(int vc_index) { return sim->svc[vc_index].armport; }

/* Every VC slot we have read to the end has gone back to the VC, and none
   other: it pre-loaded its queue with all of its slots, so the count it has
   had back is its slot total plus the slots behind our read position.  A
   client that skips messages, or loses its place, breaks this long before
   the VC actually runs dry. */
static bool all_read_slots_recycled(void)
{
   vchiq_slot_zero_t *z = vc_zero();
   uint32_t slots = (uint32_t)(z->master.slot_last - z->master.slot_first + 1);
   return (uint32_t)z->master.slot_queue_recycle == slots + (uint32_t)vc.rx_pos / VCHIQ_SLOT_SIZE;
}

/* Run a kernel in a child: vchiq.c's statics start fresh, as after a jump. */
static void kernel(void (*fn)(void), const char *name)
{
   fflush(stdout);
   pid_t pid = fork();
   if (pid == 0) {
      failures = 0;
      fn();
      fflush(stdout);
      _exit(failures > 255 ? 255 : failures);
   }
   int status = 0;
   (void)waitpid(pid, &status, 0);
   checks++;
   if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      failures++;
      printf("FAIL kernel %s (%s %d)\n", name,
             WIFEXITED(status) ? "exit" : "signal",
             WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status));
   }
}

/* ---- kernels ---------------------------------------------------------------- */

#define MMAL VCHIQ_FOURCC('m','m','a','l')
#define SMEM VCHIQ_FOURCC('S','M','E','M')

/* Cold boot: connect, use two services, close them, hand over. */
static void k_first(void)
{
   CHECK(vchiq_init(), "fresh init failed");
   CHECK(sim->inits == 1, "%d INITs", sim->inits);
   int a = vchiq_open_service(MMAL, 16, 10, &cbs);
   int b = vchiq_open_service(SMEM, 1, 0, &cbs);
   CHECK(a == 0 && b == 1, "services %d %d", a, b);
   CHECK(arm_port_of(0) == 1u && arm_port_of(1) == 2u, "ports %u %u", arm_port_of(0), arm_port_of(1));
   CHECK(round_trips(a, 40, 1000u), "round trips failed");
   CHECK(round_trips(b, 7, 300u), "round trips on the second service failed");
   CHECK(all_read_slots_recycled(), "slot accounting off");
   int closes = sim->closes_sent;
   CHECK(vchiq_close_service(a), "close of the first not answered");
   CHECK(vchiq_close_service(b), "close of the second not answered");
   CHECK(sim->closes_sent == closes + 2, "VC answered %d closes", sim->closes_sent - closes);
   CHECK(!sim->svc[0].open && !sim->svc[1].open, "VC still holds a service");
   /* Our CLOSE answered by the VC's must not be answered again: that would
      be a close of the VC's own, of a port it has already let go. */
   int seen = sim->closes_seen;
   for (int i = 0; i < 20; i++) { vchiq_poll(); (void)RPI_GetSystemTime(); }
   CHECK(sim->closes_seen == seen, "the VC's answer to our CLOSE was answered");
   sim->result = vchiq_handover();
   CHECK(sim->result == shared_phys && sim->result != 0u, "handover gave %08x", sim->result);
   CHECK(!vc.inited, "still using the channel after the handover");
}

/* Between kernels the VC goes on sending - to a port the outgoing kernel
   had, now nobody's - across a slot boundary. */
static void vc_late_traffic(int n)
{
   static uint8_t junk[1500];
   for (int i = 0; i < n; i++)
      vc_send(VCHIQ_MAKE_MSG(VCHIQ_MSG_DATA, VC_PORT0, 1u), junk, sizeof junk);
}

/* After the jump: adopt, no INIT, the late traffic dropped (and its slots
   recycled), fresh ports beyond the previous kernel's, a working stream. */
static void k_adopt(void)
{
   int inits = sim->inits;
   uint32_t base_before = handover_record(sim->result)->port_base;
   vchiq_adopt(sim->result);
   CHECK(vchiq_init(), "adoption failed");
   CHECK(sim->inits == inits, "an INIT was sent on adoption");
   CHECK(got == 0, "%d stale messages delivered", got);
   CHECK(all_read_slots_recycled(), "the late traffic's slots were not given back");
   CHECK(vc.port_base == base_before, "port base %u, record said %u", vc.port_base, base_before);
   int a = vchiq_open_service(MMAL, 16, 10, &cbs);
   CHECK(a == 0, "service %d", a);
   CHECK(arm_port_of(0) == base_before + 1u, "new port %u, want %u", arm_port_of(0), base_before + 1u);
   CHECK(round_trips(a, 60, 1500u), "round trips after adoption failed");
   CHECK(all_read_slots_recycled(), "slot accounting off after the round trips");
   CHECK(vchiq_close_service(a), "close not answered");
   sim->result = vchiq_handover();
   CHECK(sim->result != 0u, "nothing handed on");
   CHECK(handover_record(sim->result)->port_base == base_before + VCHIQ_MAX_SERVICES,
         "next port base %u", handover_record(sim->result)->port_base);
}

/* A kernel that never used video between two jumps passes the record on
   untouched. */
static void k_idle(void)
{
   vchiq_adopt(sim->result);
   uint32_t again = vchiq_handover();
   CHECK(again == sim->result, "idle kernel handed on %08x, had %08x", again, sim->result);
}

/* A kernel that does not adopt - an older one, or a lost record - gets
   what the hardware gives it: an INIT ignored, no CONNECT. */
static void k_fresh_after_jump(void)
{
   int inits = sim->inits;
   CHECK(!vchiq_init(), "a second INIT connected");
   CHECK(sim->inits == inits + 1, "no INIT sent");
   CHECK(vchiq_condemned, "not condemned");
}

/* A record that does not check out is not adopted: fresh start instead. */
static void k_bad_record(void)
{
   volatile vchiq_handover_t *h = handover_record(sim->result);
   uint32_t saved = h->rx_pos;
   h->rx_pos ^= 8;                                   /* the check no longer matches */
   int inits = sim->inits;
   vchiq_adopt(sim->result);
   CHECK(!vchiq_init(), "a corrupt record was adopted");
   CHECK(sim->inits == inits + 1, "no fresh start after the bad record");
   h->rx_pos = (int32_t)saved;
}

/* The VC does not answer a CLOSE: the service is gone on our side, its slot
   never reused this session, and after the jump the VC's still-open
   service cannot reach the new kernel's. */
static void k_close_unanswered(void)
{
   vchiq_adopt(sim->result);
   CHECK(vchiq_init(), "adoption failed");
   int a = vchiq_open_service(MMAL, 16, 10, &cbs);
   CHECK(round_trips(a, 3, 100u), "round trips failed");
   sim->ignore_close = true;
   CHECK(!vchiq_close_service(a), "an unanswered close reported answered");
   CHECK(!vc.svc[a].open && vc.svc[a].stale, "service not marked stale");
   int b = vchiq_open_service(SMEM, 1, 0, &cbs);
   CHECK(b >= 0 && b != a, "stale slot reused (%d)", b);
   sim->ignore_close = false;
   CHECK(vchiq_close_service(b), "close not answered");
   sim->result = vchiq_handover();
}

static void k_after_unanswered(void)
{
   vchiq_adopt(sim->result);
   CHECK(vchiq_init(), "adoption failed");
   /* The VC's leftover service (index 0 on its side, still open) sends to
      the port it knows - the previous kernel's - and nobody gets it. */
   int stale = -1;
   for (int i = 0; i < VC_SERVICES; i++) if (sim->svc[i].open) stale = i;
   CHECK(stale >= 0, "the VC let go of the unanswered service after all");
   int a = vchiq_open_service(MMAL, 16, 10, &cbs);
   CHECK(a == 0, "service %d", a);
   uint8_t x[16] = {0};
   vc_send(VCHIQ_MAKE_MSG(VCHIQ_MSG_DATA, VC_PORT0 + (uint32_t)stale, sim->svc[stale].armport), x, sizeof x);
   vchiq_poll();
   CHECK(got == 0, "the old service's message reached the new one");
   CHECK(SVC_PORT(a) != sim->svc[stale].armport, "new port %u is the old service's",
         SVC_PORT(a));
   CHECK(round_trips(a, 5, 800u), "round trips failed");
}

int main(void)
{
   arena = mmap(NULL, ARENA_SIZE + 4096u, PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
   if (arena == MAP_FAILED) { perror("mmap"); return 2; }
   sim = (sim_t *)(arena + ARENA_SIZE);
   memset(sim, 0, sizeof *sim);

   kernel(k_first, "first");
   vc_late_traffic(5);
   kernel(k_adopt, "adopt");
   kernel(k_idle, "idle");
   vc_late_traffic(3);
   kernel(k_adopt, "adopt again");
   kernel(k_bad_record, "bad record");
   kernel(k_close_unanswered, "close unanswered");
   kernel(k_after_unanswered, "after unanswered");
   kernel(k_fresh_after_jump, "fresh after jump");

   printf("%d checks, %d failed\n", checks, failures);
   if (failures == 0)
      printf("VCHIQ TESTS PASSED\n");
   return failures ? 1 : 0;
}
