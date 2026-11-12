/* The RAM probe end to end: the real ramprobe.c, its 6502 code and the real
   services_emulator.c, driving a cycle-stepped BBC (kc-beeb) with no agent ROM.
   The &FC88 helper select (helpers.c) and the &FD00 window (ram_emulator.c) are the
   firmware's too; the board (board.h) adds only a delay before a select takes
   effect, as a parameter.  Run by run_tests.sh. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "machine.c"

#include "board.h"

/* ---- kc-beeb on the board ----------------------------------------------------- */
static struct machine *m;
static int nmi_edges;
static bool nmi_low;
static void board_nmi_line(bool nmi)
{
   if (nmi && !nmi_low) { nmi_edges++; m->nmi_edge = 1; m->nmi_cycle = m->synced; }
   nmi_low = nmi;
}
uint8_t board_irq;
static int irq_raised;
static void board_irq_line(bool low)
{
   if (low && !board_irq) irq_raised++;
   if ((uint8_t)low != board_irq) { board_irq = low; m->irq_late = 1; }
}
uint8_t board_read(struct machine *mm, uint16_t a) { (void)mm; return board_bus_read(a); }
void board_write(struct machine *mm, uint16_t a, uint8_t v) { (void)mm; board_bus_write(a, v); }

/* ---- the machine ------------------------------------------------------------ */
static const char *roms;
static uint8_t *load(const char *name)
{
   char p[512];
   snprintf(p, sizeof p, "%s/%s", roms, name);
   FILE *f = fopen(p, "rb");
   if (!f) { printf("no %s\n", p); exit(2); }
   uint8_t *b = malloc(16384);
   size_t n = fread(b, 1, 16384, f);
   fclose(f);
   if (n != 16384) exit(2);
   return b;
}
static void boot(void)
{
   if (!m) {
      m = calloc(1, sizeof *m);
      machine_init(m);
      for (int i = 0; i < MACHINE_SWRAM_SLOTS; i++) m->swram[i] = calloc(1, 0x4000);
      m->mos = load("mos.rom");
      m->rom[0x9] = load("dfs.rom");
      static const char *others[6] = {"viewsht.rom", "edit.rom", "basic4.rom", "adfs.rom", "view.rom", "terminal.rom"};
      for (int i = 0; i < 6; i++) m->rom[0xa + i] = load(others[i]);
   }
   memset(m->keys, 0, sizeof m->keys);
   machine_reset(m);
   nmi_low = false;
   board_reset();
}
/* A pass of the main loop every 100 us, as a machine with a quiet poll has. */
static void run_ms(double ms)
{
   uint64_t end = m->cpu.cycles + (uint64_t)(ms * 2000);
   while (m->cpu.cycles < end) {
      uint64_t step = m->cpu.cycles + 200;
      machine_run(m, step < end ? step : end);
      board_poll();
   }
}
static int key_of(char c, int *shift)
{
   static const uint8_t letters[26] = {0x41,0x64,0x52,0x32,0x22,0x43,0x53,0x54,0x25,0x45,0x46,0x56,0x65,
                                       0x55,0x36,0x37,0x10,0x33,0x51,0x23,0x35,0x63,0x21,0x42,0x44,0x61};
   static const uint8_t digits[10] = {0x27,0x30,0x31,0x11,0x12,0x13,0x34,0x24,0x15,0x26};
   *shift = 0;
   if (c >= 'A' && c <= 'Z') return letters[c - 'A'];
   if (c >= '0' && c <= '9') return digits[c - '0'];
   switch (c) {
   case ' ': return 0x62;
   case '\r': return 0x49;
   case ':': return 0x48;
   case '/': return 0x68;
   case '?': *shift = 1; return 0x68;
   case '%': *shift = 1; return 0x13;
   case '&': *shift = 1; return 0x34;
   case '=': *shift = 1; return 0x17;
   case '*': *shift = 1; return 0x48;
   case '+': *shift = 1; return 0x57;
   case ',': return 0x66;
   case 27: return 0x70;                  /* Escape */
   }
   printf("no key for %c\n", c);
   exit(2);
}
static void press(int k, int down) { machine_set_key(m, (k >> 4) & 7, k & 15, down); }
static void type(const char *s)
{
   for (; *s; s++) {
      int sh, k = key_of(*s, &sh);
      if (sh) { press(0x00, 1); run_ms(30); }
      press(k, 1);
      run_ms(50);
      press(k, 0);
      if (sh) press(0x00, 0);
      run_ms(50);
   }
}
static int on_screen(const char *t)
{
   uint16_t a = m->ram[0x350] | (m->ram[0x351] << 8);
   char row[41];
   for (int r = 0; r < 25; r++) {
      for (int c = 0; c < 40; c++) {
         uint8_t v = m->ram[0x7C00 + ((a - 0x7C00 + r * 40 + c) & 0x3FF)] & 0x7F;
         row[c] = v >= 32 && v < 127 ? (char)v : ' ';
      }
      row[40] = 0;
      if (strstr(row, t)) return 1;
   }
   return 0;
}
static void to_prompt(void)
{
   run_ms(2500);
   type("MODE 7\r");
   run_ms(200);
}

/* ---- results ---------------------------------------------------------------- */
static int checks, fails;
static void ok(int cond, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   checks++;
   printf("  %s: ", cond ? "ok" : "FAIL");
   vprintf(fmt, ap);
   puts("");
   va_end(ap);
   if (!cond) fails++;
}
static uint32_t rng = 12345;
static uint32_t rnd(uint32_t n) { rng = rng * 1664525u + 1013904223u; return (rng >> 8) % n; }
static uint32_t a_percent(void) { return m->ram[0x404] | m->ram[0x405] << 8 | m->ram[0x406] << 16; }

static bool wait_state(rp_state_t want, double ms)
{
   for (double t = 0; t < ms; t += 0.2) {
      if (ramprobe_state() == want) return true;
      run_ms(0.2);
   }
   return ramprobe_state() == want;
}
static bool halt_now(void) { return ramprobe_halt(RP_RUNG_NMI) && wait_state(RP_HALTED, 200); }
static bool resume_now(const rp_resume_t *how) { return ramprobe_resume(how) && wait_state(RP_RUNNING, 200); }

static void install(void)
{
   type("X%=23:CALL &FC88\r");
   run_ms(300);
}

static uint16_t vec(uint16_t a) { return (uint16_t)(m->ram[a] | m->ram[a + 1] << 8); }
/* The old vector a hook in FRED goes on to: its JMP operand. */
static uint16_t fred_vec(unsigned hook) { return (uint16_t)(pi.Memory[hook + 1u] | (pi.Memory[hook + 2u] << 8)); }

