#-------------------------------------------------------------------------
# VideoCore IV implementation of 1MHz handler
#-------------------------------------------------------------------------

# on entry
# GPIO pins setup by arm
# Addresses passed into vc are VC based
# gpfsel_data_idle setup

#  r0 - pointer to shared memory ( VC address) of 1MHz registers
#  r1 - pointer to data to xfer to ARM
#  r2 - nOE pin
#  r3 - data outputs
#  r4 - debug output control
#  r5 - debug pin mask (0 = no debug  xx= debug pin e.g 1<<21)

# Internal register allocation
#  r0 - pointer to shared memory ( VC address) of tube registers
#  r1 - pointer to data to xfer to ARM
#  r2 - External nOE pin
#  r3 - Databus and test pin output select
#  r4 - debug output control
#  r5 - debug pin mask (0 = no debug  xx= debug pin e.g 1<<21)
#  r6 - GPFSEL0 constant
#  r7 -
#  r8 - temp
#  r9 -
# r10 -
# r11 -
# r12 - GPIO pins value
# r13 - pointer to doorbell register
# r14 -
# r15 -

# GPIO registers
.equ GPFSEL0,       0x7e200000
.equ GPFSEL0_offset, 0
.equ GPSET0_offset, 0x1C
.equ GPCLR0_offset, 0x28
.equ GPLEV0_offset, 0x34
.equ GPEDS0_offset, 0x40

# Post ring (docs/dev/bus-post-ring.md).  Each bus cycle's GPIO sample goes
# into one of 8 consecutive registers (SMI DSR0..DSW3, 0x7E600010..2C) with
# a 5-bit sequence tag in bits 27-31 (GPIO 27 is CLK, which the ARM never
# reads from the sample; GPIO 28-31 are not on the bus), then
# the doorbell rings.  The ARM FIQ acknowledges the bell first and drains
# every slot whose tag matches its own sequence.  Nothing here reads a
# register: two stores per post, as before.
#  r1  - ring base, passed by the ARM at launch (it was the single post word
#        before the ring, so the launch parameter did not change)
#  r10 - the next tag, pre-shifted to bits 27-31 (add TAG_STEP to advance);
#        bits 27-29 are the slot, bits 30-31 the lap, so the ARM can tell
#        "last lap, not written yet" from "the VPU is a lap ahead"
#  r11 - word index of the next slot, 0..7 (st rX,(ra,rb) scales rb by 4,
#        like ld r8,(r0,r8) below)
#  The sample's GPIO 27-31 are dropped with extu rX,27 before the tag goes in.
.equ TAG_STEP,  0x08000000

# fixed pin bit positions ( TEST passed in dynamically)
.equ nRST,         26
.equ nPCFD,        25
.equ nPCFC,        24

.equ RnW,          10
.equ CLK,          27
.equ DATASHIFT,    2
.equ ADDRBUS_SHIFT, (16)
.equ OUTPUTBIT,   (15)

.equ ADDRESSBUS_WIDTH, (8 + 1)
.equ DATABUS_WIDTH, 8

.equ NPCFC_MASK,    (1<<nPCFC)

.equ Pi1MHz_MEM_RNW, (1<<9)

.equ GPU_ARM_DBELL, 0x7E00B844

.org 0                     # NB all code relative

   di                      # disable interrupts
   or     r3, r4           # add in test pin so that it is still enabled
   mov    r6, GPFSEL0

   mov    r13, GPU_ARM_DBELL
   mov    r10, 0                 # post ring: first tag is 0 (the ARM starts at sequence 0)
   mov    r11, 0                 # ... first slot (the base is r1, from the ARM)
   cmp    r2, 0
   bne    use_nOE
   BEQ    Poll_loop

# poll for nPCFC or nPCFD being low
.balignw 16,1 # Align with nops
Poll_loop:
   # st     r5, GPCLR0_offset(r6)  # Turn off debug signal

Poll_access_low:
   ld     r12, GPLEV0_offset(r6)  # loop until we see FRED or JIM low

   btst   r12, nPCFC
   btstne r12, nPCFD
   bne    Poll_access_low

   # st     r5, GPSET0_offset(r6)  # Debug pin

   btst   r12, CLK
   beq    waitforclkhigh

waitforclklow:                   # wait for extra half cycle to end
   ld     r12, GPLEV0_offset(r6)
   btst   r12, nPCFC
   btstne r12, nPCFD
   bne    Poll_loop

   btst   r12, CLK
   bne    waitforclklow

.balignw 16,1 # Align with nops
waitforclkhigh:
waitforclkhighloop:
   LSR    r8, r12,ADDRBUS_SHIFT+1
   ld     r12, GPLEV0_offset(r6)
   extu   r8, ADDRESSBUS_WIDTH-1   # bmask Isolate address bus
   ld     r8, (r0,r8)            # get byte to write out

   btst   r12, CLK
   beq    waitforclkhighloop

# seen rising edge of CLK
# so address bus has now been setup

   btst   r12, nPCFC
   btstne r12, nPCFD
   bne    Poll_loop

