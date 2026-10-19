\ telnetd.rom - run helper 20 (the telnet daemon) on every reset.
\
\ Helper 20 hooks the Beeb's keyboard and RS423 output to the Pi, and a BREAK
\ undoes the hooks.  This sideways ROM puts them back: the MOS offers every ROM
\ service call 1 on a reset, and this answers it with the equivalent of
\
\       X%=20 : CALL &FC88
\
\ so a telnet session survives BREAK and CTRL-BREAK with nothing to type.
\
\ It also lets the Pi ask what is on the screen, silently.  The Pi cannot read the
\ Beeb's screen and a program cannot be interrupted to read it, but the foreground
\ code that is waiting for a key calls the buffer-remove vector (REMV) over and over,
\ so this ROM takes that vector (as an extended vector, which pages this ROM in).  The
\ Pi puts one byte, &FF, in the keyboard buffer; REMV sees it come out, keeps it from
\ the program, reads every character cell of the screen (OSBYTE 135) and sends the ones
\ that are not blank to the Pi as VDU 31,x,y,char with the screen drivers switched off,
\ so nothing shows on the Beeb, and puts the cursor back.  It runs on the host, so it
\ works with a second processor on.  Nothing is kept in zero page, which the MOS's
\ OSBYTE and VDU routines use: only the stack.  No workspace, no commands, no language.
\
\ Put it in a ROM socket (an EPROM), or load it into sideways RAM like any other
\ ROM image.  On a BBC B press BREAK; on a Master 128 also *INSERT its bank, which
\ the MOS needs for a ROM that appeared after the last reset (*ROMS shows the bank).  The same image works on
\ a B, B+ and Master 128: plain 6502, and the only OS call is OSBYTE &FD.
\
\ POWER-ON.  The Pi is usually powered from the Beeb, so at power-on it is
\ booting while the Beeb runs its ROM scan, and its window at &FC88 does not
\ answer for about two seconds.  After a power-on reset (OSBYTE &FD, X=1) this
\ waits, up to about 3.6 seconds, for the window to answer before running the
\ helper (any key pressed ends the wait at once); after any other reset the Pi is
\ already up and it does not wait.  If no Pi ever answers it gives up and the Beeb
\ carries on booting.  The window counts as there only when all three bytes of
\ its STX &FC88 read back, so an empty bus never passes for a Pi.
\
\ Service call 1 is used, not 3, for the reason the 1MHz-WiFi ROM gives: a
\ filing system that claims 3 stops the call reaching a ROM below it, and 1 is
\ never claimed.  A, X and Y are given back as they came - the MOS keeps its
\ slot number in X, and a ROM that does not claim the call must not change Y.
\
\ ROM FILING SYSTEM.  While the cassette or ROM filing system runs it owns the 6850,
\ and the MOS sends nothing to RS423 (OS 1.2 &E0DC): what *CAT and friends print is
\ on the Beeb's screen only.  Every ROM is offered service call &0D for each such
\ operation, so this passes it on untouched and tells the Pi (&22 to the stub's
\ control port), which then asks for the screen as above, after the operation.
\
\ Build: beeb/telnetd-rom/build.sh (needs beebasm).

