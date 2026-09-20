\ util.asm
\ 1MHz-WiFi ROM: shared helper routines.
\
\ Written for the 1MHz-WiFi project, replacing the helper file the ROM
\ inherited from ElkWiFi 0.23. The entry point names are kept, because they are
\ the interface every other source file in this ROM calls through and because
\ short functional names of this kind carry no design of their own. The
\ implementations below are this project's, and several deliberately differ in
\ approach from what they replace: the inline string printer walks the string
\ with the Y register instead of incrementing a pointer per character, the hex
\ printer folds the nibble with ORA rather than a compare and add pair, and the
\ string to integer conversion shifts the destination in memory rather than
\ rolling each bit out of the accumulator.
\
\ Where a routine's observable contract was relied on elsewhere it is preserved
\ exactly, including one inherited quirk that is called out at string2hex.

\ ===========================================================================
\ Inline string output
\ ===========================================================================
\ Prints the string that follows the JSR, and continues at the terminator.
\
\   jsr printtext
\   equs "text",&0D,&EA
\   ...continues here
\
\ The string ends at the first byte with bit 7 set. That byte is not printed;
\ execution resumes at its address, so &EA is the conventional terminator
\ because it is also NOP and simply falls through into the code after it.
\ A, Y and the zero page pointer are not preserved.

.printtext          pla                     \ return address is the last byte
                    sta zp                  \ of the JSR, one before the text
                    pla
                    sta zp+1
                    ldy #1                  \ so the text starts at offset one
.printtext_next     lda (zp),y
                    bmi printtext_done      \ bit 7 set ends the string
                    tya                     \ OSWRCH is specified to preserve Y,
                    pha                     \ but a WRCHV claimant might not, and
                    lda (zp),y              \ every command prints through here
                    jsr osasci
                    pla
                    tay
                    iny
                    bne printtext_next
                    inc zp+1                \ carry into a string over 255 long
                    jmp printtext_next      \ Y wrapped to zero, keep going
.printtext_done     tya                     \ resume at the terminator itself
                    clc
                    adc zp
                    sta zp
                    bcc printtext_go
                    inc zp+1
.printtext_go       jmp (zp)


\ Print from the Pi reply buffer until a carriage return is printed or the
\ buffer runs out. The carriage return is printed. Uses read_buffer, so X walks
\ the buffer and is left after the last byte read.

.print_string       jsr read_buffer         \ Z set means the buffer ended
                    beq print_string_end
                    jsr osasci
                    cmp #&0D
                    bne print_string
.print_string_end   rts


\ ===========================================================================
\ Command line parsing
\ ===========================================================================
\ Advance Y past spaces on the MOS command line. Enter at skipspace to step
\ over the current character first, or at skipspace1 to test it. On exit Y
\ indexes the first non-space character and A holds it.

.skipspace          iny
.skipspace1         lda (line),y
                    cmp #' '
                    beq skipspace
                    rts


\ Copy the next command line parameter into strbuf, terminated with a carriage
\ return. A parameter ends at a space or at the end of the line. On exit X is
\ the parameter length, so zero means there was no parameter, and Y indexes the
\ terminator that ended it.

.read_cli_param     ldx #0
.read_param_loop    lda (line),y
                    cmp #&0D
                    beq read_param_end
                    cmp #' '
                    beq read_param_end
                    sta strbuf,x
                    iny
                    inx
                    cpx #&FF                \ leave room for the terminator
                    bne read_param_loop
.read_param_end     lda #&0D
                    sta strbuf,x
                    rts


\ Append the parameter now in strbuf to the heap parameter block, carriage
\ return included. X is the heap write offset and must be zero for the first
\ call; it is left after the terminator so parameters can be appended in turn.
\ Y is preserved.

.copy_to_heap       sty save_y
                    ldy #0
.cth1               lda strbuf,y
                    iny
                    sta heap,x
                    inx
                    cmp #&0D
                    bne cth1
                    ldy save_y
                    rts


\ ===========================================================================
\ Number conversion and output
\ ===========================================================================
\ Print A as two hexadecimal digits. A and Y are not preserved.

.printhex           pha
                    lsr a
                    lsr a
                    lsr a
                    lsr a
                    jsr printhex_l1
                    pla
.printhex_l1        and #&0F
                    ora #'0'                \ '0' to '9' land correctly
                    cmp #'9'+1
                    bcc printhex_l2
                    adc #6                  \ carry is set, so this adds seven
.printhex_l2        jmp osasci


\ Convert one ASCII hexadecimal digit in A to its value.
\ Exit: carry clear, A is the value 0-15; or carry set and A undefined.
\ Lower case is rejected, as it was before, so that callers which relied on
\ that rejection to end a parameter still do.

.digit2hex          cmp #'0'
                    bcc digit2hex_inv
                    cmp #'9'+1
                    bcc digit2hex_conv
                    cmp #'A'
                    bcc digit2hex_inv
                    cmp #'F'+1
                    bcs digit2hex_inv
                    sec
                    sbc #'A'-10             \ 'A' becomes ten
                    clc
                    rts
.digit2hex_conv     and #&0F
                    clc
                    rts
.digit2hex_inv      sec
                    rts


