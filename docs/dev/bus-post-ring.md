# Bus post ring: a FIFO between the VPU bus handler and the ARM FIQ

STATUS (2026-09-10 21:00): **committed (6839666 ring, 4eccb14 launch once,
a07fed8 BREAK row) and on the bench SD.**  5-bit tag; VPU launched ONCE;
no re-read.  Soaked 10 h / 87k F2 presses clean.  Then CTRL-BREAK failed
~60% of the time: TAG_LAUNCH_VPU1 against a running VPU1 does not reliably
restart it, so after a reset the old code carried on with its old tag while
the ARM had reset C to 0 (the DEBUG Ring row decodes it exactly: each
failed boot's tags continue from the previous boot's end).  Fix: launch
once at boot, never on a reset; the ring flows across BREAK.  12/12 BREAKs
pass after the fix.  A first-slot re-read was tried on the theory that the
entry lands after its bell; a measurement build (64-read budget, histogram
of reads needed) found no hit within four reads - every hit was the Beeb's
next cycle arriving - so it was removed again.  The BREAK row (zero-cost
stamps) stays in release; the Ring row is DEBUG-only.

## The fault this closes

The VPU services every 1MHz bus cycle and hands each one to the ARM by
writing the GPIO sample into ONE word (`Pi1MHz_VPU_RETURN`, SMI offset 0x10)
and ringing ARM doorbell 1.  The FIQ reads the word, then reads the doorbell
register to acknowledge.  Nothing waits for anything: a second post that
rings inside the FIQ's ring-to-acknowledge window (entry latency plus the
data read plus the doorbell read, about 1.5 us) overwrites the word and has
its bell swallowed by that acknowledge, so it is never dispatched.

Measured with the counter builds (VPU counts posts in a register, the FIQ
counts what it received): **3 posts lost per Beeb session, twice**, with the
worst clean FIQ entry latency 1 us over 88k writes and the longest FIQ
callback 5 us (at the Beeb's boot).  No long masked region exists; two
Beeb stores to FRED on the stretched bus are 2-3 us apart, which is the
whole margin.  A lost VDU byte desynchronises the Pi's VDU parser (a VDU
24's parameters ran as commands, one of them VDU 4, and every label went to
the top row) or silently diverges its window state.

An ARM-only re-read of the word after the acknowledge was tried and is
**not** viable: the posted word is the raw GPIO sample and pins outside the
bus (PWM audio, UART, test pin) change on their own, so "the word changed"
fired continuously and duplicated bytes.

## Design: tagged entries, no index registers

A ring of N = 8 entries.  Each entry is the GPIO sample word with a 5-bit
sequence tag in bits 27-31, which the ARM never uses (GPIO 27 is CLK, read
only by the VPU; GPIO 28-31 are not on the Zero's header; the existing
masks on the ARM side already ignore them).  The tag is the low 5 bits of
the post sequence number: bits 27-29 are the slot, bits 30-31 a 2-bit lap
count.  Two lap bits are the minimum: with one, "this slot still holds last
lap's entry" (the normal not-yet-written state) and "the producer is a lap
ahead" are the same bit flip, which is what made the first build count
every empty slot as an overrun and, with the skip-8 it then did, loop in
the FIQ re-dispatching stale entries (the hang on Beeb traffic).  There is no producer index register and no consumer
index register: the tag in the entry says whether the slot has been written
for the lap the consumer expects.

```
E[0..7]     eight consecutive 32-bit registers
entry       (sample & 0x07FFFFFF) | (seq & 31) << 27
slot        seq & 7
```

Accesses per post: VPU **2 stores** (entry, bell) - the same as today's
data store and bell.  FIQ per invocation: 1 doorbell read (as today) plus
one entry read per pending entry plus one read that finds the first
unwritten slot (no re-read: see "Why acknowledging first" below).  For a single post that is 2 entry reads against today's
1 data read, and that extra read is on the ARM, which is not the side
that must be back at the GPIO poll within a microsecond.

### Producer (VPU)

Keep two registers across the loop: `rtag`, the sequence tag already
shifted to bits 28-31, and `rslot`, the address of the next entry.  Both
start at the ring base / tag 0 at launch.

```
   # word already assembled in r8 (write cycle) or r12 (read cycle)
   # r1 = ring base, passed by the ARM at launch (the same parameter that carried the
   # single post word), r10 = tag pre-shifted to bits 27-31, r11 = slot word index 0..7
   extu   r8, 27                # drop GPIO 27-31 (keep the low 27 bits)
   or     r8, r10               # tag it
   st     r8, (r1, r11)         # the entry - ONE store; (ra,rb) scales rb by 4, as ld r8,(r0,r8)
   st     r8, (r13)             # the bell - as today
   add    r10, 0x08000000       # next tag; the carry out of bit 31 is the wrap
   add    r11, 1
   extu   r11, 3                # next slot, 0..7
```
That is what `vidcore/Pi1MHzvc.s` now carries at all four post sites: five
16-bit ALU ops and the same two stores, no loads.