OSBYTE = &FFF4
OSNEWL = &FFE7
CMDTAIL = &F2               \ service call 9: the *HELP line, Y its offset
OSWRCH = &FFEE
CMDPTR = &F2                \ service call 4: the command line, and Y its offset
HELPER_ENTRY = &FC88        \ the helper window: STX &FC88 / NOP / JMP &FD00
HELPER_OPCODE = &8E         \ the Pi writes STX abs here, so it is there only if a Pi1MHz answers
TELNET_HELPER = 20
BUILD = 15           \ the build number in the version string, so *HELP says which image is fitted
INSV = &022A                \ the MOS vectors: keyboard buffer insertion ...
IRQ1V = &0204               \ ... and the interrupt
REMV = &022C                \ buffer remove, vector 22: foreground code polls it while it waits for a key
EXTVEC = &0D9F              \ the extended vector table: 3 bytes per vector (address, ROM number)
REMV_ENTRY = &FF42          \ the MOS's entry for vector 22 as an extended vector (&FF00 + 3*22)
SCANKEY = &FF               \ the byte the Pi puts in the keyboard buffer to ask for the screen
LITKEY = &FB                \ ... and before a byte to be read as it is (above 127: no soft or cursor key)
RDCHV = &0210               \ the read-character vector, vector 8
RDCHV_ENTRY = &FF18         \ the MOS's entry for vector 8 as an extended vector (&FF00 + 3*8)
KEYV = &0228                \ the keyboard vector, vector 20
KEYV_ENTRY = &FF3C          \ the MOS's entry for vector 20 as an extended vector (&FF00 + 3*20)
DEFVEC = &FFB7              \ the MOS: where its table of the default vectors is (OS 1.20 &D940)
ZPTMP = &A8                 \ two bytes of the OS commands' workspace, kept on the stack while used
STUBPAGE = &FC              \ the stub's page (FRED); only a test build, in RAM, changes it
STUB = &FCCC                \ the Pi's serial stub at its default place (src/serial_redirect.c)
\ The ROM's workspace: 8 bytes the Pi keeps in FRED just below the stub's escape routine
\ (SER_WS_OFF in src/serial_redirect.c), so the ROM owns no Beeb RAM.  They are there only
\ with the Pi, and the ROM hooks the Beeb in only with the Pi, the stub where it always is.
WS = STUB - 21
REMVOLD = WS                \ the MOS's REMV routine (the claim goes on to it)
KEYVOLD = WS+2              \ ... its KEYV routine
RDCHVOLD = WS+4             \ ... its RDCHV routine
SHIFTFLAG = WS+6            \ the Pi's SHIFT request for paged mode: set by the Pi, cleared here
TELNETON = WS+7             \ 1 when telnetd is on (telnetd_port): only then is the Beeb hooked in
POWER_ON = 1                \ OSBYTE &FD: 0 soft BREAK, 1 power-on, 2 CTRL-BREAK
SOFT_BREAK = 0              \ OSBYTE &FD: a soft reset (the BREAK key, or a jump through the reset vector)
TUBE_ULA = &FEE0            \ the Tube ULA's status register: write &80+flags to set, flags to clear
WAIT_TURNS = 22             \ x about 0.16 s at 2 MHz: about 3.6 seconds (the Pi boots in about 2.5)
READ_TRIES = 4              \ OSBYTE 135 reads of a cell that comes back unrecognised (0)

ORG &8000
GUARD &C000

.romstart       equb 0                      \ no language entry
                equb 0
                equb 0
                jmp service
                equb &82                    \ service ROM, no language
                equb (copyright - romstart) \ copyright offset
                equb 1                      \ binary version
.romtitle       equs "TelnetD"
                equb 0
.romversion     equs "0.2 b" + STR$(BUILD)         \ *HELP shows it: BUILD is bumped for every image that is tried on a machine
.copyright      equb 0
                equs "(C)2026 Pi1MHz"
                equb 0

\ A is the reason code, X this ROM's slot, Y the reason argument.  Every call is
\ passed on with all three untouched: none is claimed.
.service        cmp #9
                bne notcmd9
                jmp help                    \ *HELP
.notcmd9        cmp #&0D
                bne notrfs
                pha                         \ a ROM filing system operation: tell the Pi, and pass it on
                tya
                pha
                lda #&22
                jsr marker
                pla
                tay
                pla
                rts
.notrfs         cmp #1
                bne done
                pha                         \ the MOS wants A, X and Y back
                txa
                pha
                tya
                pha
                lda #&FD                    \ how did this reset start?
                ldx #0
                ldy #&FF
                jsr OSBYTE                  \ X = 0, 1 or 2
                cpx #SOFT_BREAK
                bne notsoft
                lda #&A0                    \ a reset in software (CALL !-4, the telnet F12) did not
                sta TUBE_ULA                \ reset a second processor as the BREAK key does, and the
                lda #&20                    \ Tube's start would wait for it for ever: the Tube ULA's
                sta TUBE_ULA                \ parasite reset (flag P) on and off.  No Tube: nothing there