/* ---- the scenarios ------------------------------------------------------------ */
static void session(void)
{
   puts("== before it's installed");
   boot();
   to_prompt();
   ok(on_screen(">"), "BASIC is at its prompt, no agent ROM in the machine");
   ok(m->ram[0x0D00] == 0x40, "&0D00 holds the OS's RTI");
   ok(!ramprobe_installed(), "the Pi knows it isn't installed");
   int edges = nmi_edges;
   ok(!ramprobe_halt(RP_RUNG_NMI), "halt refused: %s", ramprobe_error());
   ok(nmi_edges == edges, "no NMI sent into a machine with no stub");
   helper_ram[(RP_HELPER_INSTALL << 8) + 255] = 0x60;          /* a 6502code.bin from before the probe */
   ok(!ramprobe_halt(RP_RUNG_EVENT) && strstr(ramprobe_error(), "6502code.bin"), "an old 6502code.bin, said: %s", ramprobe_error());
   helper_ram[(RP_HELPER_INSTALL << 8) + 255] = RP_HELPER_INSTALL;
   {
      uint8_t *rep = &pi.JIM_ram[DISC_RAM_BASE + RP_REPORT], was[16];
      memset(rep, 0xA7, sizeof was);                      /* someone else's bytes, say */
      memcpy(was, rep, sizeof was);
      ramprobe_init(0, 0);                                /* a BBC reset, the probe never installed */
      ok(memcmp(rep, was, sizeof was) == 0, "a reset with no install leaves its JIM area alone");
      memset(rep, 0, sizeof was);
   }

   puts("== install: one typed line");
   install();
   ok(m->ram[0x0D00] == 0x4C && m->ram[0x0D01] == (uint8_t)(RP_FRED_A + 5u) && m->ram[0x0D02] == 0xFC,
      "&0D00 holds JMP &FC%02X, the NMI's way in, and that's all of BBC RAM", RP_FRED_A + 5u);
   ok(rp_fred[RP_IN_AT] == 0x00 && pi.Memory[RP_FRED_A + RP_IN_AT] == RP_IN_OP
      && memcmp(pi.Memory + RP_FRED_A + 3u, rp_fred + 3u, RP_IN_AT - 3u) == 0
      && memcmp(pi.Memory + RP_FRED_A + RP_IN_AT + 1u, rp_fred + RP_IN_AT + 1u, RP_FRED_A_LEN - RP_IN_AT - 1u) == 0
      && memcmp(pi.Memory + RP_FRED_B + 3u, rp_fred + RP_FRED_A_LEN + 3u, RP_FRED_B_LEN - 3u) == 0
      && memcmp(pi.Memory + RP_FRED_C, rp_fred + RP_FRED_A_LEN + RP_FRED_B_LEN, RP_FRED_C_LEN) == 0,
      "the hooks and the ways in and out are in FRED, with the Pi's PHA at rp_in");
   ok(pi.Memory[RP_HOOK_EVENT] == RP_IDLE && pi.Memory[RP_HOOK_IRQ2] == RP_IDLE, "both hooks idle: JMP");
   ok(vec(0x0220) == 0xFC00u + RP_HOOK_EVENT && fred_vec(RP_HOOK_EVENT) >= 0x8000u,
      "EVNTV points at its hook in FRED, which goes on to &%04X", fred_vec(RP_HOOK_EVENT));
   ok(ramprobe_installed(), "the Pi was told");
   ok(on_screen(">"), "back at the prompt");

   puts("== halt a BASIC program");
   type("10 REPEAT:A%=A%+1:UNTIL 0\r");
   type("RUN\r");
   run_ms(300);
   uint32_t a0 = a_percent();
   run_ms(100);
   ok(a_percent() != a0, "it is counting");
   ok(halt_now(), "halted: %s", ramprobe_state_name());
   const rp_regs_t *st = ramprobe_stopped();
   printf("       A=%02X X=%02X Y=%02X P=%02X PC=%04X S=%02X\n", st->a, st->x, st->y, st->p, st->pc, st->s);
   ok(st->pc >= 0x8000, "the PC is in BASIC or the OS (&%04X)", st->pc);
   a0 = a_percent();
   run_ms(1000);
   ok(a_percent() == a0, "frozen for a second");

   puts("== peek and poke");
   ok(ramprobe_peek(0x8000, 0x4000, 12, RP_RUNG_NONE) && wait_state(RP_HALTED, 5000), "peek all of bank 12: %s", ramprobe_error());
   uint32_t got = 0;
   const uint8_t *d = ramprobe_data(&got);
   ok(got == 0x4000 && memcmp(d, m->rom[12], 0x4000) == 0, "the bytes are BASIC's ROM, %u of them", got);
   ok(ramprobe_peek(0x0000, 0x1000, RP_NO_BANK, RP_RUNG_NONE) && wait_state(RP_HALTED, 2000), "peek 4 KB of RAM");
   d = ramprobe_data(&got);
   {  /* The agent itself moves on after the copy: it and its calls use the stack below the
         frame, and nothing else.  Nothing else may differ. */
      int wrong = 0, first = -1;
      unsigned below = 0x100u + ramprobe_stopped()->s - 0x20u;
      for (unsigned i = 0; i < 0x1000u; i++) {
         if (d[i] == m->ram[i]) continue;
         if (i >= below && i <= 0x100u + ramprobe_stopped()->s) continue;
         if (first < 0) first = (int)i;
         wrong++;
      }
      ok(got == 0x1000 && wrong == 0, "matches memory, &0D00 included (%d differ, first &%04X)", wrong, first < 0 ? 0 : first);
      ok(d[0x0D00] == 0x4C && d[0x0D02] == 0xFC, "the NMI's JMP reads back through the peek");
   }
   uint8_t buf[600];
   for (unsigned i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)(i * 7 + 3);
   ok(ramprobe_poke(0x3000, buf, sizeof buf, RP_RUNG_NONE) && wait_state(RP_HALTED, 2000), "poke %zu bytes, across pages", sizeof buf);
   ok(memcmp(m->ram + 0x3000, buf, sizeof buf) == 0, "they arrived");
   uint8_t b0 = 0x11;
   ok(!ramprobe_poke(0x0D02, &b0, 1, RP_RUNG_NONE), "refused: %s", ramprobe_error());
   ok(!ramprobe_poke(0xFCA0, &b0, 1, RP_RUNG_NONE), "refused: %s", ramprobe_error());
   ok(ramprobe_state() == RP_HALTED, "still stopped after a refusal");

   puts("== several ranges in one request");
   {
      rp_range_t r[RP_MAX_RANGES];
      uint32_t total = 0;
      for (unsigned i = 0; i < RP_MAX_RANGES; i++) {        /* 1 to 300 bytes, anywhere from &0400 to &7000 */
         r[i].length = i == 5 ? 300u : 1u + rnd(i < 16 ? 8u : 270u);
         r[i].address = (uint16_t)(0x0400u + rnd(0x6C00u));
         total += r[i].length;
      }
      ok(ramprobe_peek_ranges(r, RP_MAX_RANGES, RP_NO_BANK, RP_RUNG_NONE) && wait_state(RP_HALTED, 3000),
         "peek %u ranges, %u bytes, in one request", RP_MAX_RANGES, total);
      uint32_t got = 0, at = 0;
      const uint8_t *d = ramprobe_data(&got);
      int bad_ranges = 0;
      for (unsigned i = 0; i < RP_MAX_RANGES; i++) {
         if (memcmp(d + at, m->ram + r[i].address, r[i].length) != 0) bad_ranges++;
         at += r[i].length;
      }
      ok(got == total && bad_ranges == 0, "back to back, each as memory has it (%d differ)", bad_ranges);

      static const rp_range_t w[3] = { {0x3100, 4}, {0x32F0, 0x20}, {0x3500, 300} };   /* the second across a page */
      uint8_t bytes[4 + 0x20 + 300];
      for (unsigned i = 0; i < sizeof bytes; i++) bytes[i] = (uint8_t)(i * 13 + 5);
      ok(ramprobe_poke_ranges(w, 3, bytes, RP_RUNG_NONE) && wait_state(RP_HALTED, 2000), "poke 3 ranges in one request");
      ok(memcmp(m->ram + 0x3100, bytes, 4) == 0 && memcmp(m->ram + 0x32F0, bytes + 4, 0x20) == 0
         && memcmp(m->ram + 0x3500, bytes + 0x24, 300) == 0, "each where it was meant to go");

      static const rp_range_t rb[2] = { {0x8000, 0x10}, {0xBF00, 0x100} };
      ok(ramprobe_peek_ranges(rb, 2, 12, RP_RUNG_NONE) && wait_state(RP_HALTED, 2000), "two ranges of a sideways bank");
      d = ramprobe_data(&got);
      ok(got == 0x110 && memcmp(d, m->rom[12], 0x10) == 0 && memcmp(d + 0x10, m->rom[12] + 0x3F00, 0x100) == 0, "BASIC's, both");

      rp_range_t many[RP_MAX_RANGES + 1];
      for (unsigned i = 0; i <= RP_MAX_RANGES; i++) many[i] = (rp_range_t){ (uint16_t)(0x3000u + i), 1u };
      ok(!ramprobe_peek_ranges(many, RP_MAX_RANGES + 1u, RP_NO_BANK, RP_RUNG_NONE), "33 ranges refused: %s", ramprobe_error());
      static const rp_range_t io[2] = { {0x3000, 4}, {0xFE40, 1} };
      ok(!ramprobe_peek_ranges(io, 2, RP_NO_BANK, RP_RUNG_NONE), "a range on I/O refused, the lot: %s", ramprobe_error());
      static const rp_range_t nmi[2] = { {0x3000, 4}, {0x0D01, 1} };
      ok(!ramprobe_poke_ranges(nmi, 2, bytes, RP_RUNG_NONE) && memcmp(m->ram + 0x3000, buf, 4) == 0,
         "a poke with a range on the NMI's JMP refused, and nothing written: %s", ramprobe_error());
      static const rp_range_t big[2] = { {0x0000, 0x8000}, {0x0000, 0x8001} };
      ok(!ramprobe_peek_ranges(big, 2, RP_NO_BANK, RP_RUNG_NONE), "more than 64 KB in all refused: %s", ramprobe_error());
      ok(ramprobe_state() == RP_HALTED, "still stopped");
   }

   puts("== call code");
   static const uint8_t add[] = {0x18, 0x69, 0x05, 0xE8, 0xC8, 0x60};   /* CLC : ADC #5 : INX : INY : RTS */
   ok(ramprobe_run(add, sizeof add, 0x20, 0x40, 0x60) && wait_state(RP_HALTED, 200), "run code from the Pi's memory");
   const rp_regs_t *res = ramprobe_result();
   ok(res->a == 0x25 && res->x == 0x41 && res->y == 0x61, "A, X and Y in and out (&%02X &%02X &%02X)", res->a, res->x, res->y);
   memcpy(buf, (const uint8_t[]){0xA9, 0x77, 0x60}, 3);                   /* LDA #&77 : RTS */
   ok(ramprobe_poke(0x3400, buf, 3, RP_RUNG_NONE) && wait_state(RP_HALTED, 200), "poke a routine");
   ok(ramprobe_call(0x3400, 0, 0, 0) && wait_state(RP_HALTED, 200) && ramprobe_result()->a == 0x77, "JSR it: A=&%02X", ramprobe_result()->a);
   rp_regs_t was = *ramprobe_stopped();
   ok(resume_now(NULL), "continue");
   run_ms(200);
   ok(a_percent() != a0, "the program goes on (A%% %u -> %u)", a0, a_percent());

   puts("== take over: a program put in over a running one");
   ok(halt_now(), "halted");
   was = *ramprobe_stopped();
   static const uint8_t spin[] = {0x78, 0xEE, 0x70, 0x00, 0x4C, 0x01, 0x20};  /* SEI : .l INC &0070 : JMP l */
   ok(ramprobe_poke(0x2000, spin, sizeof spin, RP_RUNG_NONE) && wait_state(RP_HALTED, 200), "poke it to &2000");
   rp_resume_t go = { .set_pc = true, .pc = 0x2000 };
   ok(resume_now(&go), "continue at &2000");
   run_ms(100);
   ok(m->ram[0x70] != 0 && m->cpu.pc >= 0x2000 && m->cpu.pc < 0x2008, "it is running, interrupts off (PC=&%04X)", m->cpu.pc);

   puts("== a machine with interrupts off");
   ok(halt_now(), "the NMI reaches it");
   ok((ramprobe_stopped()->pc & 0xFFF0) == 0x2000 && (ramprobe_stopped()->p & 4), "PC=&%04X, I set", ramprobe_stopped()->pc);
   uint8_t z = m->ram[0x70];
   run_ms(500);
   ok(m->ram[0x70] == z, "frozen");
   ok(ramprobe_stopped()->s == was.s, "the stack pointer is where it was");
   rp_resume_t back = { .set_pc = true, .pc = was.pc, .set_p = true, .p = was.p, .set_a = true, .a = was.a,
                        .set_x = true, .x = was.x, .set_y = true, .y = was.y };
   ok(resume_now(&back), "hand it back: PC, flags, A, X, Y");
   a0 = a_percent();
   run_ms(200);
   ok(a_percent() != a0, "the BASIC program is counting again");
   ok(resume_now(NULL) == false, "continue refused when running: %s", ramprobe_error());
}