\ Convert the hexadecimal string in strbuf to a sixteen bit value in the zero
\ page pair at X, using &02,X as scratch. Conversion stops at a carriage return
\ or at the first character that is not a hexadecimal digit.
\ Exit: X and Y preserved. A is the scratch byte, which callers test against
\ zero to mean "no address was given".
\
\ That test is why &02,X holds the index of the digit being consumed rather
\ than a count: a single digit at index zero therefore reports A=0. The
\ behaviour is inherited and is kept deliberately, because *WGET and the sideways
\ RAM commands rely on a bare "0" being treated as no address.

.string2hex         tya
                    pha
                    lda #0
                    sta &00,x
                    sta &01,x
                    sta &02,x
                    tay
.string2hex_l1      lda strbuf,y
                    cmp #&0D
                    beq string2hex_end
                    jsr digit2hex
                    bcs string2hex_end
                    sty &02,x               \ index survives the shift loop
                    ldy #4
.string2hex_l2      asl &00,x               \ make room for the new nibble
                    rol &01,x
                    dey
                    bne string2hex_l2
                    ora &00,x               \ and merge it in
                    sta &00,x
                    ldy &02,x
                    iny
                    bne string2hex_l1
.string2hex_end     pla
                    tay
                    lda &02,x
                    rts


\ ===========================================================================
\ Reply buffer searching
\ ===========================================================================
\ Search the JIM reply buffer for the string at needle, of length size, from
\ the current read position X.
\ Exit: carry set and X just past the match; or carry clear at end of buffer.
\ A and Y are undefined.

.fnd                ldy #0
.fnd1               jsr read_buffer
                    beq fnd_not_found
                    cmp (needle),y
                    bne fnd                 \ mismatch, start the match again
                    iny
                    cpy size
                    bne fnd1
                    sec
                    rts
.fnd_not_found      clc
                    rts


\ Test whether the two buffer bytes at X are "OK", without consuming them.
\ Exit: Z set if they are. A and X preserved.
\ X is not allowed to be 255 here, which no caller does: the second byte would
\ wrap to the start of the window rather than reading past its end.

.test_ok            pha
                    lda pageram,x
                    cmp #'O'
                    bne test_ok_end
                    lda pageram+1,x
                    cmp #'K'
.test_ok_end        pla
                    rts


\ Test whether the next three buffer bytes are "ERR", consuming them.
\ Exit: Z set if they are. A undefined; X is left wherever the test stopped.

.test_error         pha
                    jsr read_buffer
                    cmp #'E'
                    bne test_error_end
                    jsr read_buffer
                    cmp #'R'
                    bne test_error_end
                    jsr read_buffer
                    cmp #'R'
.test_error_end     pla
                    rts


\ Advance X past the next line feed in the buffer.
\ Exit: Z set if the buffer ended first, clear if a line feed was found.

.search0a           jsr read_buffer
                    beq search0a_l1
                    cmp #&0A
                    bne search0a
                    cmp #&0D                \ any unequal value clears Z
.search0a_l1        rts


\ ===========================================================================
\ Housekeeping
\ ===========================================================================
\ Restore the X and Y a service call entry pushed, and claim the call.

.call_claimed       pla
                    tax
                    pla
                    tay
                    lda #&00
                    rts


\ Report whether Escape is pending, acknowledging it if so.
\ Exit: carry set if Escape was pressed. A, X and Y are preserved.

.check_esc
if __ELECTRON__
                    bit &FF
                    bmi esc_pressed
                    clc
                    rts
.esc_pressed        lda #126
                    jsr osbyte
                    sec
                    rts
else
                    rts
endif


\ Wait for two vertical syncs, to pace polling without burning the bus.

.wait
if __ELECTRON__
                    pha
                    lda #19
                    jsr osbyte
                    jsr osbyte
                    pla
else
                    jsr &FE66
                    jsr &FE66
endif
                    rts


\ ===========================================================================
\ Startup banner glyph
\ ===========================================================================
\ Define and print the signal icon that precedes the banner text.
\
\ The ROM this project started from poked the glyph straight into screen
\ memory at &60A0, which pinned it to row zero while the banner itself followed
\ the boot cursor. Defining character 255 and printing it through the VDU keeps
\ the icon on the banner's own line whatever the ROM ordering and however the
\ screen has scrolled.
\
\ The BBC B, B+ and Master boot into MODE 7, where characters above 127 are
\ teletext codes rather than soft characters, so a defined glyph cannot be
\ displayed and VDU 255 would emit a stray graphics cell instead. Only the
\ Electron, which has no teletext mode, ever reaches the glyph. The banner
\ text is identical on all four machines either way.

if __ELECTRON__
.print_logo         lda #135                \ OSBYTE 135 returns the mode in Y
                    jsr osbyte
                    cpy #7                  \ teletext cannot show a soft
                    beq logo2               \ character, so print none
                    lda #' '
                    jsr oswrch
                    lda #23                 \ VDU 23,255,... defines a glyph
                    jsr oswrch
                    lda #255
                    jsr oswrch
                    ldx #0
.logo1              lda wifi_symbol,x
                    jsr oswrch
                    inx
                    cpx #8
                    bne logo1
                    lda #255                \ and print it
                    jsr oswrch
.logo2              rts
else
.print_logo         rts
endif

\ Two arcs over a point, drawn for this ROM.
.wifi_symbol        equb &3C,&42,&18,&24,&00,&18,&18,&00
