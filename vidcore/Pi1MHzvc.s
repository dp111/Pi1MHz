#-------------------------------------------------------------------------
# VideoCore IV implementation of 1MHz handler
#-------------------------------------------------------------------------

# on entry
# GPIO pins setup by arm
# Addresses passed into vc are VC based
# gpfsel_data_idle setup

#  r0 - pointer to shared memory ( VC address) of 1MHz registers
#  r1 - pointer to data to xfer to ARM
#  r2 -
#  r3 - data outputs
#  r4 - debug output control
#  r5 - debug pin mask (0 = no debug  xx= debug pin e.g 1<<21)

# Intenal register allocation
#  r0 - pointer to shared memory ( VC address) of tube registers
#  r1 - pointer to data to xfer to ARM
#  r2 - unused
#  r3 -
#  r4 - debug output control
#  r5 - debug pin mask (0 = no debug  xx= debug pin e.g 1<<21)
#  r6 - GPFSEL0 constant
#  r7 -
#  r8 - temp
#  r9 - r9 Databus and test pin output select
# r10 - address mask
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

# fixed pin bit positions ( TEST passed in dynamically)
.equ nRST,         26
.equ nPCFD,        25
.equ nPCFC,        24

.equ RnW,          10
.equ CLK,          27
.equ DATASHIFT,    2
.equ ADDRBUS_SHIFT,    (16)

.equ D7_PIN,       (9)
.equ D6_PIN,       (8)
.equ D5_PIN,       (7)
.equ D4_PIN,       (6)
.equ D3_PIN,       (5)
.equ D2_PIN,       (4)
.equ D1_PIN,       (3)
.equ D0_PIN,       (2)

.equ ADDRBUS_MASK,  (0xFF<<ADDRBUS_SHIFT)
.equ NPCFC_MASK,    (1<<nPCFC)

.equ Pi1MHz_MEM_RNW, (1<<9)

.equ GPU_ARM_DBELL, 0x7E00B844

.org 0

# disable interrupts

  di
   or     r9, r3, r4       # add in test pin so that it is still enabled
   mov    r6, GPFSEL0
   mov    r10,((ADDRBUS_MASK>>ADDRBUS_SHIFT) | (NPCFC_MASK>>ADDRBUS_SHIFT))>>1
   mov    r13, GPU_ARM_DBELL
   b Poll_loop

# poll for nPCFC or nPCFD being low
.align 4
Poll_loop:
   st     r5, GPCLR0_offset(r6)  # Turn off debug signal
Poll_access_low:
   ld     r12, GPLEV0_offset(r6)  # loop until we see FRED or JIM low

   btst   r12, nPCFC
   btstne r12, nPCFD
   bne    Poll_access_low

   st     r5, GPSET0_offset(r6)  # Debug pin

   btst   r12, CLK
   beq    waitforclkhigh

waitforclklow:                   # wait for extra half cycle to end
   ld     r12, GPLEV0_offset(r6)
   btst   r12, nPCFC
   btstne r12, nPCFD
   bne    Poll_loop

   btst   r12, CLK
   bne    waitforclklow

waitforclkhigh:
.align 4
   b waitforclkhighloop
waitforclkhighloop:
   ld     r12, GPLEV0_offset(r6)
   btst   r12, CLK
   beq    waitforclkhighloop

# seen rising edge of CLK
# so address bus has now been setup

   LSR    r8, r12,ADDRBUS_SHIFT+1

   and    r8, r10           # Isolate address bus

   ld     r8,(r0,r8) # get byte to write out
   btst   r12, nPCFC
   btstne r12, nPCFD
   bne    Poll_loop
# check if we are in a read or write cycle
# we do this here while the read above is stalling
   btst   r12, RnW
   beq    writecycle

   btst   r12,ADDRBUS_SHIFT
   lsrne  r8,16-DATASHIFT
   lsleq  r8,DATASHIFT

   and    r8,255<<DATASHIFT

   st     r8, GPSET0_offset(r6)  # set up databus
   st     r9, GPFSEL0_offset(r6)                # set databus to outputs
   st     r12, (r1)         # post data
   st     r12, (r13)             # ring doorbell

   b waitforclklow2loop

.align 4
waitforclklow2loop:
   ld     r12, GPLEV0_offset(r6)
   btst   r12, CLK
   bne    waitforclklow2loop
   st     r4, GPFSEL0_offset(r6)               # data bus to inputs except debug

   mov    r8, (0xFF<<DATASHIFT)
   st     r8, GPCLR0_offset(r6)

   b Poll_loop


writecycle:

waitforclkloww2:
   ld     r8, GPLEV0_offset(r6)
   btst   r8, CLK
   movne  r12,r8
   bne    waitforclkloww2
   st     r12, (r1)         # post data
   st     r12, (r13)        # ring doorbell

   b Poll_loop


.if 0
printbyte:
   mov    r1,0x7e215040

   lsr   r2,r12,28
   and   r2,0xf
   add   r2,'0'
   cmp   r2,'9'
   addgt   r2,r2,'A'-'9'-1
   stb   r2,(r1)

   lsr   r2,r12,24
   and   r2,0xf
   add   r2,'0'
   cmp   r2,'9'
   addgt   r2,r2,'A'-'9'-1
   stb   r2,(r1)

   lsr   r2,r12,20
   and   r2,0xf
   add   r2,'0'
   cmp   r2,'9'
   addgt   r2,r2,'A'-'9'-1
   stb   r2,(r1)

   lsr   r2,r12,16
   and   r2,0xf
   add   r2,'0'
   cmp   r2,'9'
   addgt   r2,r2,'A'-'9'-1
   stb   r2,(r1)

   lsr   r2,r12,12
   and   r2,0xf
   add   r2,'0'
   cmp   r2,'9'
   addgt   r2,r2,'A'-'9'-1
   stb   r2,(r1)

   lsr   r2,r12,8
   and   r2,0xf
   add   r2,'0'
   cmp   r2,'9'
   addgt   r2,r2,'A'-'9'-1
   stb   r2,(r1)

   lsr   r2,r12,4
   and   r2,0xf
   add   r2,'0'
   cmp   r2,'9'
   addgt   r2,r2,'A'-'9'-1
   stb   r2,(r1)

   lsr   r2,r12,0
   and   r2,0xf
   add   r2,'0'
   cmp   r2,'9'
   addgt   r2,r2,'A'-'9'-1
   stb   r2,(r1)
   rts
.endif