static bool back_running(double ms) { return wait_state(RP_RUNNING, ms) && ramprobe_error()[0] == '\0'; }

/* The ladder: rung 0 (the vsync event) and rung 1 (nIRQ, IRQ2V) with no NMI; each fails on its
   own, cleanly, where it can't reach; the client picks.  Starts and ends with BASIC counting. */
static void ladder(void)
{
   puts("== the ladder: the hooks are in");
   uint16_t oldev = fred_vec(RP_HOOK_EVENT), oldirq = fred_vec(RP_HOOK_IRQ2);
   ok(vec(0x0220) == 0xFC00u + RP_HOOK_EVENT && vec(0x0206) == 0xFC00u + RP_HOOK_IRQ2, "EVNTV and IRQ2V point at the hooks in FRED");
   ok(oldev >= 0x8000 && oldirq >= 0x8000, "and go on to the OS's own: &%04X, &%04X", oldev, oldirq);

   puts("== rung 0: the vsync event, no NMI, no IRQ");
   int e = nmi_edges, q = irq_raised;
   ok(ramprobe_halt(RP_RUNG_EVENT) && wait_state(RP_HALTED, 200), "halted at rung 0: %s", ramprobe_error());
   ok(ramprobe_rung() == RP_RUNG_EVENT && nmi_edges == e && irq_raised == q, "by rung %d; NMIs %d, IRQs %d",
      ramprobe_rung(), nmi_edges - e, irq_raised - q);
   ok(pi.Memory[RP_HOOK_EVENT] == RP_IDLE && pi.Memory[RP_HOOK_IRQ2] == RP_IDLE, "nothing asked of the BBC any more");
   uint32_t a0 = a_percent();
   run_ms(300);
   ok(a_percent() == a0, "frozen");
   ok(ramprobe_peek(0x0404, 4, RP_NO_BANK, RP_RUNG_NONE) && wait_state(RP_HALTED, 200), "a peek, stopped");
   /* Stopped inside the OS's vsync event: the registers are the OS's, so none may be set. */
   rp_resume_t elsewhere = { .set_pc = true, .pc = 0x2000 }, just_a = { .set_a = true, .a = 1 };
   ok(!ramprobe_resume(&elsewhere) && !ramprobe_resume(&just_a) && strstr(ramprobe_error(), "rung 0")
      && ramprobe_state() == RP_HALTED, "a new PC or A refused, still stopped: %s", ramprobe_error());
   ok(resume_now(NULL), "continue");
   run_ms(200);
   ok(a_percent() != a0, "counting again");

   puts("== in, a transfer and out, in one request");
   static const uint8_t pat[8] = {1, 2, 3, 4, 5, 6, 7, 8};
   uint32_t got = 0;
   ok(ramprobe_poke(0x3000, pat, sizeof pat, RP_RUNG_EVENT) && back_running(300), "a poke at rung 0, the BBC running: %s", ramprobe_error());
   ok(memcmp(m->ram + 0x3000, pat, sizeof pat) == 0, "it's there");
   ok(ramprobe_peek(0x3000, sizeof pat, RP_NO_BANK, RP_RUNG_EVENT) && back_running(300)
      && memcmp(ramprobe_data(&got), pat, sizeof pat) == 0 && got == sizeof pat, "a peek at rung 0: the same 8 bytes");
   a0 = a_percent();
   run_ms(200);
   ok(a_percent() != a0 && nmi_edges == e && irq_raised == q, "BASIC carried on; still no NMI or IRQ");
   ok(!ramprobe_peek(0x3000, 8, RP_NO_BANK, RP_RUNG_NONE), "with no rung, a running BBC is refused: %s", ramprobe_error());

   puts("== a hook that read BIT just before JMP was put back: let go at once");
   pi.Memory[RP_HOOK_EVENT] = RP_WANTED;     /* as if a failed rung 0 had put JMP back a moment late */
   run_ms(25);
   pi.Memory[RP_HOOK_EVENT] = RP_IDLE;
   run_ms(100);
   a0 = a_percent();
   run_ms(200);
   ok(ramprobe_state() == RP_RUNNING && a_percent() != a0, "the Pi sent it on its way: %s, counting", ramprobe_state_name());

   puts("== events off: rung 0 fails, cleanly; rung 1 reaches it");
   type("\x1b");
   run_ms(300);
   type("*FX13,4\r");
   run_ms(200);
   ok(ramprobe_halt(RP_RUNG_EVENT), "rung 0 asked");
   ok(wait_state(RP_RUNNING, 500) && strstr(ramprobe_error(), "rung 0") != NULL, "and failed: %s", ramprobe_error());
   ok(pi.Memory[RP_HOOK_EVENT] == RP_IDLE && pi.Memory[RP_HOOK_IRQ2] == RP_IDLE && nmi_edges == e && irq_raised == q, "nothing left asked; no NMI, no IRQ");
   ok(ramprobe_halt(RP_RUNG_IRQ) && wait_state(RP_HALTED, 200), "rung 1: halted: %s", ramprobe_error());
   ok(ramprobe_rung() == RP_RUNG_IRQ && irq_raised == q + 1 && nmi_edges == e, "by rung %d, one IRQ, no NMI", ramprobe_rung());
   ok(!board_irq && pi.Memory[RP_HOOK_EVENT] == RP_IDLE && pi.Memory[RP_HOOK_IRQ2] == RP_IDLE, "nIRQ let go once in");
   ok(resume_now(NULL), "continue");
   run_ms(100);
   ok(on_screen(">"), "BASIC at its prompt, no worse");
   type("*FX14,4\r");
   run_ms(200);

   puts("== interrupts off: rungs 0 and 1 fail, cleanly; rung 2 reaches it");
   static const uint8_t spin[] = {0x78, 0xEE, 0x70, 0x00, 0x4C, 0x01, 0x20};   /* SEI : .l INC &0070 : JMP l */
   ok(ramprobe_poke(0x2000, spin, sizeof spin, RP_RUNG_EVENT) && back_running(300), "the loop poked in at rung 0");
   type("CALL &2000\r");
   run_ms(100);
   ok(ramprobe_halt(RP_RUNG_EVENT) && wait_state(RP_RUNNING, 500), "rung 0 fails: %s", ramprobe_error());
   ok(ramprobe_halt(RP_RUNG_IRQ) && wait_state(RP_RUNNING, 200), "rung 1 fails: %s", ramprobe_error());
   ok(!board_irq && pi.Memory[RP_HOOK_EVENT] == RP_IDLE && pi.Memory[RP_HOOK_IRQ2] == RP_IDLE && nmi_edges == e, "nIRQ let go, nothing asked, no NMI");
   ok(halt_now() && ramprobe_rung() == RP_RUNG_NMI, "rung 2, the NMI, reaches it");
   ok(ramprobe_poke(0x2000, (const uint8_t[]){0x58, 0x60}, 2, RP_RUNG_NONE) && wait_state(RP_HALTED, 200), "CLI : RTS over it");
   rp_resume_t home = { .set_pc = true, .pc = 0x2000 };
   ok(resume_now(&home), "and back to BASIC");
   run_ms(300);
   ok(on_screen(">"), "at its prompt");

   puts("== the line typed again keeps the vectors it found");
   install();
   ok(fred_vec(RP_HOOK_EVENT) == oldev && fred_vec(RP_HOOK_IRQ2) == oldirq, "not the hooks' own: &%04X, &%04X",
      fred_vec(RP_HOOK_EVENT), fred_vec(RP_HOOK_IRQ2));
   type("RUN\r");
   run_ms(300);
}

