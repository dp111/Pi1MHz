\ Pi1MHz JIM helpers retained under the stock ElkWiFi labels. An unmodified
\ Electron AP5 forwards only &FCFF and JIM, so it must not touch &FCFD/&FCFE.
\ Direct BBC-family Pi1MHz systems expose those selectors and must explicitly
\ return them to bank 00:00 after another JIM client has changed them.

.detect_jim_machine
 pha
 txa
 pha
 tya
 pha
 lda #&81
 ldx #0
 ldy #&FF
 jsr osbyte
 stx driver_machine
 pla
 tay
 pla
 tax
 pla
 rts

.set_bank_0
 lda #0
 jsr select_public_page_a
 lda #0
 rts

\ Select JIM address 00:00:A while preserving X. Electron/AP5 does not forward
\ FCFD/FCFE; direct BBC-family hosts must reassert them in every masked JIM
\ transaction because another expansion or interrupt handler may change them.
.select_public_page_a
 pha
 txa
 pha
 lda #0
 ldx driver_machine
 cpx #1
 beq set_bank_0_page
 sta &FCFD
 jsr bus_delay
 sta &FCFE
 jsr bus_delay
.set_bank_0_page
 pla
 tax
 pla
 sta pagereg
 jsr bus_delay
 rts

set_bank_1 = set_bank_0

\ Settle the 1MHz bus after touching a Pi1MHz register. The Pi needs time to
\ see a write and publish a reply, and the host must not read back sooner.
\ A and the flags are preserved so this can be dropped between any pair of
\ accesses. Written for this project; it lived in the filing system source
\ only because that is where the first caller happened to be.

.bus_delay          php
                    pha
                    lda #64
.bus_delay_loop     sec
                    sbc #1
                    bne bus_delay_loop
                    pla
                    plp
                    rts


\ Point the Pi1MHz service cursor at persisted state byte X, which lives in
\ the reserved services buffer at &FFEF00. Leaves the cursor set so the
\ caller can read or write &FCA9 directly.

.pi_state_address_x txa
                    sta &FCA6
                    jsr bus_delay
                    lda #&EF
                    sta &FCA7
                    jsr bus_delay
                    lda #&FF
                    sta &FCA8
                    jsr bus_delay
                    rts