.notsoft        cpx #POWER_ON
                bne probe                   \ the Pi is already up
                lda #WAIT_TURNS
                pha                         \ turns left, on the stack: nothing else is ours
.wait           jsr stubok
                beq settle
                jsr helperok
                beq ready
                lda #&79                    \ a key pressed (from key 16: SHIFT and CTRL held for
                ldx #&10                    \ the boot do not count) ends the wait: a machine
                jsr OSBYTE                  \ without a Pi1MHz need not sit through it
                cpx #&FF
                bne ready
                jsr pause
                pla
                sec
                sbc #1
                pha
                bne wait
                beq ready
.settle         jsr pause                   \ the Pi is still setting the stub up: give it a moment
.ready          pla                         \ the turns left
.probe          jsr unstale
                jsr stubok                  \ the Pi's stub where it always is: hook it in here,
                bne nopi                    \ and only if telnetd is on.  Moved (Serial_addr=) or
                lda TELNETON                \ telnetd off: nothing is touched (helper 20 can still
                cmp #1                      \ hook a moved stub by hand: X%=20:CALL &FC88)
                bne nopi
                jsr install
.hooked         tsx
                lda &102,x                  \ this ROM's slot, as the MOS gave it in X
                jsr claimkeyv               \ A is kept
                jsr claimrdchv              \ A is kept
                jsr claimremv               \ A = what to tell the Pi: &21 if it can ask for the screen
                jsr marker
.nopi           pla
                tay
                pla
                tax
                pla
.done           rts

\ A software BREAK (CALL !-4, the reset vector) can leave the interrupt vector pointing
\ at the stub the helper installed before it, which the helper takes for another
\ program's hook and gives up.  A stub is recognised by its first two bytes (CPX #2) and
\ the vector pointing 12 into it; it kept the vector it replaced at +0D, which goes back.
.unstale        lda IRQ1V+1
                cmp #&FC
                bne unsx
                lda IRQ1V
                sec
                sbc #&0C
                tax                         \ X = where the stub would start
                lda &FC00,x
                cmp #&E0
                bne unsx
                lda &FC01,x
                cmp #2
                bne unsx
                lda &FC0D,x
                sta IRQ1V
                lda &FC0E,x
                sta IRQ1V+1
.unsx           rts

.pause          ldx #0                      \ about 0.16 s at 2 MHz
                ldy #0
.spin           dey
                bne spin
                dex
                bne spin
                rts

\ Z set if the Pi's helper window answers: all three bytes of its STX &FC88, not only the
\ opcode, so a bus with nothing on it (or something else in FRED) never passes for a Pi.
.helperok       lda HELPER_ENTRY
                cmp #HELPER_OPCODE
                bne hlpx
                lda HELPER_ENTRY+1
                cmp #<HELPER_ENTRY
                bne hlpx
                lda HELPER_ENTRY+2
                cmp #>HELPER_ENTRY
.hlpx           rts

\ Z set if the Pi's serial stub is at its usual place (&FCCC, Serial_addr's default):
\ its first instructions, CPX #2 / BNE, and its last, RTS.  The Pi writes it when it
\ starts, whether or not 6502code.bin (the helpers) is on the card.
.stubok         lda STUB
                cmp #&E0
                bne stubx
                lda STUB+1
                cmp #2
                bne stubx
                lda STUB+2
                cmp #&D0
                bne stubx
                lda STUB+&30
                cmp #&60
.stubx          rts