/* A program checking its own registers, then many stops at random moments. */
static void load_checker(void)
{
   /* .l LDA #&55 : LDX #&AA : LDY #&33 : SEC : CMP #&55 : BNE bad : CPX #&AA : BNE bad
         CPY #&33 : BNE bad : BCC bad : CLC : BCS bad : INC &0070 : BNE l : INC &0072 : JMP l
      .bad INC &0071 : JMP l */
   static const uint8_t chk[] = {0xA9,0x55,0xA2,0xAA,0xA0,0x33,0x38,0xC9,0x55,0xD0,0x18,0xE0,0xAA,0xD0,0x14,0xC0,
                                 0x33,0xD0,0x10,0x90,0x0E,0x18,0xB0,0x0B,0xEE,0x70,0x00,0xD0,0xE3,0xEE,0x72,0x00,
                                 0x4C,0x00,0x20,0xEE,0x71,0x00,0x4C,0x00,0x20};
   ok(halt_now() && ramprobe_poke(0x2000, chk, sizeof chk, RP_RUNG_NONE) && wait_state(RP_HALTED, 500), "the checker is in at &2000");
   m->ram[0x70] = m->ram[0x71] = m->ram[0x72] = 0;
   rp_resume_t go = { .set_pc = true, .pc = 0x2000, .set_p = true, .p = 0x30 };
   ok(resume_now(&go), "and running, interrupts on");
   run_ms(50);
}

