\ net_transport.asm
\ Pi1MHz service transport: addressing, byte access and dispatch.
\
\ Written for the 1MHz-WiFi project. This is the layer both ROMs talk to the
\ Pi through, so it lives in its own file rather than inside *WGET, which is
\ where it happened to be written first. The network ROM uses it for every
\ command; the filing system ROM uses it to read persisted state and to fetch
\ UEF windows.
\
\ The service registers are at &FC00+net_svc_*, and a cursor is held across
\ calls so a caller can address once and then read or write a run of bytes.

net_result_pending = 1
net_wait_lo = heap+&EA
net_wait_hi = heap+&EB
net_svc_addr_lo = &A6
net_svc_addr_mid = &A7
net_svc_addr_hi = &A8
net_svc_data = &A9
net_svc_command = &AA
net_empty_lo = drv_svc_workspace+24
net_empty_hi = drv_svc_workspace+25
net_cursor_lo = drv_svc_workspace+21
net_cursor_mid = drv_svc_workspace+22
net_cursor_hi = drv_svc_workspace+23

\ Select logical JIM &FFF000: Pi1MHz maps it into the reserved service RAM.
.net_command_address
 lda #0
 jsr net_address_low
 lda #&F0
 jsr net_address_mid
 lda #&FF
 jmp net_address_high

.net_scratch_address
 lda #0
 jsr net_address_low
 lda #&F1
 jsr net_address_mid
 lda #&FF
 jmp net_address_high

.net_address_low
 sta net_cursor_lo
 sta &FC00+net_svc_addr_lo
 jsr bus_delay
 rts
.net_address_mid
 sta net_cursor_mid
 sta &FC00+net_svc_addr_mid
 jsr bus_delay
 rts
.net_address_high
 sta net_cursor_hi
 sta &FC00+net_svc_addr_hi
 jsr bus_delay
 rts

.net_write_a
 php
 sei
 pha
 lda net_cursor_lo
 sta &FC00+net_svc_addr_lo
 jsr bus_delay
 lda net_cursor_mid
 sta &FC00+net_svc_addr_mid
 jsr bus_delay
 lda net_cursor_hi
 sta &FC00+net_svc_addr_hi
 jsr bus_delay
 pla
 sta &FC00+net_svc_data
 jsr bus_delay
 inc net_cursor_lo
 bne net_write_done
 inc net_cursor_mid
 bne net_write_done
 inc net_cursor_hi
.net_write_done
 jsr net_wait_cursor
 plp
 rts

.net_read_a
 php
 sei
 lda net_cursor_lo
 sta &FC00+net_svc_addr_lo
 jsr bus_delay
 lda net_cursor_mid
 sta &FC00+net_svc_addr_mid
 jsr bus_delay
 lda net_cursor_hi
 sta &FC00+net_svc_addr_hi
 jsr bus_delay
 lda &FC00+net_svc_data
 jsr bus_delay
 pha
 inc net_cursor_lo
 bne net_read_done
 inc net_cursor_mid
 bne net_read_done
 inc net_cursor_hi
.net_read_done
 jsr net_wait_cursor
 pla
 plp
 cmp #0
 rts

\ FCA9 read/write callbacks advance the shared cursor asynchronously on real
\ Pi1MHz hardware. Wait until the complete published cursor matches the
\ software cursor before another transaction can select FCA6-FCA8. The loop is
\ deliberately bounded and preserves A and X.
.net_wait_cursor
 pha
 txa
 pha
 ldx #0
.net_wait_cursor_loop
 lda &FC00+net_svc_addr_lo
 jsr bus_delay
 cmp net_cursor_lo
 bne net_wait_cursor_again
 lda &FC00+net_svc_addr_mid
 jsr bus_delay
 cmp net_cursor_mid
 bne net_wait_cursor_again
 lda &FC00+net_svc_addr_hi
 jsr bus_delay
 cmp net_cursor_hi
 beq net_wait_cursor_done
.net_wait_cursor_again
 dex
 bne net_wait_cursor_loop
.net_wait_cursor_done
 pla
 tax
 pla
 rts

\ Dispatch handle zero (&F0).  Bit 7 means the Pi main loop has not serviced
\ the FIQ latch; result 1 means an async DNS/connect is still pending and must
\ be re-issued.  Both paths are bounded and Escape-aware.
.net_dispatch_wait
 lda #0
 sta net_wait_lo
 lda #&FF                 \ about five seconds at one yield per video frame
 sta net_wait_hi
.net_dispatch_again
 php
 sei
 lda #&F0
 sta &FC00+net_svc_command
 jsr bus_delay
 plp
.net_dispatch_busy
 lda &FC00+net_svc_command
 bpl net_dispatch_ready
 dec net_wait_lo
 bne net_dispatch_busy
 lda #19                  \ yield to the Pi main-loop network poll
 jsr osbyte
 dec net_wait_hi
 bne net_dispatch_busy
 jmp net_dispatch_timeout
.net_dispatch_ready
 cmp #net_result_pending
 bne net_dispatch_return
 jsr check_esc
 bcs net_dispatch_cancel
 lda #19
 jsr osbyte
 dec net_wait_lo
 bne net_dispatch_again
 dec net_wait_hi
 bne net_dispatch_again
 jmp net_dispatch_timeout
.net_dispatch_cancel
 lda #&2A                  \ cancelled: never masquerade as successful EOF
.net_dispatch_return
 rts
.net_dispatch_timeout
 lda #&29                  \ private transport-timeout result
 rts