Three ALU ops (mask, or, tag increment) plus the slot advance, and NO
loads.  Order for the write cycle: the entry store can be issued BEFORE the
wait for CLK low - the sample is complete once CLK-high was seen - so its
posted-write latency overlaps the wait; only the bell must follow the entry.
For the read cycle the post already happens after the bus has been driven;
the added ALU ops are a few nanoseconds against the ~1 us before the next
cycle can start.  Nothing on the hot path reads a register, so no VPU
stall is added; the two stores are the same two posted writes as today.

The same sequence goes at all four post sites (read and write, `Poll_loop`
and `nOE_Poll_loop`).

No overflow test on the VPU: it would need a read.  Overflow is detected
by the consumer from the lap bit (below).

### Consumer (ARM FIQ)

`src/FIQ.s`.  FIQ mode has its own r8-r14, so nothing is saved or restored
and the constants live in registers from one FIQ to the next:

| reg | holds |
|---|---|
| r10 | C, the tag of the next entry to consume, pre-shifted to bits 27-31 exactly like the VPU's r10 (bits 27-29 = slot, bits 30-31 = lap) |
| r11 | ring base |
| r12 | the address-bus mask for the callback table (saved round the callback, C code treats r12 as scratch) |
| r8, r9 | temps; r9 is zero on entry and exit (the DMB takes a zero register) |

Keeping C pre-shifted makes each step one instruction: the slot address is
`and r8, r10, #0x38000000` then `ldr r8, [r11, r8, LSR #25]` (the LSR turns
the slot in bits 27-29 into slot * 4), the tag test is `sub r9, r8, r10` +
`tst r9, #0xF8000000` (the difference serves the lap test too), and the
advance is `add r10, r10, #0x08000000`.

```
FIQstart:
   DMB                              # r9 is zero: every exit leaves it so
   and  r8, r10, #0x38000000        # first slot index, early: the shifted index below never interlocks
   read doorbell into r9            # ACKNOWLEDGE FIRST: the data is in the ring, not the bell;
                                    # the value is unused and r9 is not written again until
                                    # the entry has arrived, so nothing waits on this read
drain:
   ldr  r8, [r11, r8, LSR #25]      # the entry: the one peripheral read per entry
   sub  r9, r8, r10                 # tag difference (no borrow: C's low 27 bits are 0)
   tst  r9, #0xF8000000             # tag matches C?
   bne  notmine
   ... RnW test, address mask (r12), callback-pointer load ...
   add  r10, r10, #0x08000000       # C += 1, in the callback-pointer load's shadow
   cmp  r9, #0
   andeq r8, r10, #0x38000000       # no callback: next slot index, and round again
   beq  drain
   push ; r0 = r8 >> 2 ; and r8, r10, #0x38000000 (next slot, survives the call) ; blx ; pop ; b drain
notmine:
   eor  r9, r9, #0xC0000000         # lap difference 3 = last lap's entry, not written yet ...
   ands r9, r9, #0xF8000000         # ... leaves r9 zero ...
   bne  overrun                     # 1 or 2 laps ahead: count it, C stays, the stream resyncs
done:
   DMB ; return                     # ... the common exit falls through, r9 already zero for the DMB
```

Five instructions from entry load to dispatch decision, no stack traffic,
no loads other than the entry itself and the callback pointer.  The slot
index is always computed at least two instructions before the load that
uses it as a shifted index (ARM11 reads a shifted index register early).

Overrun: no skip.  The counter is per FIQ that met the lap, not per
event, and the stream resyncs by itself once the producer's tag comes
round (up to 32 posts lost).  The count is on the /status Bus diag row as
`ovr`.  The first build's 4-bit tag could not tell last lap from next lap
and counted 974 "overruns" on one Beeb boot; the 5-bit tag reads 0.

Why acknowledging first is correct: a post that lands after the
acknowledge rings again and re-enters after we return; a post that landed
before it is in the ring and the drain reads it.  The old order existed
only because the data lived in the bell's word.  The VPU stores the entry
before it rings, and the entry is there by the time the FIQ reads it: a
measurement build re-read an unwritten slot up to 64 times, at the first
slot of a FIQ and at the end of a drain, and binned the reads each hit
needed (1, 2, 3-4, 5-16, 17-64).  First slot: 0 0 0 0 7.  Drain end, over
~16,000 hits: 80 207 1068 12275 2812 - a smooth arrival-time curve peaking
at 1-2 us, the Beeb's next cycle, with no spike at one or two reads.  So
there is no store-after-bell window to cover and the handler does not
re-read; a re-read only loiters in the FIQ until the next post.

