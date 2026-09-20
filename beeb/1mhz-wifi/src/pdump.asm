; PRD - inspect Pi1MHz JIM without reading the write-only selectors.
;
; Syntax: *PRD [address] [bank]
;
; `address` is a 16-bit offset in the selected 64K window. Direct BBC-family
; connections expose bank 0/1 through &FCFE. Electron AP5 only forwards the
; page selector, so bank 1 is rejected instead of being silently ignored.

pdump_bank = driver_entry_x

.pdump_cmd
 jsr detect_jim_machine
 lda #0
 sta pdump_bank
 sta load_addr
 sta load_addr+1
 jsr skipspace1
 jsr read_cli_param
 cpx #0
 beq pdump_start
 ldx #load_addr
 jsr string2hex

 jsr skipspace1
 jsr read_cli_param
 cpx #0
 beq pdump_start
 ldx #zp
 jsr string2hex
 lda zp+1
 bne pdump_bad_bank
 lda zp
 cmp #2
 bcs pdump_bad_bank
 sta pdump_bank
 beq pdump_start
 lda driver_machine
 cmp #1
 bne pdump_start
 jmp pdump_bad_bank

.pdump_bad_bank
 jsr set_bank_0
 ldx #(error_bad_option-error_table)
 jmp error

.pdump_start
 lda load_addr
 and #&F8
 sta load_addr
 tay

.pdump_l1
 lda load_addr+1
 jsr printhex
 lda #':'
 jsr oswrch
 tya
 pha
 jsr printhex
 lda #' '
 jsr oswrch
 jsr oswrch
 ldx #8
.pdump_l2
 jsr pdump_read_y
 jsr printhex
 lda #' '
 jsr oswrch
 iny
 dex
 bne pdump_l2
 pla
 tay
 ldx #8
.pdump_l3
 jsr pdump_read_y
 bmi pdump_dot
 cmp #&20
 bmi pdump_dot
 cmp #&7F
 beq pdump_dot
 jsr oswrch
.pdump_l4
 iny
 dex
 bne pdump_l3
 jsr osnewl
 jsr check_esc
 bcs pdump_end
 cpy #0
 bne pdump_l1
 inc load_addr+1
 bne pdump_l1

.pdump_end
 jsr set_bank_0
 jmp call_claimed

.pdump_dot
 lda #'.'
 jsr oswrch
 jmp pdump_l4

; MOS output and interrupt handlers may use JIM. Reassert the complete direct
; address, or the AP5-visible page, immediately before every data access.
.pdump_read_y
 php
 sei
 lda driver_machine
 cmp #1
 beq pdump_read_page
 lda #0
 sta &FCFD
 jsr bus_delay
 lda pdump_bank
 sta &FCFE
 jsr bus_delay
.pdump_read_page
 lda load_addr+1
 sta pagereg
 jsr bus_delay
 lda pageram,y
 plp
 ora #0
 rts
