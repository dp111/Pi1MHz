#-------------------------------------------------------------------------
# VideoCore IV implementation of 1MHz handler 
#-------------------------------------------------------------------------

# on entry
# GPIO pins setup by arm
# Addresses passed into vc are VC based
# gpfsel_data_idle setup

#  r0 - pointer to shared memory ( VC address) of 1MHz registers
#  r1 - 
#  r2 - 
#  r3 - 
#  r4 - debug output control
#  r5 - debug pin mask (0 = no debug  xx= debug pin e.g 1<<21)

# Intenal register allocation
#  r0 - pointer to shared memory ( VC address) of tube registers
#  r1 - unused
#  r2 - unused
#  r3 - ARM_GPU_MAILBOX constant
#  r4 - scratch
#  r5 - debug pin mask (0 = no debug  xx= debug pin e.g 1<<21)
#  r6 - GPFSEL0 constant
#  r7 -
#  r8 - GPIO pins value
#  r9 - 
# r10 - 
# r11 -
# r12 - 
# r13 -
# r14 - 
# r15 -

# GPIO registers
.equ GPFSEL0,       0x7e200000
.equ GPSET0_offset, 0x1C
.equ GPCLR0_offset, 0x28
.equ GPLEV0_offset, 0x34
.equ GPEDS0_offset, 0x40

.equ GPU_ARM_MBOX, 0x7E00B880

# fixed pin bit positions ( TEST passed in dynamically)
.equ nRST,         26
.equ nPCFD,        25
.equ nPCFC,        24

.equ RnW,          10
.equ CLK,          17
.equ DATASHIFT     2
.equ ADDRSHUFT     8+2

.equ D7_PIN,       (9)
.equ D6_PIN,       (8)
.equ D5_PIN,       (7)
.equ D4_PIN,       (6)
.equ D3_PIN,       (5)
.equ D2_PIN,       (4)
.equ D1_PIN,       (3)
.equ D0_PIN,       (2)

.equ Pi1MHz_MEM_RNW (1<<9)

.equ MAGIC_,     ((1 << (D0_PIN * 3)) | (1 << (D1_PIN * 3)) | (1 << (D2_PIN * 3)) | (1 << (D3_PIN * 3)) |(1 << (D4_PIN * 3)) | (1 << (D5_PIN * 3)) |(1 << (D6_PIN * 3)) | (1 << (D7_PIN * 3)))

  # mail box flags
.equ ATTN_MASK,    31
.equ OVERRUN_MASK, 30

.equ RESET_MAILBOX_BIT, 12
.equ RW_MAILBOX_BIT, 11

.equ GPU_ARM_DBELL, 0x7E00B844  

.org 0

# disable interrupts
  di
   mov    r9, (0xFF<<DATASHIFT) # all the databus
   or     r9, r5       # add in test pin so that it is cleared at the end of the access

   mov    r10, #(ADDRBUS_MASK>>ADDRBUS_SHIFT)  | (NPCFC_MASK>>ADDRBUS_SHIFT)

# r1, r3, r4 now free

   mov    r13, GPU_ARM_DBELL

   mov    r6, GPFSEL0
   b Poll_loop
# poll for nPCFC or nPCFD being low
.align 4
Poll_loop:

Poll_access_low:
   ld     r8, GPLEV0_offset(r6)
   
   btst   r8, nPCFC
   beq    access
   btst   r8, nPCFD
   bne    Poll_access_low

   st     r5, GPSET0_offset(R6)

access:
   btst   r8, CLK
   beq    waitforclkhigh

waitforclklow:
   ld     r8, GPLEV0_offset(r6)
   btst   r8, nPCFC
   beq    checkforclk
   btst   r8, nPCFD
   bne    Poll_loop
checkforclk:
   btst   r8, CLK
   bne    waitforclklow

waitforclkhigh:
   ld     r8, GPLEV0_offset(r6)
   btst   r8, nPCFC
   beq    checkforclkh
   btst   r8, nPCFD
   bne    Poll_loop
checkforclkh:
   btst   r8, CLK
   beq    waitforclkhigh

   LSR    r7, r8,# ADDRBUS_SHIFT

   and    r7, r8, r10           # Isolate address bus
   
   btst    r8, # RnW
   beq    reading
   
   ldb    r7,(r0,r7) # get byte to write out
   
   lsl    r7, # DATASHIFT
   st     r7, GPSET0_offset(r6)  # set up databus
   st     r9,(r6)                # set databus to outputs

reading:

waitforclklow2:
   ld     r8, GPLEV0_offset(r6)
checkforclk2:
   btst   r8, CLK
   bne    waitforclklow2
   st     r4, (r6)              # data bus to inputs except debug
   st     r8, 512(r0)
   st     r8, (13)              # ring doorbell
   
   mov    r8, #0xFF<<DATASHIFT
   st     r8, GPCLR0_offset(r6)
   
   b Poll_loop