The drain runs until it meets an unwritten slot.  It cannot stay resident
for ever: the bus delivers at most one post per microsecond and a pass of
the loop is well under that, so the consumer always catches up and finds
the empty slot.  (A drain bound with a self re-ring was built and removed:
it cost a stack frame and a reload per entry, and whether an ARM-side
write to 0x2000B844 raises the ARM's own doorbell was never established.)

Not a peripheral register for C: the first entry's address depends on C,
so a C read from SMI L or DC would be a stalled read that the entry read
has to wait behind, on every FIQ.  If something else needs to see C
(/status), mirror it with one posted write after the drain - never a read.

The dispatch itself does not change: callbacks still get
`word >> DATABUS_SHIFT` and use `GET_DATA` / `GET_ADDR`; the tag bits sit
above everything they mask.  Audit: any callback that looks at raw bits
above the address field would see the tag - none is known to.

### Registers

Requirements: 32-bit, all bits read/write, no side effects on read or
write, untouched by the firmware while we run, on the peripheral bus for
both cores.  The single word today is SMI `DSR0` (block 0x7E600000, offset
0x10), which shows the SMI block is free for this.  Full-width, plain
configuration registers there:

| offset | name | width | note |
|---|---|---|---|
| 0x10 - 0x2C | DSR0, DSW0, DSR1, DSW1, DSR2, DSW2, DSR3, DSW3 | 32 | eight consecutive words: **the ring** |
| 0x04, 0x30 | L, DC | 32 | spare, e.g. an overrun count if wanted (check DC's reserved bits) |
| 0x0C, 0x3C | D, DD | 32 | avoid: data registers can start a transfer if SMI is ever enabled |
| 0x08, 0x38 | A, DA | 8 | too narrow |

Verify each register on the bench before trusting it: write a pattern from
the VPU, read it back on the ARM, all 32 bits, every register.  The block
at `Pi1MHz_MEM_BASE` did not return what was stored (memory note
`vpu-memory-word-access-only`), so this is not a formality.  Confirm SMI's
enable bit (CS bit 0) stays clear so D/DD/A never act.  Fallbacks if the
SMI block cannot supply eight clean words: the ARM control block's
semaphores (0x7E00B800), or a NOINIT line in ARM RAM via the 0x4 alias -
but RAM costs the FIQ an L1 invalidate per entry, and the VPU's store may
land after its bell (seen in the counter builds); the tag protocol
tolerates that, the stall budget does not.

### Initialisation and BREAK

The VPU is launched ONCE, at boot, under one FIQ mask: fill the eight
entries with their own slot number and lap 3 (`(s | 24) << 27`, which the
consumer starting at lap 0 reads as "last lap's entry, not written yet"),
set the FIQ's C to 0 with `_fiq_set_consumer(0)`, launch, enable the
doorbell FIQ, unmask.  `init_emulator()` runs again on every Beeb reset
and must NOT repeat any of that: TAG_LAUNCH_VPU1 against a VPU1 already
running the poll loop does not reliably restart it (measured: the old code
carried on with its old tag while C had been reset to 0, and the ROM's
helper select after CTRL-BREAK was lost about 60% of the time).  Nothing
the launch passes changes on a reset, so the ring simply keeps flowing
across it while the callback table is rebuilt underneath.

A kernel.now chain-boot also arrives with the VPU running the previous
kernel's handler, and it cannot be relaunched either.  The outgoing kernel
leaves a marker in .noinit (`RPI_ChainBootMark`, consumed by kernel_main),
and a chain-booted `init_emulator` neither fills the ring nor launches: it
reads the eight tags and seeds C from them.  The producer writes the slots
in order, so the one place round the ring where the lap drops by one is its
position; the next entry is that slot with the previous slot's lap, or slot
0 with the next lap when every slot carries the same one (which is also
what a fresh fill reads as, so the rule gives the cold-boot answer too).
Verified on the Zero 2 W with the DEBUG Ring row: producer at 10 before the
kernel.now, C seeded to 11 after it, next BREAK in step, no overruns.

### Validation

For the test build keep a VPU-side post count (r-register, stored once
per post into a spare register only in the test build) and the FIQ's C:
**their difference must read 0 for a whole session**, where the single
word lost 3.  Keep the `/vdulog` artefact count
(`grep -cE '^[0-9]+ [89A-F][0-9A-F] *$'`) at 0 and the overrun count at 0
across the National disc sequences that reproduced the loss (picture pages
with stills, caption and bar draws).  Then strip the test store; the
release row is "dispatched N overruns K", read on demand.

### Alternatives considered

- Producer/consumer index registers instead of tags: one more VPU store per
  post and a stalled index read per FIQ; only worth it if 4 spare bits in
  the sample cannot be guaranteed.
- VPU waits for the ARM to consume before posting: needs a VPU read in the
  hot path and risks the next bus cycle.  No.
- ARM-only re-read of the single word: invalid, see above.

### What it does not fix

Two identical consecutive posts are fine here (sequence-based, not
value-based).  What it cannot fix is the VPU missing a bus cycle
altogether; the counter builds saw no evidence of that (posted counts
matched the Beeb's traffic), so it is not on the list.
