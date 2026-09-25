;
; Pi1MHz 6502 helper code
;
newoswrch = &FCA0
discaccess =newoswrch+6
OSBYTE = &FFF4
OSWRCH = &FFEE
OSNEWL = &FFE7

OSRDCH = &FFE0
OSWORD = &FFF1
OSFIND = &FFCE
OSGBPB = &FFD1

; ---------------------------------------------------------------------------
; SD card explorer (helper 17) - pages 17 and 31..45
;
; Runs entirely from the paged &FD00 window and uses no Beeb RAM outside
; the user zero page (&70-&8F), so serial and cassette keep working while
; it runs - pages &09 and &0A are the RS423/cassette buffers.
; Directory entries are fetched with FAT command 17 (readdir-ex), which
; fills fixed 128-byte records - attribute, size, a ready-to-print 38-char
; display line and the raw name - at &D00000 in the JIM transfer buffer.
;
; Anything the filing system must be pointed at (a filename for OSFIND, the
; OSWORD 0 line buffer, OSGBPB's data) lives in the transfer page: a page of
; the services buffer at &D90000 that &FC88 = &FE shows in the window (the
; firmware routes the FS's writes into it and restores the user's JIM page
; on the next select).  The calls are made by small stubs inside that page,
; so the FS reads and writes the very page it is running from.  The page is
; installed from the template helper page EXP_XFERT; its setup-only stubs
; share the data area, so it is reinstalled before each transfer or chdir.
; File data moves straight between that page and the SD file with fread /
; fwrite on file &D (command &FD, block &FFFD00) - no bounce buffer.
;
; Services buffer use, per the documented layout (docs/advanced.md): the
; explorer keeps to the program area - records &D00000, path &D80000,
; transfer page &D90000.  Clear of MMFS/MMFS2 (sector buffer &FFE000,
; command block &FFF000), the ROM loader (&F00000) and the command blocks.
; ---------------------------------------------------------------------------

; Helper numbers 18-30 are deliberately left free for future helpers, so the
; explorer's own pages start above them.  Only EXP_HUB is a helper anybody
; runs; the rest are pages it chains through while it is running.
EXP_FREE_FIRST = 18 ; first reserved-for-future helper page
EXP_FREE_LAST  = 30 ; last one

EXP_HUB   = 17      ; init
EXP_KEY   = 31      ; key dispatch loop
EXP_DIR   = 32      ; read directory into records
EXP_DRAW2 = 33      ; path row, then rows
EXP_DRAW  = 34      ; row rendering (entry 1: two rows, entry 4: all rows)
EXP_CD    = 35      ; chdir (entry 1: selected, entry 4: up)
EXP_GET1  = 36      ; get: prompt for FS name
EXP_GET2  = 37      ; get: open FS + SD files
EXP_GET3  = 38      ; get: copy loop
EXP_PUT1  = 39      ; put: prompt + open FS input
EXP_PUT2  = 40      ; put: create SD file + copy loop
EXP_FIN   = 41      ; close files, report, route on
EXP_ERR   = 42      ; BRK (filing system error) trap
EXP_EXIT  = 43      ; restore state and return
EXP_GET4  = 44      ; get: after the FS write of a chunk
EXP_PUT3  = 45      ; put: SD write half of the copy loop
EXP_XFERT = 46      ; transfer page template (installed at &D90000)

; &FC88 value that shows the transfer page instead of a helper page
XFER_SEL  = &FE

; zero page - the user area &70-&8F, nothing else.  The list-state
; variables are single bytes, so their odd neighbours are free.
zp_count  = &70     ; number of entries (0-250)
old_fx4   = &71     ; *FX4 on entry
zp_sel    = &72     ; selected entry
old_fx229 = &73     ; *FX229 on entry
zp_top    = &74     ; first entry on screen
old_stack = &75     ; stack level to unwind to after a BRK
zp_idx    = &76     ; drawrow argument
old_brkv  = &77     ; BRKV on entry, low
zp_rows   = &78     ; row loop counter
old_brkv_hi = &79   ; BRKV on entry, high
zp_oldsel = &7A     ; previous selection (two-row redraw)
zp_ret    = &7B     ; transfer page stubs return to this page ...
zp_fshand = &7C     ; current-FS file handle, 0 = none
zp_off    = &7D     ; &7D-&7F 24-bit SD file offset
zp_prptr  = &80     ; &80/&81 inline-print pointer
zp_len    = &82     ; &82/&83 chunk length
zp_eof    = &84     ; get: last-chunk flag
zp_fail   = &85     ; transfer error flag
zp_reread = &86     ; reread directory after transfer
brk_stub  = &87     ; &87-&8E 8-byte BRK redirector, built by init
zp_rent   = &8F     ; ... at this entry offset - 1

; JIM transfer-buffer addresses (24 bit)
recs_hi   = &D0     ; records at &D00000, 128 bytes each
chunk_hi  = &D8     ; path buffer at &D80000
xfer_hi   = &D9     ; transfer page at &D90000
AM_DIR    = &10

; Transfer page layout, as seen in the window.  Entries 1-16 are the stubs'
; jump table; the persistent part (gbpb stub, return, OSGBPB block) is what a
; transfer needs, and DATA..&FDF6 holds either file data or - between
; transfers - the name buffer and the setup-only stubs.
XE_INSTALL = 1      ; template only: copy this page to &D90000
XE_OSWORD  = 4      ; OSWORD 0 into xname;           out: C = Escape, X = length
XE_OSFIND  = 7      ; X = OSFIND mode, name xname;   out: X = handle
XE_GBPB    = 10     ; X = OSGBPB fn, count zp_len;   out: C, block updated
XE_P2PAGE  = 13     ; X = max; port string -> xname; out: X = length
XE_P2PORT  = 16     ; xname -> port, NUL terminated
; Every stub returns with GOTOPAGE zp_ret, zp_rent+1 after a PHP: the entry
; it returns to must PLP first.
XBLK       = &FD37  ; OSGBPB control block, 13 bytes (persistent)
XBLK_COUNT = XBLK+5 ; its count field, left holding the bytes NOT moved
XFER_DATA  = &FD44  ; data area: file data, or the setup stubs + name
XFER_LEN   = &FDF7 - XFER_DATA   ; bytes of file data per chunk

; switch to another explorer page: every explorer page carries the common
; PAGESWITCH trailer at &FDF7, so this works from any of them.  target
; offset must be >= 1 (the pushed return address is &FD00+off-1).
MACRO GOTOPAGE bank, off
    LDA #bank
    LDY #off-1
    JMP &FDF7
ENDMACRO

; run a transfer page stub, returning to retpage entry retoff.  X passes
; straight through the page switch, so it carries the stub's parameter.
MACRO XCALL entry, retpage, retoff
    LDA #retpage
    STA zp_ret
    LDA #retoff-1
    STA zp_rent
    GOTOPAGE XFER_SEL, entry
ENDMACRO

; (re)install the transfer page from its template, then return as XCALL does
MACRO XINSTALL retpage, retoff
    LDA #retpage
    STA zp_ret
    LDA #retoff-1
    STA zp_rent
    GOTOPAGE EXP_XFERT, XE_INSTALL
ENDMACRO

; dispatch the command block for a slot and wait for completion.
; exits with A = result, flags set (BEQ = FR_OK)
MACRO DOCMD slot
{
    LDA #slot
    STA discaccess+4
.wait
    LDA discaccess+4
    BMI wait
}
ENDMACRO

; point the 24-bit buffer pointer at a slot's command block (&FFxx00)
MACRO SETCMDPTR slot
    LDA #0
    STA discaccess
    LDA #slot
    STA discaccess+1
    LDA #&FF
    STA discaccess+2
ENDMACRO

; point the buffer pointer at record[A]+off (off < 128; records are
; 128-byte aligned so the ORA can never carry).  clobbers A only.
MACRO SETRECPTR off
    LSR A
    STA discaccess+1
    LDA #0
    ROR A
    ORA #off
    STA discaccess
    LDA #recs_hi
    STA discaccess+2
ENDMACRO

; local print-inline-string subroutine; string follows the JSR,
; terminated by &FF (so teletext colour codes and VDU 31 zeros pass).
; expand as:  .print  PRSUB   (the label at the expansion site)
MACRO PRSUB
{
    PLA
    STA zp_prptr
    PLA
    STA zp_prptr+1
.prnext
    INC zp_prptr
    BNE prget
    INC zp_prptr+1
.prget
    LDY #0
    LDA (zp_prptr),Y
    CMP #&FF
    BEQ prdone
    JSR OSWRCH
    JMP prnext
.prdone
    LDA zp_prptr+1
    PHA
    LDA zp_prptr
    PHA
    RTS
}
ENDMACRO

GUARD &FE00

MACRO PAGERTS
    LDX #&FF
    JMP &FC88
ENDMACRO

;; entry A bank to switch too
;; Y pointer into back - 1
;; X parameter for code
MACRO PAGESWITCHORG
    ORG &FE00-9
ENDMACRO

MACRO PAGESWITCH
    PAGESWITCHORG
;; stack rts data
    STA     &FC88 ; 3 do page switch
    LDA     #&FD  ; 2 high byte of return address
    PHA           ; 1
    TYA           ; 1 low byte of return address
    PHA           ; 1
    RTS           ; 1 off we go
                  ; 9 bytes total
ENDMACRO


MACRO ENDBLOCK pos
    SKIPTO &FE00
    COPYBLOCK &FD00, &FE00, pos
    CLEAR &FD00, &FE00
    GUARD &FE00
ENDMACRO

MACRO PRTSTRING string
FOR n,1,LEN(string)
    LDA # ASC(MID$(string,n,1))
    JSR &FFEE
NEXT

ENDMACRO

MACRO LOADFILETOSWR filename
{
    LDA     &F4
    PHA
    LDX     #15
.romlp
    stx     &f4
    stx     &fe30
    LDA     #'.'
    JSR     OSWRCH
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
    ; bne     testram

;; Step 2: Test if that pre-existing rom image is SWMMFS
;; so we reuse the same slot again and again
     ;   lda     &b5fe
     ;   cmp     #MAGIC0
     ;   bne     romnxt
     ;   lda     &b5ff
     ;   cmp     #MAGIC1
     ;   bne     romnxt
    beq     romnxt
;; Step 3: Check if slot is RAM
.testram
    lda     &8006
    eor     #&FF
    sta     &8006
    cmp     &8006
    beq     SWRfound
.romnxt
    dex
    bpl     romlp

; no SWR found
.noswr
    PLA
    sta    &f4
    sta    &fe30
   ; should really put the error on the stack
   ; and fake RTS on the stack
   ; LDX     #255
   ; STX     &FC88   ; Restore JIM
    BRK
    EQUB 255: EQUS "No SWR":EQUB 0

.fileerror

    PLA
    sta    &f4
    sta    &fe30
   ; should really put the error on the stack
   ; and fake RTS on the stack
   ; LDX     #255
   ; STX     &FC88   ; Restore JIM
    BRK
    EQUB &D6: EQUS "No ROM":EQUB 0

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
    BNE fileerror ; file not found

    LDY #0   : STY discaccess

.freadsetuploop
    LDA freaddata, Y: STA discaccess+3
    INY
    CMP #255
    BNE freadsetuploop
    STA discaccess+4

.freadcheckloop
    LDA discaccess+4
    BEQ readdone
    BMI freadcheckloop
    CMP #20
    BNE fileerror ; file open error

.readdone
    LDY #0   : STY discaccess
             : STY discaccess+1
    LDA #&F0 : STA discaccess+2

             : STY swrpointer+1
    LDA #&80 : STA swrpointer+2

    LDX #&C0-&80
.copyswrloop
    LDA discaccess+3
.swrpointer
    STA &8000,Y
    INY
    BNE copyswrloop

    INC swrpointer+2
    DEX
    BNE copyswrloop;
    ; Y is zero
           : STY discaccess
    DEY    : STY discaccess+1
             STY discaccess+2

    LDY #fclose-&FD01
    LDA #0
    BEQ pageswitch

.fopenstring
    EQUB 2, 0, 1 : EQUS filename : EQUB 0, 255

.freaddata
    EQUB 4, 0, &40, 0
    EQUB 0, 0, &F0, 0
    EQUB 0, 0, 0, 0
    EQUB &FF

    PAGESWITCHORG
.pageswitch
    PAGESWITCH
}
ENDMACRO

; Page 0
; help screen
{
ORG &FD00
.waitloop
    JMP waitloop    ; Arm code will rewrite this to signal help screen has been setup.
    LDA #0   : STA discaccess ; clear byte pointer to zero.
    LDA #&E0 : STA discaccess+1
    LDA #&FF : STA discaccess+2
.stringloop
    LDA discaccess+3 ; auto increment register
    JSR &FFEE
    TAX ; Set flags
    BNE stringloop
    PAGERTS

.*fclose
    LDA #3
    STA discaccess+3
    LDY #255
    STY discaccess+4
.*reboot
    LDA #0      ; get machine id
    LDX #1
    JSR OSBYTE
    CPX #3
    BCC doviareset  ; jump for BEEB and B+
    LDA #200        ; master
    LDY #0
    LDX #2
    JSR OSBYTE
    JMP (&FFFC) ; Reset
.doviareset
    LDA #&7F
    STA &FE4E ; clear IER
    JMP (&FFFC) ; Reset

  PAGESWITCHORG
  PAGESWITCH
  ENDBLOCK &000
}
; page 1 status screen
{
    ORG &FD00
    PAGERTS

    ENDBLOCK &100
}

; Page 2
; oswrch redirector
{
ORG &FD00
  JMP setupredirectorwithmessage ; default entry point for message redirector

  JSR setupredirector       ; &FD03 Entry point for non message redirector and no mode change
  JMP oswtchredirectexit

.setupredirector
  LDA &20F
  CMP #(newoswrch DIV 256)
  BNE redirectnextbyte
  RTS

.redirectnextbyte
  STA newoswrch+4+1

  LDA &20E
  STA newoswrch+4

  LDA #(newoswrch MOD 256)
  STA &20E
  LDA #(newoswrch DIV 256)
  STA &20F
  LDA #&75:JSR OSBYTE   :\ Read VDU status
  TXA:AND #&10:CMP #&10 :\ Test shadow flag in bit 4
  PHP                   :\ Save shadow flag in Carry
  LDA #&A0 : LDX #&55
  JSR OSBYTE  :\ Read current MODE
  TXA :ASL A:PLP:ROR A   :\ Move shadow flag into bit 7

  ; Change mode so that both screens match
  PHA
  LDA #22: JSR &FFEE
  PLA : JSR &FFEE
  RTS

.setupredirectorwithmessage
  JSR setupredirector

  PRTSTRING " Screen Redirector enabled."
  JSR OSNEWL
  JSR OSNEWL

.oswtchredirectexit
  PAGERTS

  ENDBLOCK &200
}

; Page 3
; ADFS
{
ORG &FD00
    LOADFILETOSWR "Pi1MHz/ADFS.rom"
    ENDBLOCK &300
}

; Page 4
; MMFS
{
ORG &FD00
    LOADFILETOSWR "Pi1MHz/SWMMFS.rom"
    ENDBLOCK &400
}

; Page 5
; MMFSv2
{
ORG &FD00
    LOADFILETOSWR "Pi1MHz/SWMMFS2.rom"
    ENDBLOCK &500
}
; Page 6
; BSROM
{
ORG &FD00
    LOADFILETOSWR "Pi1MHz/BSrom.rom"
    ENDBLOCK &600
}
; Page 7
; VFSROM
{
ORG &FD00
    LOADFILETOSWR "Pi1MHz/ATS.rom"
    ENDBLOCK &700
}
; Page 8
; ANFS for the BEEB
{
ORG &FD00
    LOADFILETOSWR "Pi1MHz/AUNFSBEEB.rom"
    ENDBLOCK &800
}
; Page 9
; ANFS for the M128
{
ORG &FD00
    LOADFILETOSWR "Pi1MHz/AUNFSM128.rom"
    ENDBLOCK &900
}
; Page 10
; User ROM10
{
ORG &FD00
    LOADFILETOSWR "Pi1MHz/ROM10.rom"
    ENDBLOCK &A00
}
; Page 11
; User ROM11
{
ORG &FD00
    LOADFILETOSWR "Pi1MHz/ROM11.rom"
    ENDBLOCK &B00
}
; Page 12
; User ROM12
{
ORG &FD00
    LOADFILETOSWR "Pi1MHz/ROM12.rom"
    ENDBLOCK &C00
}
; Page 13
; User ROM13
{
ORG &FD00
    LOADFILETOSWR "Pi1MHz/ROM13.rom"
    ENDBLOCK &D00
}
; Page 14
; User ROM14
{
ORG &FD00
    LOADFILETOSWR "Pi1MHz/ROM14.rom"
    ENDBLOCK &E00
}
; Page 15
; User ROM15
{
ORG &FD00
    LOADFILETOSWR "Pi1MHz/ROM15.rom"
    ENDBLOCK &F00
}

; Page 16
; 1MHz-WiFi with the UEF cassette filing system merged in (beeb/1mhz-wifi
; provides the WiFi half; the merged image carries a different licence -
; see CREDITS.md).
{
ORG &FD00
    LOADFILETOSWR "Pi1MHz/1mhz-wicfs.rom"
    ENDBLOCK &1000
}

; ---------------------------------------------------------------------------
; Page 17 : SD explorer entry - save state, trap BRK, set up MODE 7 screen
; ---------------------------------------------------------------------------
{
ORG &FD00
    NOP                     ; &FD01 is the GOTOPAGE-reachable entry
    TSX                     ; stack level to unwind to after a BRK
    STX old_stack
    LDA #4                  ; cursor keys return &88-&8B
    LDX #1
    LDY #0
    JSR OSBYTE
    STX old_fx4
    LDA #229                ; ESC returns ASCII 27
    LDX #1
    LDY #0
    JSR OSBYTE
    STX old_fx229
    LDX #7                  ; BRK redirector into page EXP_ERR
.stubcopy
    LDA stub,X
    STA brk_stub,X
    DEX
    BPL stubcopy
    LDA &0202
    STA old_brkv
    LDA &0203
    STA old_brkv_hi
    LDA #brk_stub AND &FF
    STA &0202
    LDA #brk_stub DIV 256
    STA &0203
    LDA #0
    STA zp_fshand
    LDX #0
.vduloop
    LDA vdutab,X
    JSR OSWRCH
    INX
    CPX #vdutabend-vdutab
    BNE vduloop
    GOTOPAGE EXP_DIR, 1

.stub
    EQUB &A9, EXP_ERR       ; LDA #EXP_ERR
    EQUB &8D, &88, &FC      ; STA &FC88
    EQUB &4C, &00, &FD      ; JMP &FD00

.vdutab
    EQUB 22,7                       ; MODE 7
    EQUB 23,1,0,0,0,0,0,0,0,0       ; cursor off
    EQUB 134 : EQUS "Pi1MHz SD card explorer"
    EQUB 31,0,24                    ; footer
    EQUB 133 : EQUS "RET=open/get P=put <-=up ESC=quit"
.vdutabend

    ASSERT P% <= &FE00-9
    PAGESWITCH
    ENDBLOCK &1100
}


; ---------------------------------------------------------------------------
; Pages 18-30 : reserved for future helpers
;
; Free slots, so a new helper can be added without moving the explorer's
; pages and rewriting every GOTOPAGE in it.  Each answers like any finished
; helper page, so running one before it does anything simply returns.
; ---------------------------------------------------------------------------

{
    ORG &FD00
    PAGERTS

    ENDBLOCK &1200
}

{
    ORG &FD00
    PAGERTS

    ENDBLOCK &1300
}

{
    ORG &FD00
    PAGERTS

    ENDBLOCK &1400
}

{
    ORG &FD00
    PAGERTS

    ENDBLOCK &1500
}

{
    ORG &FD00
    PAGERTS

    ENDBLOCK &1600
}

{
    ORG &FD00
    PAGERTS

    ENDBLOCK &1700
}

{
    ORG &FD00
    PAGERTS

    ENDBLOCK &1800
}

{
    ORG &FD00
    PAGERTS

    ENDBLOCK &1900
}

{
    ORG &FD00
    PAGERTS

    ENDBLOCK &1A00
}

{
    ORG &FD00
    PAGERTS

    ENDBLOCK &1B00
}

{
    ORG &FD00
    PAGERTS

    ENDBLOCK &1C00
}

{
    ORG &FD00
    PAGERTS

    ENDBLOCK &1D00
}

{
    ORG &FD00
    PAGERTS

    ENDBLOCK &1E00
}


; ---------------------------------------------------------------------------
; Page 31 : key dispatch loop
; ---------------------------------------------------------------------------
{
ORG &FD00
    NOP
.keyloop                    ; &FD01
    JSR OSRDCH
    CMP #27
    BNE notesc
    GOTOPAGE EXP_EXIT, 1
.notesc
    CMP #&8B
    BEQ up
    CMP #&8A
    BEQ down
    CMP #&88
    BEQ left
    CMP #13
    BEQ enter
    AND #&DF                ; fold lower case
    CMP #'G'
    BEQ enter
    CMP #'U'
    BEQ left
    CMP #'P'
    BEQ doput
    JMP keyloop

.up
    LDA zp_sel
    BEQ keyloop
    STA zp_oldsel
    DEC zp_sel
    LDA zp_sel
    CMP zp_top
    BCS tworow              ; still on screen
    SEC                     ; off the top: show the previous page whole,
    SBC #19                 ; with the selection on its last row
    BCS settop
    LDA #0                  ; first page
.settop
    STA zp_top
    JMP fullrows
.down
    LDX zp_sel
    INX
    CPX zp_count
    BCS keyloop
    LDA zp_sel
    STA zp_oldsel
    STX zp_sel
    TXA
    SEC
    SBC #19
    BCC tworow              ; sel < 19 always fits
    CMP zp_top
    BCC tworow
    BEQ tworow
    STX zp_top              ; off the bottom: start a new page here (X = sel)
.fullrows
    GOTOPAGE EXP_DRAW, 4
.tworow
    GOTOPAGE EXP_DRAW, 1
.left
    GOTOPAGE EXP_CD, 4
.enter
    LDA zp_count
    BEQ keyloop
    LDA zp_sel
    SETRECPTR 0
    LDA discaccess+3        ; attribute byte
    AND #AM_DIR
    BNE isdir
    GOTOPAGE EXP_GET1, 1
.isdir
    GOTOPAGE EXP_CD, 1
.doput
    GOTOPAGE EXP_PUT1, 1

    ASSERT P% <= &FE00-9
    PAGESWITCH
    ENDBLOCK &1F00
}

; ---------------------------------------------------------------------------
; Page 32 : read current directory into records at &D00000
; ---------------------------------------------------------------------------
{
ORG &FD00
    NOP                     ; &FD01
    SETCMDPTR &FC
    LDA #7                  ; fopendir
    STA discaccess+3
    LDA #0                  ; "" = current directory
    STA discaccess+3
    DOCMD &FC
    LDA #0
    STA zp_count
.rdloop
    LDA #0                  ; command block again (mid/hi still &FC/&FF)
    STA discaccess
    LDA #17                 ; readdir-ex
    STA discaccess+3
    LDA #4                  ; skip to destination field
    STA discaccess
    LDA zp_count            ; dest = &D00000 + count*128
    LSR A
    TAX
    LDA #0
    ROR A
    STA discaccess+3        ; dest lo
    TXA
    STA discaccess+3        ; dest mid
    LDA #recs_hi
    STA discaccess+3        ; dest hi
    LDA #0
    STA discaccess+3        ; dest top byte must be 0
    DOCMD &FC
    BNE rddone              ; 20 = no more entries (or error)
    INC zp_count
    LDA zp_count
    CMP #250
    BCC rdloop
.rddone
    LDA #0                  ; fclosedir
    STA discaccess
    LDA #8
    STA discaccess+3
    DOCMD &FC
    LDA #0
    STA zp_sel
    STA zp_top
    GOTOPAGE EXP_DRAW2, 1

    ASSERT P% <= &FE00-9
    PAGESWITCH
    ENDBLOCK &2000
}

; ---------------------------------------------------------------------------
; Page 33 : draw path row, then fall on to the rows
; ---------------------------------------------------------------------------
{
ORG &FD00
    NOP                     ; &FD01
    SETCMDPTR &FC
    LDA #18                 ; getcwd -> &D80000
    STA discaccess+3
    LDA #4
    STA discaccess
    LDA #0
    STA discaccess+3        ; dest lo
    STA discaccess+3        ; dest mid
    LDA #chunk_hi
    STA discaccess+3        ; dest hi
    LDA #0
    STA discaccess+3
    DOCMD &FC
    LDA #31                 ; TAB(0,1)
    JSR OSWRCH
    LDA #0
    JSR OSWRCH
    LDA #1
    JSR OSWRCH
    LDA #130                ; green
    JSR OSWRCH
    LDA #0
    STA discaccess
    STA discaccess+1
    LDA #chunk_hi
    STA discaccess+2
    LDX #38
.ploop
    LDA discaccess+3
    BEQ pad
    JSR OSWRCH
    DEX
    BNE ploop
    BEQ prows
.pad
    LDA #' '
.padloop
    JSR OSWRCH
    DEX
    BNE padloop
.prows
    GOTOPAGE EXP_DRAW, 4

    ASSERT P% <= &FE00-9
    PAGESWITCH
    ENDBLOCK &2100
}

; ---------------------------------------------------------------------------
; Page 34 : row rendering
;   entry &FD01 : redraw oldsel + sel rows (selection moved)
;   entry &FD04 : redraw all 20 rows
; ---------------------------------------------------------------------------
{
ORG &FD00
    NOP
    JMP tworow              ; &FD01
    JMP rowsall             ; &FD04
.tworow
    LDA zp_oldsel
    STA zp_idx
    JSR drawrow
    LDA zp_sel
    STA zp_idx
    JSR drawrow
    GOTOPAGE EXP_KEY, 1
.rowsall
    LDA zp_top
    STA zp_idx
    LDA #20
    STA zp_rows
.rowloop
    JSR drawrow
    INC zp_idx
    DEC zp_rows
    BNE rowloop
    GOTOPAGE EXP_KEY, 1

.drawrow
    LDA zp_idx              ; screen row = idx - top + 2
    SEC
    SBC zp_top
    CLC
    ADC #2
    TAY
    LDA #31
    JSR OSWRCH
    LDA #0
    JSR OSWRCH
    TYA
    JSR OSWRCH
    LDA zp_idx
    CMP zp_count
    BCS blankrow
    LDA zp_idx
    SETRECPTR 0
    LDA discaccess+3        ; attribute
    AND #AM_DIR
    BEQ isfile
    LDA #131                ; yellow directory
    BNE selchk
.isfile
    LDA #135                ; white file
.selchk
    LDX zp_idx
    CPX zp_sel
    BNE notsel
    LDA #130                ; green selection
.notsel
    JSR OSWRCH
    LDA zp_idx              ; back to record+8: the display line
    LSR A
    LDA #0
    ROR A
    ORA #8
    STA discaccess
    LDX #38
.dloop
    LDA discaccess+3
    JSR OSWRCH
    DEX
    BNE dloop
    RTS
.blankrow
    LDX #39
    LDA #' '
.bloop
    JSR OSWRCH
    DEX
    BNE bloop
    RTS

    ASSERT P% <= &FE00-9
    PAGESWITCH
    ENDBLOCK &2200
}

; ---------------------------------------------------------------------------
; Page 35 : change directory
;   entry &FD01 : into the selected entry     entry &FD04 : up ".."
; ---------------------------------------------------------------------------
{
ORG &FD00
    NOP
    JMP cdsel               ; &FD01
    JMP cdup                ; &FD04
    JMP installed           ; &FD07 : transfer page ready
    JMP gotname             ; &FD0A : raw name now in the transfer page
    JMP named               ; &FD0D : ... and in the command block
.cdsel
    XINSTALL EXP_CD, 7
.installed
    PLP
    LDA zp_sel              ; raw name out of the record
    SETRECPTR 48
    LDX #63
    XCALL XE_P2PAGE, EXP_CD, 10
.gotname
    PLP
    SETCMDPTR &FC
    LDA #11                 ; fchdir
    STA discaccess+3
    XCALL XE_P2PORT, EXP_CD, 13
.named
    PLP
.dispatch
    DOCMD &FC
    GOTOPAGE EXP_DIR, 1     ; reread (on error the cwd is unchanged)
.cdup
    SETCMDPTR &FC
    LDA #11
    STA discaccess+3
    LDA #'.'
    STA discaccess+3
    STA discaccess+3
    LDA #0
    STA discaccess+3
    JMP dispatch

    ASSERT P% <= &FE00-9
    PAGESWITCH
    ENDBLOCK &2300
}

; ---------------------------------------------------------------------------
; Page 36 : get (SD -> current FS), part 1 - choose the FS filename
; ---------------------------------------------------------------------------
{
ORG &FD00
    NOP
    JMP start               ; &FD01
    JMP installed           ; &FD04
    JMP typed               ; &FD07
    JMP defaulted           ; &FD0A
.start
    LDA #0
    STA zp_fail
    STA zp_reread
    XINSTALL EXP_GET1, 4
.installed
    PLP
    JSR print
    EQUB 31,0,23,135 : EQUS "Copy as: " : EQUB &FF
    XCALL XE_OSWORD, EXP_GET1, 7
.typed
    PLP
    BCS cancel
    TXA                     ; typed length
    BNE gotname
    LDA zp_sel              ; empty input: default to the SD name
    SETRECPTR 48
    LDX #30
    XCALL XE_P2PAGE, EXP_GET1, 10
.defaulted
    PLP
.gotname
    GOTOPAGE EXP_GET2, 1
.cancel
    GOTOPAGE EXP_KEY, 1

.print
    PRSUB

    ASSERT P% <= &FE00-9
    PAGESWITCH
    ENDBLOCK &2400
}

; ---------------------------------------------------------------------------
; Page 37 : get, part 2 - open both files
; ---------------------------------------------------------------------------
{
ORG &FD00
    NOP
    JMP start               ; &FD01
    JMP opened              ; &FD04 : OSFIND returned, X = handle
    JMP gotname             ; &FD07 : raw SD name now in the transfer page
    JMP named               ; &FD0A : ... and in the command block
.start
    LDX #&80                ; OPENOUT the typed name on the current FS
    XCALL XE_OSFIND, EXP_GET2, 4
.opened
    PLP
    STX zp_fshand
    TXA                     ; handle 0 and no BRK: report failure
    BNE openfs
    LDA #1
    STA zp_fail
    GOTOPAGE EXP_FIN, 1
.openfs
    LDA zp_sel              ; raw SD name out of the record
    SETRECPTR 48
    LDX #63
    XCALL XE_P2PAGE, EXP_GET2, 7
.gotname
    PLP
    SETCMDPTR &FD
    LDA #2                  ; fopen
    STA discaccess+3
    LDA #0
    STA discaccess+3
    LDA #1                  ; FA_READ
    STA discaccess+3
    XCALL XE_P2PORT, EXP_GET2, 10
.named
    PLP
    DOCMD &FD
    BEQ openok
    LDA #1
    STA zp_fail
    GOTOPAGE EXP_FIN, 1
.openok
    LDA #0
    STA zp_off
    STA zp_off+1
    STA zp_off+2
    GOTOPAGE EXP_GET3, 1

    ASSERT P% <= &FE00-9
    PAGESWITCH
    ENDBLOCK &2500
}

; ---------------------------------------------------------------------------
; Page 38 : get, part 3 - fread a chunk straight into the transfer page and
;           have its stub OSGBPB it to the FS; EXP_GET4 loops back to &FD04
; ---------------------------------------------------------------------------
{
ORG &FD00
    NOP
    JMP init                ; &FD01
    JMP chunk               ; &FD04 : loop re-entry from EXP_GET4
.init
    LDA #0
    STA zp_eof
.chunk
    SETCMDPTR &FD
    LDA #4                  ; fread
    STA discaccess+3
    LDA #XFER_LEN
    STA discaccess+3        ; length XFER_LEN
    LDA #0
    STA discaccess+3
    STA discaccess+3
    LDA #XFER_DATA AND &FF  ; dest = the transfer page's data area
    STA discaccess+3
    LDA #0
    STA discaccess+3
    LDA #xfer_hi
    STA discaccess+3
    LDA #0
    STA discaccess+3
    LDA zp_off              ; file offset
    STA discaccess+3
    LDA zp_off+1
    STA discaccess+3
    LDA zp_off+2
    STA discaccess+3
    LDA #0
    STA discaccess+3
    DOCMD &FD
    BEQ full
    CMP #20                 ; short read = final chunk
    BEQ short1
    LDA #1
    STA zp_fail
    GOTOPAGE EXP_FIN, 1
.full
    LDA #XFER_LEN
    STA zp_len
    LDA #0
    STA zp_len+1
    BEQ write               ; always
.short1
    LDA #1
    STA zp_eof
    STA discaccess          ; read back actual length from cmd+1/2
    LDA discaccess+3
    STA zp_len
    LDA discaccess+3
    STA zp_len+1
    ORA zp_len
    BEQ closeup
.write
    LDX #2                  ; OSGBPB 2: write the chunk to the FS file
    XCALL XE_GBPB, EXP_GET4, 1
.closeup
    GOTOPAGE EXP_FIN, 1

    ASSERT P% <= &FE00-9
    PAGESWITCH
    ENDBLOCK &2600
}

; ---------------------------------------------------------------------------
; Page 39 : put (current FS -> SD), part 1 - name prompt and FS open
; ---------------------------------------------------------------------------
{
ORG &FD00
    NOP
    JMP start               ; &FD01
    JMP installed           ; &FD04
    JMP typed               ; &FD07
    JMP opened              ; &FD0A : OSFIND returned, X = handle
.start
    LDA #0
    STA zp_fail
    LDA #1
    STA zp_reread           ; a new SD file appears: reread after
    XINSTALL EXP_PUT1, 4
.installed
    PLP
    JSR print
    EQUB 31,0,23,135 : EQUS "Put file: " : EQUB &FF
    XCALL XE_OSWORD, EXP_PUT1, 7
.typed
    PLP
    BCS cancel
    TXA                     ; typed length
    BEQ cancel
    LDX #&40                ; OPENIN the typed name on the current FS
    XCALL XE_OSFIND, EXP_PUT1, 10
.opened
    PLP
    TXA                     ; set Z from the handle: 0 = not found
    BNE gotfile
    JSR print
    EQUB 31,0,23,129 : EQUS "Not found         " : EQUB &FF
.cancel
    GOTOPAGE EXP_KEY, 1
.gotfile
    STA zp_fshand
    GOTOPAGE EXP_PUT2, 1

.print
    PRSUB

    ASSERT P% <= &FE00-9
    PAGESWITCH
    ENDBLOCK &2700
}

; ---------------------------------------------------------------------------
; Page 40 : put, part 2 - create the SD file, have the transfer page's stub
;           OSGBPB FS chunks into its data area; page EXP_PUT3 does the SD
;           write, looping to &FD04
; ---------------------------------------------------------------------------
{
ORG &FD00
    NOP
    JMP create              ; &FD01
    JMP chunk               ; &FD04 : loop re-entry from EXP_PUT3
    JMP named               ; &FD07 : name now in the fopen block
    JMP gotchunk            ; &FD0A : OSGBPB 4 returned
.create
    SETCMDPTR &FD
    LDA #2                  ; fopen
    STA discaccess+3
    LDA #0
    STA discaccess+3
    LDA #&0A                ; FA_CREATE_ALWAYS | FA_WRITE
    STA discaccess+3
    XCALL XE_P2PORT, EXP_PUT2, 7    ; the typed name is still in xname
.named
    PLP
    DOCMD &FD
    BEQ createok
    LDA #1
    STA zp_fail
    GOTOPAGE EXP_FIN, 1
.createok
    LDA #0
    STA zp_off
    STA zp_off+1
    STA zp_off+2
.chunk
    LDA #XFER_LEN
    STA zp_len
    LDA #0
    STA zp_len+1
    LDX #4                  ; OSGBPB 4: read a chunk from the FS file
    XCALL XE_GBPB, EXP_PUT2, 10
.gotchunk
    PLP                     ; C set = EOF reached ...
    PHP                     ; ... kept for EXP_PUT3
    LDA #XBLK_COUNT AND &FF ; remaining count, updated in place by OSGBPB
    STA discaccess
    LDA #0
    STA discaccess+1
    LDA #xfer_hi
    STA discaccess+2
    SEC                     ; transferred = XFER_LEN - remaining
    LDA #XFER_LEN
    SBC discaccess+3
    STA zp_len
    LDA #0
    SBC discaccess+3
    STA zp_len+1
    ORA zp_len
    BEQ lastnone
    GOTOPAGE EXP_PUT3, 1
.lastnone
    PLP
    GOTOPAGE EXP_FIN, 1

    ASSERT P% <= &FE00-9
    PAGESWITCH
    ENDBLOCK &2800
}

; ---------------------------------------------------------------------------
; Page 41 : finish a transfer - close both files, report, route on
; ---------------------------------------------------------------------------
{
ORG &FD00
    NOP                     ; &FD01
    LDY zp_fshand
    BEQ nofs
    LDA #0
    STA zp_fshand           ; clear first: a BRK in close cannot loop
    JSR OSFIND
.nofs
    SETCMDPTR &FD
    LDA #3                  ; fclose (error ignored)
    STA discaccess+3
    DOCMD &FD
    LDA zp_fail
    BNE badmsg
    JSR print
    EQUB 31,0,23,130 : EQUS "Done                " : EQUB &FF
    JMP route
.badmsg
    JSR print
    EQUB 31,0,23,129 : EQUS "Failed              " : EQUB &FF
.route
    LDA zp_reread
    BEQ tokey
    GOTOPAGE EXP_DIR, 1
.tokey
    GOTOPAGE EXP_KEY, 1

.print
    PRSUB

    ASSERT P% <= &FE00-9
    PAGESWITCH
    ENDBLOCK &2900
}

; ---------------------------------------------------------------------------
; Page 42 : BRK trap - a filing system error unwound to here
; ---------------------------------------------------------------------------
{
ORG &FD00
    NOP                     ; also reached by JMP &FD00 from the RAM stub
    LDX old_stack           ; unwind to the CALL-time stack level
    TXS
    JSR print
    EQUB 31,0,23,129 : EQUB &FF
    LDY #1                  ; error string sits after the BRK (ptr at &FD)
.errloop
    LDA (&FD),Y
    BEQ errdone
    JSR OSWRCH
    INY
    BNE errloop
.errdone
    JSR print
    EQUS " - press a key" : EQUB &FF
    LDY zp_fshand           ; close anything the transfer left open
    BEQ nofs
    LDA #0
    STA zp_fshand
    JSR OSFIND
.nofs
    SETCMDPTR &FD
    LDA #3
    STA discaccess+3
    DOCMD &FD
    JSR OSRDCH
    GOTOPAGE EXP_DIR, 1

.print
    PRSUB

    ASSERT P% <= &FE00-9
    PAGESWITCH
    ENDBLOCK &2A00
}

; ---------------------------------------------------------------------------
; Page 43 : exit - restore vectors, keys and cursor
; ---------------------------------------------------------------------------
{
ORG &FD00
    NOP                     ; &FD01
    LDA old_brkv
    STA &0202
    LDA old_brkv_hi
    STA &0203
    LDA #4
    LDX old_fx4
    LDY #0
    JSR OSBYTE
    LDA #229
    LDX old_fx229
    LDY #0
    JSR OSBYTE
    LDX #0
.vduloop
    LDA vdutab,X
    JSR OSWRCH
    INX
    CPX #vdutabend-vdutab
    BNE vduloop
    PAGERTS

.vdutab
    EQUB 23,1,1,0,0,0,0,0,0,0       ; cursor on
    EQUB 31,0,22                    ; park the cursor above the footer
.vdutabend

    ASSERT P% <= &FE00-9
    PAGESWITCH
    ENDBLOCK &2B00
}

; ---------------------------------------------------------------------------
; Page 44 : get, part 4 - the chunk is on the FS (the transfer page's stub
;           did the OSGBPB): advance, loop or finish
; ---------------------------------------------------------------------------
{
ORG &FD00
    NOP                     ; &FD01
    PLP                     ; the stub's saved status
    CLC
    LDA zp_off
    ADC zp_len
    STA zp_off
    LDA zp_off+1
    ADC zp_len+1
    STA zp_off+1
    LDA zp_off+2
    ADC #0
    STA zp_off+2
    LDA zp_eof
    BNE closeup
    GOTOPAGE EXP_GET3, 4
.closeup
    GOTOPAGE EXP_FIN, 1

    ASSERT P% <= &FE00-9
    PAGESWITCH
    ENDBLOCK &2C00
}

; ---------------------------------------------------------------------------
; Page 45 : put, part 3 - fwrite the staged chunk to SD, loop or finish
;           (a saved status byte from OSGBPB - C = EOF - is on the stack)
; ---------------------------------------------------------------------------
{
ORG &FD00
    NOP                     ; &FD01
    SETCMDPTR &FD
    LDA #5                  ; fwrite
    STA discaccess+3
    LDA zp_len
    STA discaccess+3
    LDA zp_len+1
    STA discaccess+3
    LDA #0
    STA discaccess+3
    LDA #XFER_DATA AND &FF  ; source = the transfer page's data area
    STA discaccess+3
    LDA #0
    STA discaccess+3
    LDA #xfer_hi
    STA discaccess+3
    LDA #0
    STA discaccess+3
    LDA zp_off
    STA discaccess+3
    LDA zp_off+1
    STA discaccess+3
    LDA zp_off+2
    STA discaccess+3
    LDA #0
    STA discaccess+3
    DOCMD &FD
    BEQ writeok
    PLP                     ; discard saved EOF state
    LDA #1
    STA zp_fail
    GOTOPAGE EXP_FIN, 1
.writeok
    CLC
    LDA zp_off
    ADC zp_len
    STA zp_off
    LDA zp_off+1
    ADC zp_len+1
    STA zp_off+1
    LDA zp_off+2
    ADC #0
    STA zp_off+2
    PLP
    BCS finished
    GOTOPAGE EXP_PUT2, 4
.finished
    GOTOPAGE EXP_FIN, 1

    ASSERT P% <= &FE00-9
    PAGESWITCH
    ENDBLOCK &2D00
}

; ---------------------------------------------------------------------------
; Page 46 : transfer page template, installed at &D90000 (see the notes at
;           the top of the SD explorer).  Only entry 1 runs in this page; the
;           other entries run in the installed copy, shown by &FC88 = XFER_SEL.
; ---------------------------------------------------------------------------
{
ORG &FD00
    NOP
    JMP selfcopy            ; &FD01 XE_INSTALL - template only
    JMP osword              ; &FD04 XE_OSWORD
    JMP osfind              ; &FD07 XE_OSFIND
    JMP gbpb                ; &FD0A XE_GBPB
    JMP port2page           ; &FD0D XE_P2PAGE
    JMP page2port           ; &FD10 XE_P2PORT

; ---- persistent: survives the file data a transfer puts in XFER_DATA ----
.gbpb                       ; X = function, zp_fshand = handle, zp_len = count
    LDA zp_fshand
    STA XBLK
    LDA #XFER_DATA AND &FF  ; OSGBPB advances the address: reset it each call
    STA XBLK+1
    LDA zp_len              ; ... and counts the count down: reset that too
    STA XBLK+5
    LDA zp_len+1
    STA XBLK+6
    TXA
    LDX #XBLK AND &FF
    LDY #XBLK DIV 256
    JSR OSGBPB
.ret                        ; every stub leaves through here
    PHP
    LDA zp_ret
    LDY zp_rent
    JMP &FDF7

    ASSERT P% = XBLK
    EQUB 0                  ; +0 handle
    EQUB XFER_DATA AND &FF, XFER_DATA DIV 256, &FF, &FF   ; +1 address, I/O
    EQUD 0                  ; +5 count
    EQUD 0                  ; +9 pointer (unused: sequential calls)

; ---- XFER_DATA: file data during a transfer, these setup stubs before one ----
    ASSERT P% = XFER_DATA
.xname                      ; name buffer, CR terminated (63 + CR)
FOR n, 1, 64
    EQUB 0
NEXT
.xword                      ; OSWORD 0 block: into xname, max 30, chars 32-126
    EQUB xname AND &FF, xname DIV 256, 30, 32, 126
.osword
    LDA #0
    LDX #xword AND &FF
    LDY #xword DIV 256
    JSR OSWORD
    TYA                     ; length in X: Y does not survive the switch
    TAX
    JMP ret
.osfind                     ; X = mode
    TXA
    LDX #xname AND &FF
    LDY #xname DIV 256
    JSR OSFIND
    TAX                     ; handle in X
    JMP ret
.port2page                  ; X = max length; NUL-terminated at the port
    LDY #0
.p2loop
    LDA discaccess+3
    BEQ p2end
    STA xname,Y
    INY
    DEX
    BNE p2loop
.p2end
    LDA #13
    STA xname,Y
    TYA                     ; length in X
    TAX
    JMP ret
.page2port                  ; xname -> port, NUL terminated
    LDY #0
.ploop
    LDA xname,Y
    CMP #13
    BEQ pend
    STA discaccess+3
    INY
    CPY #63
    BNE ploop
.pend
    LDA #0
    STA discaccess+3
    JMP ret
.selfcopy                   ; template only: copy this whole page to &D90000
    LDA #0
    STA discaccess
    STA discaccess+1
    LDA #xfer_hi
    STA discaccess+2
    LDY #0
.scloop
    LDA &FD00,Y
    STA discaccess+3
    INY
    BNE scloop
    JMP ret

    ASSERT P% <= &FE00-9
    PAGESWITCH
    ENDBLOCK &2E00
}

.end

SAVE "../firmware/Pi1MHz/6502code.bin" , 0, &2F00