static void stress(void)
{
   puts("== stress: 2000 stops, each with a peek, on a program checking its own registers");
   boot();
   to_prompt();
   install();
   load_checker();
   int lost = 0, good = 0;
   for (int i = 0; i < 2000; i++) {
      run_ms((rnd(300) + 1) / 10.0);
      if (!halt_now()) { lost++; continue; }
      if (!ramprobe_peek(0x0070, 3, RP_NO_BANK, RP_RUNG_NONE) || !wait_state(RP_HALTED, 50)) { lost++; continue; }
      if (!resume_now(NULL)) { lost++; continue; }
      good++;
   }
   run_ms(50);
   ok(lost == 0, "%d of 2000 went through (%d lost): %s", good, lost, ramprobe_error());
   ok(m->ram[0x71] == 0, "no register or flag spoiled (%u errors)", m->ram[0x71]);
   ok(m->ram[0x72] != 0 || m->ram[0x70] != 0, "still running");
   uint8_t v = m->ram[0x240];
   run_ms(200);
   ok(m->ram[0x240] != v, "the OS's vsync still ticks");

   puts("== control: the checker notices a spoiled register");
   int seen = 0;
   for (int i = 0; i < 400 && !seen; i++) {
      run_ms((rnd(300) + 1) / 10.0);
      if (!halt_now()) break;
      const uint8_t spoil[] = {0xEE, ramprobe_stopped()->s, 0x01, 0x60};   /* INC &0100+S (the saved A) : RTS */
      ramprobe_run(spoil, sizeof spoil, 0, 0, 0);
      wait_state(RP_HALTED, 50);
      resume_now(NULL);
      run_ms(1);
      seen = m->ram[0x71];
   }
   ok(seen > 0, "caught, after a spoiled A");
}

static void through_the_os(void)
{
   puts("== stress: 2000 stops on BASIC printing through the OS");
   boot();
   to_prompt();
   install();
   type("10 A%=A%+1:PRINT A%:GOTO 10\r");
   type("RUN\r");
   run_ms(500);
   int lost = 0;
   for (int i = 0; i < 2000; i++) {
      run_ms((rnd(200) + 1) / 10.0);
      if (!halt_now() || !resume_now(NULL)) lost++;
   }
   ok(lost == 0, "none lost (%d)", lost);
   uint32_t a0 = a_percent();
   run_ms(500);
   ok(a_percent() > a0, "BASIC is still printing (A%% %u -> %u)", a0, a_percent());
}

static void services_port(void)
{
   puts("== the program's own use of the services port survives a stop");
   boot();
   to_prompt();
   install();
   /* Its pointer set to &123456, then the machine is stopped and let go. */
   type("?&FCA6=&56:?&FCA7=&34:?&FCA8=&12\r");
   run_ms(100);
   ok(halt_now() && resume_now(NULL), "stopped and let go");
   ok(pi.Memory[0xA6] == 0x56 && pi.Memory[0xA7] == 0x34 && pi.Memory[0xA8] == 0x12, "the pointer is back (&%02X%02X%02X)", pi.Memory[0xA8], pi.Memory[0xA7], pi.Memory[0xA6]);
   puts("== and its JIM page");
   type("?&FCFF=&22\r");
   run_ms(100);
   pi.JIM_ram[0x2200] = 0xAB;
   pi.Memory[256] = 0xAB;
   ok(halt_now() && resume_now(NULL), "stopped and let go");
   run_ms(1);
   ok(pi.Memory[256] == 0xAB && !ram_emulator_window_is_helper(), "the window shows its page again");
}

static void what_stops_it(void)
{
   puts("== what stops it");
   boot();
   to_prompt();
   install();
   type("?&D00=&40:FOR I%=&380 TO &3DF:?I%=0:NEXT\r");   /* the NMI's JMP, and where the hooks once were */
   run_ms(300);
   ok(ramprobe_halt(RP_RUNG_EVENT) && wait_state(RP_HALTED, 300) && resume_now(NULL),
      "a program writes over &0D00 and &0380-&03DF: rung 0 doesn't care: %s", ramprobe_error());
   ok(ramprobe_halt(RP_RUNG_NMI), "but halt by the NMI starts");
   ok(!wait_state(RP_HALTED, 300), "no answer");
   ok(ramprobe_state() == RP_RUNNING, "gave up, not stuck: %s", ramprobe_error());
   ok(on_screen(">"), "the machine itself is fine");
   int edges = nmi_edges;
   ok(ramprobe_halt(RP_RUNG_NMI), "it can try again");
   run_ms(300);
   ok(nmi_edges - edges <= 3, "at most 3 NMIs a try (%d)", nmi_edges - edges);

   puts("== a call that doesn't come back");
   install();
   ok(halt_now(), "halted");
   static const uint8_t loop[] = {0x4C, 0xA0, 0xFD};                     /* JMP &FDA0: itself */
   ok(ramprobe_run(loop, sizeof loop, 0, 0, 0), "run a loop");
   run_ms(6000);
   ok(ramprobe_state() == RP_RUNNING && ramprobe_error()[0], "the Pi gives up after 5 s: %s", ramprobe_error());

   puts("== a BBC reset");
   boot();
   to_prompt();
   install();
   ok(ramprobe_installed(), "installed");
   uint32_t resets = ramprobe_resets();
   ramprobe_init(0, 0);                                                  /* as the emulator table does at a reset */
   ok(!ramprobe_installed(), "a reset forgets it");
   ok(ramprobe_resets() == resets + 1u, "and counts it, for a client to notice: %u", (unsigned)ramprobe_resets());
   ok(!ramprobe_halt(RP_RUNG_NMI), "and refuses to pulse");
}

/* Break: the 6502 and the board's nRST.  RAM and the System VIA carry on, so the MOS sees no
   power-up; Ctrl held makes it a hard Break.  A model of the machine, not kc-beeb's own. */
static void do_break(bool ctrl)
{
   if (ctrl) { press(0x01, 1); run_ms(30); }
   fdc_break_reset(&m->fdc);
   cpu6502_reset(&m->cpu);
   nmi_low = false;
   board_reset();
   run_ms(3000);
   if (ctrl) { press(0x01, 0); run_ms(100); }
}