\ What helper 20 does, done here, so a ROM in an EPROM needs nothing from the card:
\ the keyboard insert vector and the interrupt vector into the stub (hookin), the Pi
\ told the MODE and the cursor and "telnet" (&80+column, &E0+row, &10+MODE to its
\ control port), the 6850 quiet (RTS high, no interrupts, its format and divider
\ kept), and *FX3,1 so what is printed goes to RS423, which is the Pi.
.install        jsr hookin
                bcs instx
                lda #135                    \ the MODE (Y) ...
                jsr OSBYTE
                tya
                and #7
                ora #&10
                pha
                lda #134                    \ ... and the cursor: X column, Y row
                jsr OSBYTE
                tya
                ora #&E0
                pha
                txa
                ora #&80
                sta STUB+&28                \ &80 + column
                pla
                sta STUB+&28                \ &E0 + row
                pla
                sta STUB+&28                \ &10 + MODE: telnet mode, and the end of the report
                lda #156
                ldx #&40
                ldy #&1F
                jsr OSBYTE
                lda #3
                ldx #1
                ldy #0
                jsr OSBYTE
                ldx #0                      \ the name, as the helper prints it
.instnm         lda romtitle,x
                beq instnl
                jsr OSWRCH
                inx
                bne instnm
.instnl         jsr OSNEWL
.instx          rts

\ The two vectors into the stub, each unless it is there already: the vector it replaces
\ is kept in the stub first (nothing runs it until the vector moves), and a vector that
\ is the stub's is never kept, or the stub would call itself.  C set (nothing done) if
\ the interrupt vector is in FRED or JIM but not this stub: another program's hook.
.hookin         lda IRQ1V+1
                and #&FE
                cmp #&FC
                bne hook0
                lda IRQ1V+1
                cmp #>STUB
                bne hookx
                lda IRQ1V
                cmp #<(STUB+&0C)
                bne hookx
.hook0          php
                sei                         \ both vectors are used in interrupts
                lda INSV+1
                cmp #>STUB
                bne hook1
                lda INSV
                cmp #<STUB
                beq hook2
.hook1          lda INSV
                sta STUB+&0A
                lda INSV+1
                sta STUB+&0B
                lda #<STUB
                sta INSV
                lda #>STUB
                sta INSV+1
.hook2          lda IRQ1V+1
                cmp #>STUB
                bne hook3
                lda IRQ1V
                cmp #<(STUB+&0C)
                beq hook4
.hook3          lda IRQ1V
                sta STUB+&0D
                sta STUB+&2B
                lda IRQ1V+1
                sta STUB+&0E
                sta STUB+&2C
                lda #<(STUB+&0C)
                sta IRQ1V
                lda #>STUB
                sta IRQ1V+1
.hook4          plp
                clc
                rts
.hookx          sec
                rts

