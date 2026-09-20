\ Resident IPv4 resolver using the same Pi1MHz mailbox transport as WGET.

nslook_octet = heap+&B2
nslook_index = heap+&B3
nslook_address = heap+&B4

.nslook_cmd
 jsr skipspace1
 jsr read_cli_param
 cpx #0
 bne nslook_have_host
 jsr printtext
 equs "Usage: *NSLOOK <hostname>",&0D,&EA
 jmp call_claimed

.nslook_have_host
 jsr net_command_address
 lda #45                    \ NET_CMD_OPEN
 jsr net_write_a
 lda #0                     \ NET_TYPE_TCP
 jsr net_write_a
 jsr net_dispatch_wait
 cmp #0
 bne nslook_error

 jsr net_command_address
 lda #46                    \ NET_CMD_DNS
 jsr net_write_a
 ldx #0
.nslook_copy_host
 stx nslook_index
 lda strbuf,x
 cmp #&0D
 bne nslook_write_host
 lda #0
.nslook_write_host
 jsr net_write_a
 cmp #0
 beq nslook_resolve
 ldx nslook_index
 inx
 bne nslook_copy_host
 lda #&2B
 bne nslook_error_close

.nslook_resolve
 jsr net_dispatch_wait
 cmp #0
 bne nslook_error_close
 jsr net_command_address
 lda #4
 jsr net_address_low
 ldx #0
.nslook_copy_ip
 jsr net_read_a
 sta nslook_address,x
 inx
 cpx #4
 bne nslook_copy_ip

 jsr printtext
 equs "Address: ",&EA
 ldx #0
.nslook_print_ip
 stx nslook_octet
 lda nslook_address,x
 jsr nslook_print_u8
 ldx nslook_octet
 inx
 cpx #4
 beq nslook_done
 lda #'.'
 jsr oswrch
 jmp nslook_print_ip
.nslook_done
 jsr osnewl
 jsr nslook_close
 jmp call_claimed

.nslook_error_close
 pha
 jsr nslook_close
 pla
.nslook_error
 jsr print_network_error
 jmp call_claimed

.nslook_close
 jsr net_command_address
 lda #53                    \ NET_CMD_CLOSE
 jsr net_write_a
 jsr net_dispatch_wait
 rts

.nslook_print_u8
 ldx #'0'
.nslook_hundreds
 cmp #100
 bcc nslook_hundreds_done
 sec
 sbc #100
 inx
 bne nslook_hundreds
.nslook_hundreds_done
 pha
 cpx #'0'
 beq nslook_tens_start
 txa
 jsr oswrch
 pla
 pha
 ldx #':'                   \ force a zero tens digit when needed
.nslook_tens_start
 pla
 ldy #'0'
.nslook_tens
 cmp #10
 bcc nslook_tens_done
 sec
 sbc #10
 iny
 bne nslook_tens
.nslook_tens_done
 pha
 cpx #'0'
 bne nslook_show_tens
 cpy #'0'
 beq nslook_ones
.nslook_show_tens
 tya
 jsr oswrch
.nslook_ones
 pla
 clc
 adc #'0'
 jmp oswrch