# check if we are in a read or write cycle
# we do this here while the read above is stalling

   btst   r12, RnW
  # lsl    r8, DATASHIFT
   beq    writecycle

   btst   r12, ADDRBUS_SHIFT     # select which 16bits hold the data
   lsrne  r8, 16 - DATASHIFT     # High 16 bits to low 16 bits with databus shift
   lsleq  r8, DATASHIFT          # low 16 bits with databus shift

  # btst   r8, OUTPUTBIT
   extu   r8, DATABUS_WIDTH + DATASHIFT      # bmask isolate the databus NB lower bit are already zero form above

   st     r8, GPSET0_offset(r6)  # set up databus
   st     r3, GPFSEL0_offset(r6) # set databus to output
   extu   r12, 27                # post: drop GPIO 27-31 ...
   or     r12, r10               # ... tag it ...
   st     r12, (r1, r11)        # ... store the entry (rb scaled by 4, as ld r8,(r0,r8) above) ...
   st     r12, (r13)             # ... ring the doorbell
   add    r10, TAG_STEP          # next tag (wraps through bit 31)
   add    r11, 1
   extu   r11, 3                 # next slot, 0..7

waitforclklow2loop:
   ld     r12, GPLEV0_offset(r6)
   btst   r12, CLK
   bne    waitforclklow2loop

   st     r4, GPFSEL0_offset(r6) # data bus to inputs except debug
   st     r8, GPCLR0_offset(r6)  # clear databus low

   b      Poll_loop

.balignw 16,1 # Align with nops
writecycle:
waitforclkloww2:
   mov    r8,r12
   ld     r12, GPLEV0_offset(r6)
   btst   r12, CLK
   bne    waitforclkloww2

   extu   r8, 27                 # post: drop GPIO 27-31, tag, store, ring (see the read path)
   or     r8, r10
   st     r8, (r1, r11)
   st     r8, (r13)              # ring doorbell
   add    r10, TAG_STEP
   add    r11, 1
   extu   r11, 3
   b      Poll_loop


#
# Same as above but with nOE pin ( only for system without the screen enabled
#
use_nOE:
   mov    r9, GPCLR0_offset>>2
.balignw 16,1 # Align with nops

nOE_Poll_loop:
   st     r5, GPCLR0_offset(r6)  # Turn off debug signal

nOE_Poll_access_low:
   ld     r12, GPLEV0_offset(r6)  # loop until we see FRED or JIM low

   btst   r12, nPCFC
   btstne r12, nPCFD
   bne    nOE_Poll_access_low

   st     r5, GPSET0_offset(r6)  # Debug pin

   btst   r12, CLK
   beq    nOE_waitforclkhigh

nOE_waitforclklow:                   # wait for extra half cycle to end
   ld     r12, GPLEV0_offset(r6)
   btst   r12, nPCFC
   btstne r12, nPCFD
   bne    nOE_Poll_loop

   btst   r12, CLK
   bne    nOE_waitforclklow

.balignw 16,1 # Align with nops
nOE_waitforclkhigh:
nOE_waitforclkhighloop:
   LSR    r8, r12,ADDRBUS_SHIFT+1
   ld     r12, GPLEV0_offset(r6)
   extu   r8, ADDRESSBUS_WIDTH-1   # bmask Isolate address bus
   ld     r8, (r0,r8)            # get byte to write out

   btst   r12, CLK
   beq    nOE_waitforclkhighloop

# seen rising edge of CLK
# so address bus has now been setup

   btst   r12, nPCFC
   btstne r12, nPCFD
   bne    nOE_Poll_loop

# check if we are in a read or write cycle
# we do this here while the read above is stalling

   btst   r12, RnW
   #lsl    r8, DATASHIFT
   beq    nOE_writecycle

   btst   r12, ADDRBUS_SHIFT     # select which 16bits hold the data
   lsrne  r8, 16 - DATASHIFT     # High 16 bits to low 16 bits with databus shift
   lsleq  r8, DATASHIFT          # low 16 bits with databus shift


   btst   r8, OUTPUTBIT
   extu   r8, DATABUS_WIDTH + DATASHIFT      # bmask isolate the databus NB lower bit are already zero form above

   st    r8, GPSET0_offset(r6)  # set up databus
   st    r3, GPFSEL0_offset(r6) # set databus to output
   beq   bytenotwrittento
   st    r2,(r6,r9)             # set external output enable low ( only if it has been written to)
bytenotwrittento:
   extu  r12, 27                # post: drop GPIO 27-31, tag, store, ring (see the first read path)
   or    r12, r10
   st    r12, (r1, r11)
   st    r12, (r13)             # ring doorbell
   add   r10, TAG_STEP
   add   r11, 1
   extu  r11, 3

.balignw 4,1 # Align with nops
nOE_waitforclklow2loop:
   ld    r12, GPLEV0_offset(r6)
   btst  r12, CLK
   bne   nOE_waitforclklow2loop

   st    r2, GPSET0_offset(r6)  # set external output enable high
   st    r4, GPFSEL0_offset(r6) # data bus to inputs except debug
   st    r8, GPCLR0_offset(r6)  # clear databus low

   b      nOE_Poll_loop

nOE_writecycle:
   st     r2, GPCLR0_offset(r6)  # set external output enable low
nOE_waitforclkloww2:
   mov    r8,r12
   ld     r12, GPLEV0_offset(r6)
   btst   r12, CLK
   bne    nOE_waitforclkloww2

   extu   r8, 27                 # post: drop GPIO 27-31, tag, store, ring (see the first read path)
   or     r8, r10
   st     r8, (r1, r11)
   st     r8, (r13)              # ring doorbell
   st     r2, GPSET0_offset(r6)  # set external output enable high
   add    r10, TAG_STEP
   add    r11, 1
   extu   r11, 3
   b      nOE_Poll_loop