\ Hooked in again if something has put the vectors back behind the Pi's back - the second
\ processor's start, a Master's MOS - while the Pi still has the stub in telnet mode (its
\ LDX # at +15 says the keyboard buffer, 0).  Called on a read of the keyboard, which is
\ in the foreground on the host whichever processor runs the program.  No report: the
\ Pi's state has not changed, only the Beeb's vectors.  A, X and Y are kept.
.rehook         pha
                txa
                pha
                tya
                pha
                jsr stubok
                bne rehx
                lda STUB+&15
                cmp #&A2                    \ LDX #
                bne rehx
                lda STUB+&16
                bne rehx                    \ not telnet mode (unhooked on purpose, or the modem)
                lda INSV+1
                cmp #>STUB
                bne reh1
                lda INSV
                cmp #<STUB
                bne reh1
                lda IRQ1V+1
                cmp #>STUB
                bne reh1
                lda IRQ1V
                cmp #<(STUB+&0C)
                beq rehx
.reh1           jsr hookin
                bcs rehx
                lda #236                    \ *FX3: what is printed still goes to RS423 (bit 0)?
                ldx #0
                ldy #&FF
                jsr OSBYTE
                txa
                ora #1
                tax
                lda #3
                ldy #0
                jsr OSBYTE
.rehx           pla
                tay
                pla
                tax
                pla
                rts

\ Tell the Pi a program has taken a key it follows (see remvh).  A, X and Y are kept.
.keyother       pha                         \ &25: any other key
                lda #&25
                bne ktgo
.keytold        pha                         \ &24: a cursor key, COPY or RETURN
                lda #&24
.ktgo           pha                         \ the code
                txa
                pha
                tya
                pha                         \ &101,x Y  &102,x X  &103,x the code  &104,x A
                lda INSV+1
                cmp #STUBPAGE
                bne ktx
                ldy INSV
                tsx
                lda &103,x
                sta &FC28,y
.ktx            pla
                tay
                pla
                tax
                pla
                pla
                rts

\ RDCHV (build 13): a byte above 127 cannot be typed - the MOS's read turns &80-&FF into a soft
\ key or, with a low nibble &B-&F, a cursor key (OS 1.20 &E554) - so the Pi sends such a character
\ as LITKEY and the byte, and this, ahead of the MOS's routine, reads the byte after LITKEY as it
\ is.  OSWORD 0 (BASIC's line input) and GET read through RDCHV (OS 1.20 &E924).  Anything else
\ goes to the MOS's routine untouched.  Claimed only while RDCHV is the MOS's own.
\ In: A = this ROM's slot, kept.  X and Y are not.
.claimrdchv     pha
                lda ZPTMP
                pha
                lda ZPTMP+1
                pha
                lda DEFVEC
                sta ZPTMP
                lda DEFVEC+1
                sta ZPTMP+1
                ldy #&10
                lda (ZPTMP),y
                cmp RDCHV
                bne crno
                iny
                lda (ZPTMP),y
                cmp RDCHV+1
                bne crno
                lda RDCHV
                sta RDCHVOLD
                lda RDCHV+1
                sta RDCHVOLD+1
                lda #<rdchvh
                sta EXTVEC+24
                lda #>rdchvh
                sta EXTVEC+25
                tsx
                lda &103,x
                sta EXTVEC+26
                php
                sei
                lda #<RDCHV_ENTRY
                sta RDCHV
                lda #>RDCHV_ENTRY
                sta RDCHV+1
                plp
.crno           pla
                sta ZPTMP+1
                pla
                sta ZPTMP
                pla
                rts

.rdchvh         php
                pha
                txa
                pha
                tya
                pha                         \ &101,x Y  &102,x X  &103,x A  &104,x P
                ldx #0
                bit rdvset                  \ V set: look at the next keyboard byte
                jsr rdremv
                bcs rdold                   \ none
                cmp #LITKEY                 \ a look gives the byte in A (OS 1.20 &E472, &E491)
                bne rdold
                ldx #0
                clv
                jsr rdremv                  \ take LITKEY ...
.rdwait         ldx #0
                clv
                jsr rdremv                  \ ... and the byte after it (the Pi sends both)
                bcc rdgot
                bit &FF
                bpl rdwait                  \ not there yet; ESCAPE: let the MOS's routine say so
.rdold          pla
                tay
                pla
                tax
                pla
                plp
                jmp (RDCHVOLD)
.rdgot          tya
                tsx
                sta &103,x                  \ the byte is what comes back in A
                pla
                tay
                pla
                tax
                pla
                plp
                clc
                rts
.rdremv         jmp (REMV)
.rdvset         equb &40

\ Tell the Pi this was a reset: its reset line is not touched by a software BREAK.
\ The stub's control port is at +28, and INSV points at the stub once it is hooked in.
.marker         pha
                lda INSV+1
                cmp #&FC
                bne mkx
                ldy INSV
                pla
                sta &FC28,y
                rts
.mkx            pla
                rts

\ Claim REMV, as a ROM does: the vector points at the MOS's extended vector entry for
\ it, and the extended vector table says where in which ROM the routine is.  The MOS's
\ own routine is kept in the ROM's workspace (REMVOLD, in the Pi's FRED page).
\ In: A = this ROM's slot.  Out: A = &26 if claimed, &20 if not (the vector is
\ somebody's extended vector already, or the MOS has no extended vectors).
.claimremv      ldx REMV+1
                cpx #&FF
                beq notours
                pha
                lda REMV
                sta REMVOLD
                lda REMV+1
                sta REMVOLD+1
                lda #<remvh
                sta EXTVEC+66
                lda #>remvh
                sta EXTVEC+67
                pla
                sta EXTVEC+68
                lda #<REMV_ENTRY
                sta REMV
                lda #>REMV_ENTRY
                sta REMV+1
                lda #&26                    \ &26: the screen can be asked for, every key taken is told, and
                                            \ LITKEY bytes are read as they are (build 13)
                rts
.notours        lda #&20
                rts

\ REMV: X = the buffer, V clear to remove a byte, set to look at it; on return Y is the byte
\ and C set if there is none.  The keyboard buffer (X=0) is read by whatever is waiting
\ for a key, so this is called over and over while the Beeb is idle at a prompt, outside any
\ interrupt.  A byte of SCANKEY is the Pi asking for the screen: it is not given to the
\ program, the screen is read and sent (nothing shown), and the buffer is reported empty.
\ Every byte a program takes out is told to the Pi: a cursor key, COPY or RETURN with &24 (the
\ MOS's line input takes them as they are typed; a listing waiting in paged mode for SHIFT takes
\ none), so the copy cursor the Pi draws moves only when the Beeb's does; any other with &25, so
\ the Pi knows the keys waiting, sees a Beeb that has stopped taking them, and can read the
\ screen after a key that printed nothing (a program that writes the screen directly).
.remvh          php
                cpx #0
                bne remvold
                bvs remvold
                jsr rfsdone
                jsr rehook
                plp
                jsr remvcall
                bcs remvret
                cpy #SCANKEY
                beq remvscan
                cpy #13
                beq remvtell
                cpy #&8B                    \ COPY and the cursor keys, as the buffer holds them: &8B-&8F
                bcc remvother
                cpy #&90
                bcs remvother
.remvtell       jsr keytold
.remvok         clc
.remvret        rts
.remvother      jsr keyother
                clc
                rts
.remvscan       jsr keyother                \ counted as taken too
                pha
                txa
                pha
                jsr scan
                pla
                tax
                pla
                ldy #0
                sec
                rts
.remvold        plp
.remvcall       jmp (REMVOLD)

\ The ROM filing system takes the 6850 for each operation (&EA = 1, OS 1.2 &FB3D) and
\ leaves it taken, so nothing reaches RS423 again until another filing system is
\ selected.  A read of the keyboard is never inside an operation (each one takes it
\ again), so here it is given back, when the ROM filing system had it (&247 = 2).
\ The cassette's (&247 = 0) is left alone: a tape may still be running.  A is kept.
CFSFLAG = &0247             \ 0 the cassette filing system, 2 the ROM one
ACIAUSE = &EA               \ 1..&7F: the cassette or ROM filing system has the 6850
.rfsdone        pha
                lda CFSFLAG
                beq rfsx
                lda ACIAUSE
                beq rfsx
                bmi rfsx                    \ negative: the RS423 timeout count, the MOS's
                lda #0
                sta ACIAUSE
.rfsx           pla
                rts

\ SHIFT for paged mode.  With paged mode on (VDU 14) the MOS waits at the end of each page
\ until SHIFT is pressed (OS 1.20 &CB0E), and it asks the keyboard vector, KEYV with C and V
\ clear, which answers in A: bit 7 CTRL, bit 6 SHIFT (OS 1.20 &EEEB).  SHIFT is a key of the
\ matrix, not a byte in the buffer, so nothing typed ahead (a COPY pressed during the wait)
\ may hold it up.  A telnet client cannot send it on its own: for its Page Down key the Pi
\ sets SHIFTFLAG in the ROM's workspace (src/serial_redirect.c), and this ROM, which
\ claims KEYV, answers that question SHIFT pressed once when it finds the flag set, and clears
\ it.  Every other KEYV call (the keyboard interrupt and scan) goes straight on to the MOS's
\ routine, whose address is in the MOS's own table of default vectors, so KEYV is claimed only
\ while it is the MOS's.  (Build 9 put a byte, &FC, in the keyboard buffer instead, and a COPY
\ in front of it kept it from being seen.)
\ In: A = this ROM's slot, kept.  X and Y are not.
.claimkeyv      pha
                lda ZPTMP
                pha
                lda ZPTMP+1
                pha
                lda DEFVEC
                sta ZPTMP
                lda DEFVEC+1
                sta ZPTMP+1
                ldy #&28
                lda (ZPTMP),y
                cmp KEYV
                bne ckno
                iny
                lda (ZPTMP),y
                cmp KEYV+1
                bne ckno                    \ somebody's already, or an extended vector
                lda KEYV                    \ the MOS's routine, for keymos
                sta KEYVOLD
                lda KEYV+1
                sta KEYVOLD+1
                lda #<keyvh
                sta EXTVEC+60
                lda #>keyvh
                sta EXTVEC+61
                tsx
                lda &103,x
                sta EXTVEC+62
                php
                sei                         \ the keyboard interrupt goes through KEYV
                lda #<KEYV_ENTRY
                sta KEYV
                lda #>KEYV_ENTRY
                sta KEYV+1
                lda #0
                sta SHIFTFLAG               \ no SHIFT left over from before the reset
                plp
