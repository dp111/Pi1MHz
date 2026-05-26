;
; Pi1MHz 6502 code
;

discaccess = &FCD6

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

    MACRO  page_rom_x
    stx    &f4
    stx    &fe30
    ENDMACRO

MACRO FINDSWR
    LDA &F4
    PHA
    LDX #15
.romlp
    page_rom_x

;; Step 1: Test if candidate slot already contains a rom image
;; so we don't clobber any pre-existing ROM images
        ldy     &8007
        lda     &8000, Y
        bne     testram
        lda     &8001, Y
        cmp     #'('
        bne     testram
        lda     &8002, Y
        cmp     #'C'
        bne     testram
        lda     &8003, Y
        cmp     #')'
        bne     testram

;; Step 2: Test if that pre-existing rom image is SWMMFS
;; so we re-use the same slot again and again
     ;   lda     &b5fe
     ;   cmp     #MAGIC0
     ;   bne     romnxt
     ;   lda     &b5ff
     ;   cmp     #MAGIC1
     ;   bne     romnxt

;; Step 3: Check if slot is RAM
.testram
        lda     &8006
        eor     #&FF
        sta     &8006
        cmp     &8006
        php
        eor     #&FF
        sta     &8006
        plp
        beq     testdone
.romnxt
        dex
        bpl     romlp

.testdone
        PLA
        sta    &f4
        sta    &fe30
ENDMACRO

MACRO LOADFILE

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

    FINDSWR
    TXA
    BPL SWRfound
.pagertsjmp
    JMP pagerts ; no SWR found
.SWRfound
    ; fopen
    LDY #0   : STY discaccess
    DEY      : STY discaccess+1
               STY discaccess+2
.fopenloop
    INY
    LDA fopenstring, Y: STA discaccess+3
    BPL fopenloop
    STA discaccess+4

.fopencheckloop
    LDA discaccess+4
    BMI fopencheckloop
    BNE pagertsjmp ; file not found

    LDY #0   : STY discaccess
    DEY      : STY discaccess+1
               STY discaccess+2

.freadsetuploop
    LDA freaddata+1, Y: STA discaccess+3
    INY
    CPY #12
    BNE freadsetuploop
    STA discaccess+4

.freadcheckloop
    LDA discaccess+4
    BMI freadcheckloop
    BEQ readdone
    CMP #20
    BNE pagerts ; file open error

.readdone
    LDY #0   : STY discaccess
             : STY discaccess+1
    LDA #&F0 : STA discaccess+2

             : STY swrpointer+1
    LDA #&80 : STA swrpointer+2

    LDA &F4
    PHA
    STX &F4
    STX &FE30

.copyswrloop
    LDA discaccess+3 : .swrpointer STA &8000,Y
    INY
    BNE copyswrloop

    LDX swrpointer+2
    INX
    STX swrpointer+2
    CPX #&C0
    BNE copyswrloop;

    PLA
    STA &F4
    STA &FE30
    JMP (&FFFC) ; Reset


.fopenstring
    EQUB 2 , 0, 1 : EQUS "ROMS\ADFS.rom" : EQUB 0, 255

.freaddata
    EQUB 4, 0, &40, 0
    EQUB 0, 0, &F0, 0
    EQUB 0, 0, 0, 0
    EQUB &FF


    PAGESELECT
    COPYBLOCK &FD00, &FE00, &350
    CLEAR &FD00, &FE00
}

; Page 5 CALL &FDDC
{
ORG &FD00

    FINDSWR
    TXA
    BPL SWRfound
.pagertsjmp
    JMP pagerts ; no SWR found
.SWRfound
    ; fopen
    LDY #0   : STY discaccess
    DEY      : STY discaccess+1
               STY discaccess+2

.fopenloop
    INY
    LDA fopenstring, Y: STA discaccess+3
    BPL fopenloop
    STA discaccess+4

.fopencheckloop
    LDA discaccess+4
    BMI fopencheckloop
    BNE pagertsjmp ; file not found

    LDY #0   : STY discaccess
    DEY      : STY discaccess+1
               STY discaccess+2

.freadsetuploop
    LDA freaddata+1, Y: STA discaccess+3
    INY
    CPY #12
    BNE freadsetuploop
    STA discaccess+4

.freadcheckloop
    LDA discaccess+4
    BMI freadcheckloop
    BEQ readdone
    CMP #20
    BNE pagerts ; file open error

.readdone
    LDY #0   : STY discaccess
             : STY discaccess+1
    LDA #&F0 : STA discaccess+2

             : STY swrpointer+1
    LDA #&80 : STA swrpointer+2

    LDA &F4
    PHA
    STX &F4
    STX &FE30

.copyswrloop
    LDA discaccess+3 : .swrpointer STA &8000,Y
    INY
    BNE copyswrloop

    LDX swrpointer+2
    INX
    STX swrpointer+2
    CPX #&C0
    BNE copyswrloop;

    PLA
    STA &F4
    STA &FE30
    JMP (&FFFC) ; Reset


.fopenstring
    EQUB 2 , 0, 1 : EQUS "ROMS\MMFSJR.rom" : EQUB 0, 255

.freaddata
    EQUB 4, 0, &40, 0
    EQUB 0, 0, &F0, 0
    EQUB 0, 0, 0, 0
    EQUB &FF

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
