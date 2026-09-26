\ Helpers shared by the Pi1MHz WGET implementation.
\ The inherited UART/AT-command downloader is deliberately not emitted.

proto = heap+&F5
newln = heap+&F6
clptr = heap+&F7
index = heap+&F8
sflag = heap+&F9
tflag = heap+&FA
aflag = heap+&FB
pflag = heap+&FC
uflag = heap+&FD
laddr = heap+&FE

\ *WGET -S: copy the 64 pages the download left in JIM 00:00:00-3F into the
\ sideways RAM bank named by the last parameter.
\
\ The copy runs with that bank selected, and for its duration this ROM - and
\ its workspace, which lives inside the image - is out of the memory map; only
\ zero page, main RAM and the JIM window stay put. So the loop runs from the
\ bottom of the stack page: those bytes are saved in the workspace first and
\ put back before interrupts are enabled again, so nothing else ever sees them,
\ and the stack pointer is checked to be well clear of them. Nothing in the
\ loop may JSR or JMP out of it. Its variables are in &A8-&AF, the MOS's
\ transient workspace for * commands - never the filing system's &C0-&CF.

swr_run    = &0100                  \ where the loop runs
swr_page   = &A8                    \ JIM page being copied
swr_first  = &A9                    \ first write to the ROM select register
swr_bank   = &AA                    \ target bank
swr_sel    = &AB                    \ ROM select register address, 2 bytes
swr_dest   = &AD                    \ destination pointer, 2 bytes
swr_count  = &AF                    \ pages left to copy
swr_save   = heap                   \ the stack-page bytes the loop displaces

.wget_copy_file_to_swr
 lda laddr
 and #&0F
 sta swr_bank
 lda shadow
 and #&0F
 cmp swr_bank
 bne wget_swr_other_bank
 ldx #(error_bad_param-error_table) \ the target is this ROM
 jmp error
.wget_swr_other_bank
 tsx
 cpx #wget_swr_loop_end-wget_swr_loop+&20
 bcs wget_swr_stack_clear
 ldx #(error_stack_deep-error_table)
 jmp error
.wget_swr_stack_clear
 \ BBC and Master select a bank with one write to &FE30. The Electron's &FE05
 \ must see &0C first when the bank is 0-7, which does no harm otherwise.
 lda #&30
 ldx swr_bank
 ldy driver_machine
 cpy #1
 bne wget_swr_select_known
 lda #&05
 ldx #&0C
.wget_swr_select_known
 sta swr_sel
 lda #&FE
 sta swr_sel+1
 stx swr_first
 lda #0
 sta swr_page
 sta swr_dest
 lda #&80                           \ sideways RAM starts at &8000
 sta swr_dest+1
 lda #&40                           \ and is 64 pages long
 sta swr_count
 php
 sei
 ldy #wget_swr_loop_end-wget_swr_loop-1
.wget_swr_borrow
 lda swr_run,y
 sta swr_save,y
 lda wget_swr_loop,y
 sta swr_run,y
 dey
 bpl wget_swr_borrow
 lda #0
 jsr select_public_page_a
 jsr swr_run                        \ A = 0 copied, nonzero if not RAM
 pha
 ldy #wget_swr_loop_end-wget_swr_loop-1
.wget_swr_give_back
 lda swr_save,y
 sta swr_run,y
 dey
 bpl wget_swr_give_back
 lda net_primary_page
 jsr select_public_page_a
 pla
 plp
 tax
 bne wget_swr_not_ram
 rts
.wget_swr_not_ram
 ldx #(error_not_swram-error_table)
 jmp error

\ Copied to swr_run and run there, so only relative branches.
.wget_swr_loop
 ldy #0
 lda swr_first
 sta (swr_sel),y
 lda swr_bank
 sta (swr_sel),y
.wget_swr_byte
 lda pageram,y
 sta (swr_dest),y
 cmp (swr_dest),y                   \ read back: absent RAM will not hold it
 bne wget_swr_failed
 iny
 bne wget_swr_byte
 inc swr_page
 lda swr_page
 sta pagereg
 ldx #90                            \ bus_delay, inline: it is in this ROM
.wget_swr_settle
 dex
 bne wget_swr_settle
 inc swr_dest+1
 dec swr_count
 bne wget_swr_byte
 ldx #0
 beq wget_swr_done
.wget_swr_failed
 ldx #&FF
.wget_swr_done
 ldy #0
 lda shadow                         \ this ROM back before returning into it
 sta (swr_sel),y
 txa
 rts
.wget_swr_loop_end
\ The borrowed bytes are the bottom &40 of the stack page, and the copies above
\ count Y down with BPL, which only works for fewer than 128 bytes.
ASSERT wget_swr_loop_end-wget_swr_loop <= &40