.ckno           pla
                sta ZPTMP+1
                pla
                sta ZPTMP
                pla
                rts

.keyvh          bvs keymos                  \ the interrupt and the scans: the MOS's, untouched
                bcs keymos
                jsr keymos                  \ C and V clear: is SHIFT or CTRL pressed?  A = the answer
                pha
                lda SHIFTFLAG
                beq keyvno
                lda #0
                sta SHIFTFLAG               \ once
                pla
                ora #&40                    \ SHIFT
                rts
.keyvno         pla
                rts

\ Go on to the MOS's KEYV routine with A, X, Y and the flags as they came.  This runs on
\ every keyboard interrupt too, so it touches nothing but the address kept at the claim: no
\ zero page, which an NMI (Econet) could find changed.
.keymos         jmp (KEYVOLD)



.coltab         equb 80, 40, 20, 80, 40, 20, 40, 40     \ text columns in MODE 0 to 7
.rowtab         equb 32, 32, 32, 25, 32, 32, 25, 25     \ ... and rows

\ Send the screen to the Pi.  The Pi gets the characters only, as VDU 31,x,y,char with
\ the VDU drivers off (OSBYTE 3, X=3); the Beeb's cursor is moved on the screen to read
\ each cell, and put back.  A text window would keep the cursor inside it (VDU 31 is
\ relative to it and ignores a cell outside it), so for the scan the window is opened
\ to the whole screen in the MOS's VDU variables (&300+8 to &300+11, which OSBYTE 160
\ reads), and put back afterwards; the Pi is sent VDU 26 before and VDU 28 with the
\ window after, so its own picture has the same window.  Nothing is kept in zero page,
\ which the MOS's OSBYTE and VDU routines use: the counters live on the stack, below
\ the return address, from the top:
\   &101,x column  &102,x row  &103,x the output streams as they were
\   &104,x cursor column  &105,x cursor row  &106,x columns  &107,x rows
\   &108,x window top  &109,x right  &10A,x bottom  &10B,x left
VDUVARS = &0300
.scan           lda INSV+1                  \ the Pi is hooked in when the keyboard vector is its stub
                cmp #STUBPAGE
                beq scango
                rts