static void break_key(void)
{
   puts("== the one line sets up Break too (*KEY10)");
   boot();
   to_prompt();
   type("MODE 7\r");
   run_ms(200);
   install();
   ok(on_screen("Pi1MHz probe on") && on_screen(">"), "the line says so, and BASIC carries on");
   ok(ramprobe_installed() && m->ram[0x0D00] == 0x4C, "and the NMI's JMP is in");
   install();
   ok(ramprobe_installed() && on_screen(">"), "typed again, no harm");
   uint32_t resets = ramprobe_resets();
   int back = 0, worked = 0;
   for (int i = 0; i < 5; i++) {
      do_break(false);
      if (m->ram[0x0D00] == 0x4C && vec(0x0220) == 0xFC00u + RP_HOOK_EVENT && ramprobe_installed()) back++;
      uint32_t n = 0;
      if (halt_now() && ramprobe_peek(0x0D00, 1, RP_NO_BANK, RP_RUNG_NONE) && wait_state(RP_HALTED, 500)
          && ramprobe_data(&n)[0] == 0x4C && n == 1u && resume_now(NULL)) worked++;
   }
   ok(back == 5, "after each of 5 soft Breaks the probe is back by itself: %d", back);
   ok(worked == 5, "and halt, peek and continue work: %d", worked);
   ok(ramprobe_resets() == resets + 5u, "each Break counted: %u", (unsigned)(ramprobe_resets() - resets));
   type("NEW\r10 PRINT 123\r");
   run_ms(100);
   do_break(false);
   type("OLD\rRUN\r");
   run_ms(300);
   ok(on_screen("123"), "a program in memory comes back with OLD");
   do_break(true);
   ok(m->ram[0x0D00] != 0x48 && !ramprobe_installed(), "Ctrl-Break clears the key: the line is typed again then");
   ok(!strcmp(ramprobe_state_name(), "waiting"), "and the state says waiting, not running: %s", ramprobe_state_name());

   puts("== Y%=1 leaves the BBC's own *KEY10");
   type("*KEY10 PRINT 12345\r");                   /* no |M: Break types it onto the input line */
   run_ms(100);
   type("X%=23:Y%=1:CALL &FC88\r");
   run_ms(300);
   ok(ramprobe_installed() && on_screen("Pi1MHz probe on"), "installed");
   type("CLS\r");
   run_ms(100);
   do_break(false);
   run_ms(300);
   ok(on_screen("PRINT 12345") && !ramprobe_installed(), "a soft Break types the user's key, and doesn't put the probe back");
   type("\x1b");                                     /* that line, abandoned */
   run_ms(100);
   type("X%=23:Y%=0:CALL &FC88\r");
   run_ms(300);
   ok(ramprobe_installed(), "Y%% not 1: installed, and the key set");
   type("CLS\r");
   run_ms(100);
   do_break(false);
   ok(ramprobe_installed(), "and a soft Break puts it back");

   puts("== the key checks HIMEM: a shadow mode's &8000 is still the BBC's own BASIC");
   type("MODE 135\r");
   run_ms(300);
   type("CLS\r");
   run_ms(100);
   do_break(false);
   ok(ramprobe_installed(), "a soft Break in MODE 135 puts it back too");
}

/* A stop while another helper runs from &FD00 must come back to that helper, its page as it
   left it.  Helper 24 here counts in its own page (&FD80, &FD81) and in RAM (&82): a stop that
   came back to JIM RAM would crash, and one that came back to a fresh copy of the page would
   start the count again, leaving &82 past &C0. */
static void helper_stop(void)
{
   puts("== a stop while another helper runs: back to it, its page as it was");
   boot();
   to_prompt();
   install();
   static const uint8_t counter[] = {
      0xA9, 0x00, 0x8D, 0x80, 0xFD, 0x8D, 0x81, 0xFD, 0x85, 0x82,   /* LDA #0: STA &FD80, &FD81, &82 */
      0xEE, 0x80, 0xFD, 0xD0, 0xFB,                                 /* loop: INC &FD80: BNE loop */
      0xE6, 0x82, 0xEE, 0x81, 0xFD,                                 /* INC &82: INC &FD81 */
      0xAD, 0x81, 0xFD, 0xC9, 0xC0, 0xD0, 0xEF,                     /* LDA &FD81: CMP #&C0: BNE loop */
      0xA2, 0xFF, 0x4C, 0x88, 0xFC };                               /* LDX #&FF: JMP &FC88 */
   memcpy(helper_ram + (24 << 8), counter, sizeof counter);
   m->ram[0x82] = 0;
   type("X%=24:CALL &FC88:PRINT 1000+?&82\r");
   int in_helper = 0, stops = 0, lost = 0;
   for (int i = 0; i < 400 && !on_screen("1192"); i++) {
      int rung = i % 3 == 0 ? RP_RUNG_EVENT : RP_RUNG_IRQ;
      if (ramprobe_halt(rung) && wait_state(RP_HALTED, 400)) {
         stops++;
         if ((ramprobe_stopped()->pc & 0xFF00) == 0xFD00) in_helper++;
         if (!resume_now(NULL)) lost++;
      } else {
         lost++;
      }
      run_ms(rnd(8));
   }
   run_ms(300);
   ok(in_helper > 20 && lost == 0, "%d stops, %d of them in the helper, %d lost", stops, in_helper, lost);
   ok(on_screen("1192") && m->ram[0x82] == 0xC0, "it finished its count, once: &82=%u", m->ram[0x82]);
   ok(pi.Memory[0x88 + 4] == 0x60 && !ram_emulator_window_is_helper(), "and left the window on JIM RAM");

   puts("== a helper left by the page registers, not by &FF: the stop goes back to JIM RAM");
   type("?&FC88=24:?&FCFF=0:?&FCFE=0:?&FCFD=0:?&FD00=171\r");
   run_ms(200);
   ok(halt_now() && resume_now(NULL), "a stop and go");
   type("?&FD01=205\r");
   run_ms(200);
   ok(!ram_emulator_window_is_helper() && pi.JIM_ram[0] == 171 && pi.JIM_ram[1] == 205, "writes reach JIM RAM after it: %u %u",
      pi.JIM_ram[0], pi.JIM_ram[1]);
}

/* helpers.c's &FC88 select straight, as the agent's stop (22) and way out (&FD) use it, in the
   cases the BBC can't easily be made to land in: a stop inside a stop, a stop during a transfer
   page, and &FD with no stop. */
static void sel(uint8_t v) { board_bus_write(0xFC88, v); }
static void jim_page(uint32_t page)
{
   board_bus_write(0xFCFD, (uint8_t)(page >> 16));
   board_bus_write(0xFCFE, (uint8_t)(page >> 8));
   board_bus_write(0xFCFF, (uint8_t)page);
}
static void select_paths(void)
{
   puts("== the select: a stop inside a stop, a stop in a transfer, &FD alone");
   boot();
   to_prompt();
   const uint32_t xfer = DISC_RAM_BASE + 0x00D90000u;
   sel(24);
   board_bus_write(0xFD40, 0x99);
   sel(22);
   ok(pi.Memory[256 + 255] == 22, "a stop shows the agent's page");
   board_bus_write(0xFDF1, (uint8_t)~helper_ram[(22 << 8) + 0xF1]);   /* the Pi's mailbox, used */
   sel(22);
   ok(pi.Memory[256 + 0xF1] == helper_ram[(22 << 8) + 0xF1], "a stop inside it: the agent's page afresh, an ordinary stop");
   sel(0xFD);
   ok(pi.Memory[256 + 255] == 22 && ram_emulator_window_is_helper(), "a stop inside it, back: still the agent's page");
   sel(0xFD);
   ok(pi.Memory[256 + 0x40] == 0x99 && ram_emulator_window_is_helper(), "then back: helper 24, as it left it");
   sel(0xFF);

   jim_page(0x0123);
   sel(0xFE);
   ok(pi.page_ram_addr == xfer, "&FE: the transfer page");
   board_bus_write(0xFD00, 0x5A);
   sel(22);
   sel(0xFD);
   ok(pi.page_ram_addr == xfer && !ram_emulator_window_is_helper() && pi.Memory[256] == 0x5A,
      "a stop in it, back: still the transfer page");
   board_bus_write(0xFD01, 0x6B);
   ok(pi.JIM_ram[xfer + 1] == 0x6B, "and writes reach it");
   jim_page(0x0456);
   sel(22);
   sel(0xFD);
   ok(pi.page_ram_addr == 0x045600u && !ram_emulator_window_is_helper(), "the page registers moved, a stop, back: that page");
   sel(0xFF);
   ok(pi.page_ram_addr == 0x012300u, "&FF: the user's page again, &%06X", (unsigned)pi.page_ram_addr);

   sel(24);
   pi.Memory[0x88 + 4] = 0x4C;
   sel(0xFD);
   ok(pi.Memory[0x88 + 4] == 0x60 && !ram_emulator_window_is_helper(), "&FD with no stop: as &FF");
   jim_page(0);
   ok(on_screen(">"), "BASIC never noticed");
}

