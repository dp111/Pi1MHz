;
; Pi1MHz 6502 code
;

discaccess = &FCD6

    MACRO PAGERTS
        LDX #&FF
        JMP &FC88
    ENDMACRO

    MACRO ENDBLOCK pos
        SKIPTO &FE00
        COPYBLOCK &FD00, &FE00, pos
        CLEAR &FD00, &FE00
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

MACRO LOADFILETOSWR filename
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
    INY :
    LDA freaddata, Y: STA discaccess+3
    CMP #255
    BNE freadsetuploop
    STA discaccess+4

.freadcheckloop
    LDA discaccess+4
    BMI freadcheckloop
    BEQ readdone
    CMP #20
    BNE pagertsjmp ; file open error

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
    EQUB 2, 0, 1 : EQUS filename : EQUB 0, 255

.freaddata
    EQUB 4, 0, &40, 0
    EQUB 0, 0, &F0, 0
    EQUB 0, 0, 0, 0
    EQUB &FF

ENDMACRO

; Page 0
; help screen
{
ORG &FD00

  ENDBLOCK &700
}

; Page 8 CALL &FDD0
; Strings
{
ORG &FD00
    LDA #0   : STA &FCD6 ; clear byte pointer to zero.
    LDA #&E0 : STA &FCD7
    LDA #&FF : STA &FCD8
.stringloop
    LDA &FCD9 ; auto increment register
    JSR &FFEE
    CMP #0
    BNE stringloop
    PAGERTS

  ENDBLOCK &000
}

; Page 1
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
  PAGERTS

  ENDBLOCK &100
}

; Page 2
; ADFS
{
ORG &FD00

    FINDSWR
    TXA
    BPL SWRfound
.pagertsjmp
    PAGERTS; no SWR found
.SWRfound

    LOADFILETOSWR "ROMS/ADFS.rom"

    ENDBLOCK &200
}


; Page 3
; MMFS
{
ORG &FD00

    FINDSWR
    TXA
    BPL SWRfound
.pagertsjmp
    PAGERTS; no SWR found
.SWRfound

    LOADFILETOSWR "ROMS/MMFS.rom"

    ENDBLOCK &300
}

; Page 4
; MMFSv2
{
ORG &FD00

    FINDSWR
    TXA
    BPL SWRfound
.pagertsjmp
    PAGERTS; no SWR found
.SWRfound

    LOADFILETOSWR "ROMS/MMFSv2.rom"

    ENDBLOCK &400
}

.end

SAVE "../firmware/6502code.bin" , 0, &500