.scango         ldx #8                      \ the text window: left, bottom, right, top
                jsr rdvdu
                pha
                ldx #9
                jsr rdvdu
                pha
                ldx #10
                jsr rdvdu
                pha
                ldx #11
                jsr rdvdu
                pha
                lda #135                    \ Y = the mode
                jsr OSBYTE
                tya
                and #7
                tax
                lda rowtab,x
                pha                         \ rows
                lda coltab,x
                pha                         \ columns
                lda #134                    \ the cursor (in the window), to put back
                jsr OSBYTE
                tya
                pha                         \ cursor row
                txa
                pha                         \ cursor column
                tsx                         \ the window opened to the whole screen
                lda &103,x
                sec
                sbc #1
                sta VDUVARS+10              \ right = columns - 1
                lda &104,x
                sec
                sbc #1
                sta VDUVARS+9               \ bottom = rows - 1
                lda #0
                sta VDUVARS+8
                sta VDUVARS+11
                lda #3                      \ the screen only, and what it was
                ldx #0
                jsr OSBYTE
                txa
                pha                         \ the streams as they were
                lda #0
                pha                         \ row
                pha                         \ column
                jsr topi
                lda #26                     \ the Pi: no window, and a clear screen
                jsr OSWRCH
                lda #12
                jsr OSWRCH
                jsr toscr