/* The SD card's 6502code.bin is loaded apart from the kernel: one that isn't this build's must
   leave the BBC alone, and GET /bbc must say what to do. */
static void mismatch(void)
{
   puts("== a 6502code.bin that isn't this build's: the install hooks nothing, and the Pi says why");
   code_bin[(RP_HELPER_AGENT << 8) + RP_VERSION_AT] ^= 0xFFu;
   boot();
   to_prompt();
   uint16_t ev = vec(0x0220), irq2 = vec(0x0206);
   install();
   ok(vec(0x0220) == ev && vec(0x0206) == irq2 && m->ram[0x0D00] != 0x4C && !ramprobe_installed()
      && pi.Memory[RP_FRED_A + RP_IN_AT] == 0x00, "no PHA at rp_in, nothing hooked, not installed");
   ok(pi.Memory[RP_FRED_B] == RP_IDLE && pi.Memory[RP_FRED_C] == rp_fred[RP_FRED_A_LEN + RP_FRED_B_LEN],
      "the rest of the FRED code is there, for an install that doesn't check");
   /* An earlier build's install, which doesn't check: its report says installed. */
   pi.JIM_ram[DISC_RAM_BASE + RP_REPORT + RP_INSTALLED_AT] = RP_INSTALLED;
   ok(!ramprobe_installed() && !ramprobe_halt(RP_RUNG_IRQ) && pi.Memory[RP_HOOK_IRQ2] == RP_IDLE,
      "the Pi still won't stop it: %s", ramprobe_error());
   pi.JIM_ram[DISC_RAM_BASE + RP_REPORT + RP_INSTALLED_AT] = 0;
   ok(on_screen(">") && !ramprobe_halt(RP_RUNG_EVENT) && strstr(ramprobe_error(), "isn't this firmware's"),
      "BASIC carries on; a halt says: %s", ramprobe_error());
   code_bin[(RP_HELPER_AGENT << 8) + RP_VERSION_AT] ^= 0xFFu;
   boot();
   to_prompt();
   install();
   ok(ramprobe_installed(), "with this build's pages again, it installs");
}

static void slow_fiq(void)
{
   puts("== the Pi's FIQ taking its time over the agent's page");
   static const int us[] = {1, 2, 3, 4, 5, 6, 8, 10, 15, 50, 200};
   for (unsigned i = 0; i < sizeof us / sizeof us[0]; i++) {
      select_delay = 0;
      boot();
      to_prompt();
      install();                      /* the install line goes through the firmware's own route */
      select_delay = (uint32_t)us[i] * 2u;
      type("10 A%=A%+1:GOTO 10\r");
      type("RUN\r");
      run_ms(100);
      int lost = 0;
      for (int k = 0; k < 200; k++) {
         run_ms((rnd(200) + 1) / 10.0);
         if (!halt_now() || !resume_now(NULL)) lost++;
      }
      uint32_t a0 = a_percent();
      run_ms(100);
      ok(lost == 0 && a_percent() != a0, "%3d us: 200 stops, %d lost, program %s", us[i], lost, a_percent() != a0 ? "alive" : "DEAD");
   }
   select_delay = 0;
}

/* The routes, as the web server calls them, and as a client uses them: poll until not busy. */
static char api_out[RAMPROBE_API_OUT];
static int post(const char *path, const char *query)
{
   int st = ramprobe_api(true, path, query, api_out, sizeof api_out);
   for (double t = 0; st == 200 && t < 8000 && strstr((ramprobe_api(false, "/bbc", NULL, api_out, sizeof api_out), api_out), "\"busy\":true"); t += 0.5)
      run_ms(0.5);
   return st;
}
/* The bytes that cross the port, with the FIQ running late, as the board's can. */
static void late_fiq(void)
{
   puts("== the Pi's FIQ late on the services port");
   static const int us[] = {1, 2, 5, 6, 8};   /* the board's worst seen is 5 */
   for (unsigned i = 0; i < sizeof us / sizeof us[0]; i++) {
      fiq_delay = 0;
      boot();
      to_prompt();
      install();
      fiq_delay = (uint32_t)us[i] * 2u;
      type("10 A%=A%+1:GOTO 10\r");
      type("RUN\r");
      run_ms(100);
      static uint8_t bytes[800], back[800];
      int wrong = 0, lost = 0;
      for (int k = 0; k < 20; k++) {
         for (unsigned j = 0; j < sizeof bytes; j++) bytes[j] = (uint8_t)(rnd(251) + k);
         run_ms((rnd(100) + 1) / 10.0);
         if (!halt_now()) { lost++; continue; }
         if (!ramprobe_poke(0x3000, bytes, sizeof bytes, RP_RUNG_NONE) || !wait_state(RP_HALTED, 500)) { lost++; continue; }
         if (memcmp(m->ram + 0x3000, bytes, sizeof bytes)) wrong++;
         if (!ramprobe_peek(0x3000, sizeof bytes, RP_NO_BANK, RP_RUNG_NONE) || !wait_state(RP_HALTED, 500)) { lost++; continue; }
         uint32_t got = 0;
         memcpy(back, ramprobe_data(&got), sizeof back);
         if (memcmp(back, bytes, sizeof bytes)) wrong++;
         if (!resume_now(NULL)) lost++;
      }
      ok(wrong == 0 && lost == 0, "%d us: 20 pokes and peeks of 800 bytes, %d wrong, %d lost: %s", us[i], wrong, lost, ramprobe_error());
   }
   fiq_delay = 0;
}

