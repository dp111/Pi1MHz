;
; Pi1MHz 6502 code
;
;

MACRO PAGESELECT
    SKIPTO &FDF0
.pagerts
    LDX #0
    STX &FCFF
    RTS
    EQUB 0,0
.pageselect
    STX &FCFF
    JMP &FD00
    EQUB 0,0
ENDMACRO

pagerts = &FDF0
{
    ORG &FDB0
.start
    FOR n, 16,1, -1
        LDX #n : BNE pagesel+8
    NEXT
.pagesel
    PAGESELECT
.end
    COPYBLOCK start, end, 0
    CLEAR &FD00, &FE00
}


; Page 1 CALL &FDEC
{
ORG &FD00

    PAGESELECT
    COPYBLOCK &FD00, &FE00, &50
      CLEAR &FD00, &FE00
}

; Page 2 CALL &FDE8
{
ORG &FD00

    PAGESELECT
    COPYBLOCK &FD00, &FE00, &150
      CLEAR &FD00, &FE00
}

; Page 3 CALL &FDE4
; mmfs
{
ORG &FD00

    PAGESELECT
    COPYBLOCK &FD00, &FE00, &250
    CLEAR &FD00, &FE00
}

; Page 4 CALL &FDE0
; adfs
{
ORG &FD00

    PAGESELECT
    COPYBLOCK &FD00, &FE00, &350
    CLEAR &FD00, &FE00
}

; Page 5 CALL &FDDC
{
ORG &FD00

    PAGESELECT
    COPYBLOCK &FD00, &FE00, &450
    CLEAR &FD00, &FE00
}

; Page 6 CALL &FDD8
; oswrch redirector
{
ORG &FD00

newoswrch = &FCD1

  LDA &20E
  STA newoswrch+4
  LDA &20F
  STA newoswrch+4+1
  LDA #(newoswrch MOD 256)
  STA &20E
  LDA #(newoswrch DIV 256)
  STA &20F
  JMP pagerts

    PAGESELECT
    COPYBLOCK &FD00, &FE00, &550
    CLEAR &FD00, &FE00
}

; Page 7 CALL &FDD4
; Status
{
ORG &FD00

    PAGESELECT
    COPYBLOCK &FD00, &FE00, &650
    CLEAR &FD00, &FE00
}

; Page 8 CALL &FDD0
; Strings
{
ORG &FD00
    LDA #0
    STA &FC00 ; clear byte pointer to zero.
    STA &FC01
    STA &FC02
    STA &FC04
.stringloop
    LDA &FC05 ; auto increment register
    JSR &FFEE
    CMP #13
    BNE stringloop
    JSR &FFEE
    JMP pagerts

    PAGESELECT
    COPYBLOCK &FD00, &FE00, &750
    CLEAR &FD00, &FE00
}

; Page 9 CALL &FDCC
{
ORG &FD00

    PAGESELECT
    COPYBLOCK &FD00, &FE00, &850
    CLEAR &FD00, &FE00
}


.end

SAVE "6502code.bin" , 0, &850