.scancol        tsx
                lda &101,x
                ldy &102,x
                jsr locate                  \ the cursor to the cell, on the screen
                lda #READ_TRIES             \ tries left, on the stack
                pha
.scanread       lda #135
                jsr OSBYTE                  \ X = the character at the cursor
                pla
                sec
                sbc #1
                pha
                beq scanread1               \ no tries left
                txa
                beq scanread                \ 0, not recognised: with a second processor on, a cell
.scanread1      pla                         \ now and then reads as nothing; it reads right again
                txa
                beq scanskip                \ 0: not a character the MOS knows
                cmp #32
                beq scanskip
                bcs scanchr
                ora #&80                    \ MODE 7: a control code stored as &00-&1F (WordWise-Plus's
.scanchr        pha                         \ colours) is sent as &80-&9F, the same to the chip, not VDU codes
                jsr topi
                tsx
                lda &102,x                  \ column and row are one deeper with the character pushed
                ldy &103,x
                jsr locate
                pla
                jsr OSWRCH
                jsr toscr
.scanskip       tsx
                inc &101,x
                lda &101,x
                cmp &106,x
                bne scancol
                lda #0
                sta &101,x
                inc &102,x
                lda &102,x
                cmp &107,x
                bne scancol
                tsx                         \ the window back on the Beeb ...
                lda &10B,x
                sta VDUVARS+8
                lda &10A,x
                sta VDUVARS+9
                lda &109,x
                sta VDUVARS+10
                lda &108,x
                sta VDUVARS+11
                lda &104,x                  \ ... and its cursor
                ldy &105,x
                jsr locate
                jsr topi                    \ the Pi: the same window and cursor
                lda #28
                jsr OSWRCH
                tsx
                lda &10B,x
                jsr OSWRCH
                lda &10A,x
                jsr OSWRCH
                lda &109,x
                jsr OSWRCH
                lda &108,x
                jsr OSWRCH
                lda &104,x
                ldy &105,x
                jsr locate
                tsx
                lda &103,x
                tax
                lda #3
                jsr OSBYTE                  \ the streams as they were
                ldx #11
.scandrop       pla
                dex
                bne scandrop
                rts

.rdvdu          lda #160                    \ OSBYTE 160: A = VDU variable X
                jsr OSBYTE
                txa
                rts

.locate         pha                         \ VDU 31,A,Y (OSWRCH keeps Y)
                lda #31
                jsr OSWRCH
                pla
                jsr OSWRCH
                tya
                jmp OSWRCH
.topi           lda #3                      \ output to RS423 only
                ldx #3
                jmp OSBYTE
.toscr          lda #3                      \ output to the screen only
                ldx #0
                jmp OSBYTE

\ *HELP with nothing after it: the title, version and build, so it is easy to see which
\ image is fitted.  *HELP with a topic says nothing.  Not claimed either way: every ROM
\ answers *HELP, and A, X and Y go back as they came.
.help           pha
                txa
                pha
                tya
                pha
                lda (CMDTAIL),y
                cmp #13
                bne helpx
                jsr OSNEWL
                ldx #0
.helplp         lda romtitle,x
                bne helpch
                lda #' '                    \ the 0 between the title and the version
.helpch         jsr OSWRCH
                inx
                cpx #(copyright - romtitle)
                bne helplp
                jsr OSNEWL
.helpx          pla
                tay
                pla
                tax
                pla
                rts

.romend

SAVE "telnetd.rom", &8000, &C000