static void over_http(void)
{
   puts("== the HTTP routes");
   boot();
   to_prompt();
   ok(post("/bbc/halt", NULL) == 409 && strstr(api_out, "not installed"), "halt before install: 409, says why");
   install();
   ok(ramprobe_api(false, "/bbc", NULL, api_out, sizeof api_out) == 200 && strstr(api_out, "\"state\":\"running\"") && strstr(api_out, "\"installed\":true"), "GET /bbc: %s", api_out);
   int e = nmi_edges, q = irq_raised;
   ok(post("/bbc/halt", NULL) == 200 && strstr(api_out, "\"state\":\"halted\""), "POST /bbc/halt");
   ok(ramprobe_rung() == RP_RUNG_EVENT && nmi_edges == e && irq_raised == q, "with no rung: rung 0, no NMI, no IRQ");
   ok(strstr(api_out, "\"stopped\":null") != NULL, "no registers: they'd be the OS's");
   ok(post("/bbc/continue", "pc=0x2000") == 409 && ramprobe_state() == RP_HALTED, "continue with a pc: 409, still stopped");
   ok(post("/bbc/continue", NULL) == 200, "and on again");
   ok(post("/bbc/halt", "rung=2") == 400 && strstr(api_out, "nmi=1") && ramprobe_state() == RP_RUNNING && nmi_edges == e,
      "rung 2 without nmi=1: 400, no NMI sent: %s", api_out);
   ok(post("/bbc/peek", "addr=0x3000&len=1&rung=2") == 400 && nmi_edges == e, "a peek at rung 2 without nmi=1: 400 too");
   ok(post("/bbc/halt", "rung=2&nmi=1") == 200 && ramprobe_rung() == RP_RUNG_NMI && nmi_edges > e, "rung 2 with nmi=1: by the NMI");
   ok(post("/bbc/poke", "addr=0x3000&hex=A97760") == 200, "POST /bbc/poke");
   ok(m->ram[0x3000] == 0xA9 && m->ram[0x3002] == 0x60, "it arrived");
   ok(post("/bbc/jsr", "addr=12288") == 200 && strstr(api_out, "\"result\":{\"a\":119"), "POST /bbc/jsr: A=119");
   ok(post("/bbc/peek", "addr=0x3000&len=3") == 200 && strstr(api_out, "\"peeked\":3"), "POST /bbc/peek");
   ok(ramprobe_api(false, "/bbc/data", NULL, api_out, sizeof api_out) == 200 && !strcmp(api_out, "A97760\n"), "GET /bbc/data: %s", api_out);
   ok(ramprobe_api(false, "/bbc/data", "offset=1&len=1", api_out, sizeof api_out) == 200 && !strcmp(api_out, "77\n"), "offset and len");
   ok(post("/bbc/peek", "addr=0xF000&len=0x2000") == 409 && ramprobe_state() == RP_HALTED, "past the top of memory: refused, still stopped");
   ok(post("/bbc/peek", "addr=0xFCA9&len=1") == 409 && strstr(api_out, "I/O"), "a peek of the services port: refused");
   ok(post("/bbc/peek", "addr=0xFE40&len=1") == 409, "of a VIA register: refused");
   ok(post("/bbc/peek", "addr=0xFB00&len=0x101") == 409, "one that runs into &FC00: refused");
   ok(post("/bbc/peek", "addr=0xFB00&len=0x100") == 200 && post("/bbc/peek", "addr=0xFF00&len=0x100") == 200, "&FBxx and &FFxx: read");
   ok(post("/bbc/poke", "addr=0xFE40&hex=00") == 409 && ramprobe_state() == RP_HALTED, "a poke of a VIA register: refused, still stopped");
   ok(post("/bbc/run", "hex=A9428AA8604&x=1") == 400, "odd hex: 400");
   ok(post("/bbc/run", "hex=A94260") == 200 && strstr(api_out, "\"result\":{\"a\":66"), "POST /bbc/run: A=66");
   static const char *const Bad[] = {"addr=", "addr=zz", "addr=0x", "addr=1&len=", "addr=+1&len=1", "addr=1 &len=1", "addr=1&len=1&bank=",  "addr=70000&len=1", "addr=1&len=-1", "len=1", "addr=1", "addr=1&len=1&bank=16", "addr=1&len=99999999999999999999", NULL};
   int bad = 0;
   for (int i = 0; Bad[i]; i++) if (ramprobe_api(true, "/bbc/peek", Bad[i], api_out, sizeof api_out) != 400) { bad++; printf("     accepted: %s\n", Bad[i]); }
   ok(bad == 0 && ramprobe_state() == RP_HALTED, "malformed peeks: 400 each");
   ok(ramprobe_api(true, "/bbc/nothing", NULL, api_out, sizeof api_out) == 0, "an unknown route falls to the web server's 404");
   ok(post("/bbc/continue", "pc=0xZZ") == 400 && ramprobe_state() == RP_HALTED, "bad pc: 400, not let go");
   ok(post("/bbc/continue", NULL) == 200 && strstr(api_out, "\"state\":\"running\""), "POST /bbc/continue");
}

#ifndef BOARD_EXTRA
/* Halt, then continue at a new PC, as `bbcprobe.py halt` then `continue pc=0x2000` does (halt
   tries rung 0 first; POST /bbc/halt with no rung is rung 0).  The code at &2000 records the P
   and S it was handed, then counts in &70.  What a program handed the machine should see: the
   I flag clear and the OS's clock still ticking (interrupts serviced), and S where the program
   was.  Rung 1 does the same as the control.  kc-beeb runs the real MOS 3.20. */
static void new_pc(void)
{
   static const int rungs[] = {RP_RUNG_IRQ, RP_RUNG_EVENT};
   static const uint8_t probe[] = {
      0x08, 0x68, 0x85, 0x71,     /* PHP : PLA : STA &71   the P it was handed */
      0xBA, 0x86, 0x72,           /* TSX : STX &72         the S */
      0xE6, 0x70, 0x4C, 0x07, 0x20 /* .l INC &70 : JMP l */
   };
   for (unsigned k = 0; k < 2; k++) {
      int rung = rungs[k];
      printf("== halt by rung %d, then continue at &2000\n", rung);
      boot();
      to_prompt();
      install();
      type("10 REPEAT:A%=A%+1:UNTIL 0\r");
      type("RUN\r");
      run_ms(200);
      bool h = ramprobe_halt(rung) && wait_state(RP_HALTED, 400);
      rp_regs_t st = *ramprobe_stopped();
      printf("     stopped: PC=&%04X S=&%02X P=&%02X A=&%02X  (rung %d)\n", st.pc, st.s, st.p, st.a, ramprobe_rung());
      static const uint8_t zero[3] = {0, 0, 0};
      bool poked = h && ramprobe_poke(0x2000, probe, sizeof probe, RP_RUNG_NONE) && wait_state(RP_HALTED, 200)
                     && ramprobe_poke(0x0070, zero, 3, RP_RUNG_NONE) && wait_state(RP_HALTED, 200);
      rp_resume_t go = { .set_pc = true, .pc = 0x2000 };
      bool resumed = poked && resume_now(&go);
      if (!resumed) {
         ok(poked && strstr(ramprobe_error(), "rung 0") != NULL, "rung %d: continue at a new PC refused: %s", rung, ramprobe_error());
         ok(resume_now(NULL), "a plain continue");
         run_ms(200);
         continue;
      }
      run_ms(100);
      uint8_t clk0[10];
      memcpy(clk0, &m->ram[0x0292], 10);
      uint8_t c0 = m->ram[0x70];
      run_ms(500);
      bool ticking = memcmp(clk0, &m->ram[0x0292], 10) != 0;
      uint8_t p = m->ram[0x71], s = m->ram[0x72];
      printf("     at &2000: P=&%02X (I %s)  S=&%02X  counting %s  OS clock %s  CPU PC=&%04X\n",
             p, (p & 4) ? "SET" : "clear", s, m->ram[0x70] != c0 ? "yes" : "NO", ticking ? "ticking" : "STOPPED", m->cpu.pc);
      ok(m->ram[0x70] != c0 && !(p & 4) && ticking,
         "rung %d: the code at &2000 runs with interrupts on and the OS's clock ticking", rung);
   }
}

int main(int argc, char **argv)
{
   roms = argc > 1 ? argv[1] : ".";
   session();
   ladder();
   late_fiq();
   over_http();
   new_pc();
   if (getenv("RAMPROBE_QUICK") == NULL) {
   stress();
   through_the_os();
   services_port();
   what_stops_it();
   break_key();
   helper_stop();
   select_paths();
   mismatch();
   slow_fiq();
   }
   printf("\n%d checks, %d failures\n", checks, fails);
   if (fails) return 1;
   puts("RAM PROBE TESTS PASSED");
   return 0;
}
#endif
