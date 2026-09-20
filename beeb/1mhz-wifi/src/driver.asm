\ ElkWiFi-compatible driver for the Pi1MHz 1 MHz-bus service
\ (c) Roland Leurs, May 2020

\ Main service ROM
\ Version 1.00

\ AP5/Pi1MHz exposes &FCFF as a write-only JIM page selector. Keep the shadow
\ and transient machine type in the service driver's private block. The stock
\ `heap` at &0900 is ADFS/application workspace and is not safe during OSWORD.
driver_page_shadow = drv_svc_workspace+19
driver_machine = drv_svc_workspace+20
driver_function = drv_svc_workspace+21
driver_entry_x = drv_svc_workspace+22
driver_entry_y = drv_svc_workspace+23

\ Please note that some functions or routines are not quite logical
\ but they are implemented to keep driver compatibility with the 
\ Atom wifi driver.

.wifidriver
 \ entries are.
 \ 00 init                12 cipstatus
 \ 01 reset               13 cipsend
 \ 02 gmr                 14 cipclose
 \ 03 cwlap               15 cipserver
 \ 04 cwjap               16 cipsto
 \ 05 cwqap               17 ciobaud
 \ 06 cwsap               18 cifsr
 \ 07 cwmode              19 ciupdate
 \ 08 cipstart            20 ipd
 \ 09 cpmux               21 csyswdtenable
 \ 10 cwlif               22 csyswdtdisable
 \ 11 setbuffer           23 getmuxchannel
 \ 24 disable/enable
 \ 25 cwlapopt
 \ 26 sslbufsize
 \ 27 cipmode
 \ 28 ping
 \ 29-31 reserved

 sta save_a                 \ save registers
 stx save_x
 sty save_y
 sta driver_function
 stx driver_entry_x
 sty driver_entry_y
 \ OSBYTE &81 with X=0,Y=&FF is the documented machine-type query. Run it
 \ before touching the JIM high selectors and retain the result only for this
 \ driver call; it is not valid as a reset-time cache.
 lda #&81
 ldx #0
 ldy #&FF
 jsr osbyte
 stx driver_machine
 jsr set_bank_0             \ ElkWiFi buffers are in JIM address 00:00:page
 lda #0
 sta driver_page_shadow
 \ ElkWiFi 0.23 masks public function numbers to five bits after handling its
 \ private flash entry. DATE, TIME and ONLINE call Pi routines directly and
 \ therefore do not consume public driver numbers 32-34.
 lda driver_function
 and #&1F
 asl a
 tax
 lda public_driver_dispatch+1,x
 pha
 lda public_driver_dispatch,x
 pha
 ldx driver_entry_x
 ldy driver_entry_y
 lda driver_function
 and #&1F
 rts

; RTS dispatch preserves the JMP-style handler entry stack while reducing the
; public 0-31 ABI to one auditable table. Entries contain target minus one.
.public_driver_dispatch
 equw service_driver_init-1
 equw service_driver_reset-1
 equw service_driver_version-1
 equw service_driver_scan-1
 equw service_driver_join-1
 equw service_driver_leave-1
 equw service_driver_ifcfg-1
 equw service_driver_mode-1
 equw service_driver_cipstart-1
 equw service_driver_cpmux-1
 equw service_driver_connection_status-1
 equw service_driver_set_buffer-1
 equw service_driver_connection_status-1
 equw service_driver_cipsend-1
 equw service_driver_cipclose-1
 equw service_driver_baud_compat-1
 equw service_driver_baud_compat-1
 equw service_driver_baud_compat-1
 equw service_driver_ifcfg-1
 equw service_driver_unsupported-1
 equw service_driver_ipd-1
 equw service_driver_ok-1
 equw service_driver_ok-1
 equw service_driver_mux_channel-1
 equw service_driver_wifi_control-1
 equw service_driver_lapopt-1
 equw service_driver_ok-1
 equw service_driver_mode_unsupported-1
 equw service_driver_ping-1
 equw service_driver_unsupported-1
 equw service_driver_unsupported-1
 equw service_driver_unsupported-1
\ Initialize the data buffer, by resetting the paged ram register to 0. This
\ call does not clear the buffer and will mostly be called after a command
\ is executed and the response is processed.
.reset_buffer
 php
 sei
 lda #0
 sta driver_page_shadow
 jsr select_public_page_a
 ldx #&00               \ callers read the buffer from its first byte
 plp
 rts

\ Initialize the data buffer, by resetting the paged ram register to 0
\ and clearing the first byte. After a command is executed and if the first
\ byte is 0 then there was no response from the ESP8266. This call is intended
\ before a command is executed.
.clear_buffer
 jsr reset_buffer
 php
 sei
 txa
 jsr select_public_page_a
 stx pageram
 jsr bus_delay
 plp
; txa
;.irb_l1
; sta pageram,x
; inx
; bne irb_l1
 rts

\ Reads a character from the paged ram buffer at position X
\ returns the character in A and the X register points 
\ to the next data byte.
.read_buffer
 php
 sei
 lda driver_page_shadow
 jsr select_public_page_a
 lda pageram,x
 plp
 jsr read_buffer_inc
 ora #0                    \ return N/Z for the byte read
 rts
.read_buffer_inc
 inx
 beq read_buffer_page_over  \ X wrapped, so the JIM page is exhausted
 rts
.read_buffer_page_over
 jmp inc_page_reg

\ Writes a character to the paged ram buffer at position X
\ returns with X pointing to the next byte
.write_buffer
 php
 sei
 pha
 lda driver_page_shadow
 jsr select_public_page_a
 pla
 sta pageram,x
 jsr bus_delay
 plp
 jmp read_buffer_inc

\ Decrements the 24 bit data pointer. On the Electron most transfers will be smaller than
\ 64 KB but it is possible to send up to 4 MB per transfer.
.dec_data_counter
 lda data_counter           \ borrow only into the bytes that need it
 bne dec_data_counter_low
 lda data_counter+1
 bne dec_data_counter_mid
 dec data_counter+2
.dec_data_counter_mid
 dec data_counter+1
.dec_data_counter_low
 dec data_counter
 lda data_counter           \ Z set once all three bytes are zero
 ora data_counter+1
 ora data_counter+2
 rts
\ Finalise a Pi1MHz response buffered in JIM.
.restore_env
.set_buffer
 stx datalen            \ save end of data
 ldx driver_page_shadow
 stx datalen+1
 ldx datalen            \ restore x register
rts

\ Increments the paged ram register and sets the (X) pointer to the beginning of the page. 
\ If the end of paged ram has been reached then the page register will roll over from &FF
\ to &00 and the Z flag is set. The pageregister and X will not be updated and the routine
\ returns with Z=1. The calling routine can test this flag for the end of buffer.
.inc_page_reg
 ldx driver_page_shadow \ use the write-only page register's RAM shadow
 inx                    \ increment the value
 beq buffer_end         \ if it becomes zero then the end of the buffer (paged ram) is reached
 stx driver_page_shadow
 php
 sei
 txa
 jsr select_public_page_a \ write back to page register (i.e. select next page)
 plp
 ldx #0                 \ reset y register
 cpx #1                 \ clears Z-flag
.buffer_end
 rts                    \ return to store routine
