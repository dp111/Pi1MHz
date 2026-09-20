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

.wget_context_switch_in
 php
 sei
 pha
 lda net_primary_page
 sta load_addr+1
 jsr set_bank_1
 lda pr_r
 sta pagereg
 jsr bus_delay
 ldy pr_y
 pla
 plp
 rts

.wget_context_switch_out
 php
 sei
 pha
 sty pr_y
 jsr set_bank_0
 lda load_addr+1
 sta net_primary_page
 sta pagereg
 jsr bus_delay
 pla
 plp
 rts

.wget_copy_file_to_swr
 ldy #(wget_swramload_end-wget_swramload)
.wget_copy_file_l1
 lda wget_swramload,y
 sta heap,y
 dey
 bpl wget_copy_file_l1
 jsr wget_context_switch_in
 jsr heap
 jsr wget_context_switch_out
 rts

.wget_swramload
 lda #&81
 ldx #0
 ldy #&FF
 jsr osbyte
 stx zp+5
 lda laddr
 and #&0F
 pha
 ldx zp+5
 cpx #1
 bne wget_swram_select_bbc
 lda #&0C
 sta &FE05
 pla
 sta &FE05
 jmp wget_swram_selected
.wget_swram_select_bbc
 pla
 sta &FE30
.wget_swram_selected
 sei
 ldx #0
 stx pr_r
 stx pagereg
 stx zp+2
 lda #&80                  \ sideways RAM starts at &8000
 sta zp+3
 lda #&40                  \ and is 64 pages long
 sta zp+4
 ldy #0
.wget_swramload_l1
 lda pageram,y
 sta (zp+2),y
 cmp (zp+2),y              \ read back: absent RAM will not hold the write
 bne wget_swramload_err
 iny
 bne wget_swramload_l1
 inc pr_r
 lda pr_r
 sta pagereg
 jsr bus_delay
 inc zp+3
 dec zp+4
 bne wget_swramload_l1
 lda shadow
 ldx zp+5
 cpx #1
 bne wget_swram_restore_bbc
 sta &FE05
 jmp wget_swram_restored
.wget_swram_restore_bbc
 sta &FE30
.wget_swram_restored
 cli
 rts

.wget_swramload_err
 lda shadow
 ldx zp+5
 cpx #1
 bne wget_swram_error_restore_bbc
 sta &FE05
 jmp wget_swram_error_restored
.wget_swram_error_restore_bbc
 sta &FE30
.wget_swram_error_restored
 cli
 brk
 equb 0
 equs "Not swram",0
.wget_swramload_end
