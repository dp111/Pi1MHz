\ 1mhzwifi.asm
\ 1MHz-WiFi sideways ROM: image header, service entry and command dispatch.
\
\ Written for the 1MHz-WiFi project. This replaces the main file the ROM
\ inherited from ElkWiFi 0.23, whose own command table search was in turn
\ credited to Gerrit Hillebrand's ATOM GDOS 1.5. Neither is used here: the
\ search below walks one table entry at a time with its own structure, and the
\ service, help, banner and OSWORD paths are this project's.
\
\ The table format itself is kept, because it is what the entries are written
\ in: an entry is the command name in ASCII, then the handler address high byte
\ first. A name byte with bit 7 set cannot occur, so the high byte of an
\ address doubles as the end of name marker, and a bare address with no name
\ terminates the table because every handler lives above &8000.
\
\ Credit for the parts of this ROM that do still derive from ElkWiFi is carried
\ in *VERSION, not here.

include "machine.asm"

\ ---------------------------------------------------------------------------
\ Image headers
\ ---------------------------------------------------------------------------
\ The ATM header is a sixteen byte zero padded name followed by load address,
\ execution address and length. It precedes the ROM so the image can be loaded
\ by a development loader; the shipped image starts at romstart.

.atmheader          equs "1mhzwifi.rom",0,0,0,0
                    equw &1800
                    equw &1800
                    equw romend-romstart

.romstart           equb 0                      \ no language entry
                    equb 0
                    equb 0
                    jmp service
                    equb &82                    \ service ROM, no language
                    equb (copyright-romstart)   \ copyright offset
                    equb &30                    \ binary version
.romtitle           equs "1MHz-WiFi"
                    equb 0
.romversion         equs "0.1.67"
.copyright          equb 0
                    equs "(C)2026 Peter Clarke"
                    equb 0

\ The keyword *HELP matches on, stored reversed because the compare below walks
\ it backwards from its last character.
.commands           equs "IFIW"

\ ---------------------------------------------------------------------------
\ Service entry
\ ---------------------------------------------------------------------------
\ A is the reason code, X this ROM's slot, Y the reason argument. Anything not
\ listed is passed on with A unchanged.

.service            cmp #4                      \ unrecognised command
                    beq command
                    cmp #9                      \ *HELP
                    beq help
                    cmp #1                      \ post reset initialisation
                    bne service_not_boot
                    jmp autorun
.service_not_boot   cmp #8                      \ unrecognised OSWORD
                    bne service_not_osword
                    jmp osword65
.service_not_osword rts

\ ---------------------------------------------------------------------------
\ Command dispatch
\ ---------------------------------------------------------------------------
\ Match the command line against the table, honouring a trailing dot as an
\ abbreviation. Every BNE after an INX below stands in for a JMP: X indexes the
\ table and only returns to zero if the table passes 256 bytes, which would
\ break the single byte indexing regardless.
\
\ X indexes the table and is left on an entry's address high byte
\ when a match is found; Y indexes the command line and is left just past the
\ matched text, which is where every handler expects to start reading its
\ arguments.

.command            tya                         \ A on exit belongs to the
                    pha                         \ handler, so only X and Y are
                    txa                         \ saved here
                    pha
                    cld
                    ldx #0
.cmd_entry          ldy #0
                    jsr skipspace               \ Y indexes the first non-space
                    dey                         \ the loop's iny puts it back
.cmd_step           iny
                    lda commandtable,x
                    bmi cmd_dispatch            \ name ended: full match
                    cmp (line),y
                    bne cmd_mismatch
                    inx
                    bne cmd_step

\ The character on the command line is not the one the table expects. A dot
\ there means the user abbreviated this command, so accept the entry; anything
\ else means this is a different command.
.cmd_mismatch       lda (line),y
                    cmp #'.'
                    beq cmd_abbreviated
.cmd_skip_name      lda commandtable,x
                    bmi cmd_skip_address
                    inx
                    bne cmd_skip_name
.cmd_skip_address   inx
                    inx                         \ X now starts the next entry
                    bne cmd_entry

.cmd_abbreviated    iny                         \ step over the dot
.cmd_abbrev_skip    lda commandtable,x
                    bmi cmd_dispatch
                    inx
                    bne cmd_abbrev_skip

.cmd_dispatch       sta zp+1                    \ A already holds the high byte
                    lda commandtable+1,x
                    sta zp
                    jmp (zp)

\ Reached through the table's terminating entry: no command matched.
.command_x6         pla
                    tax
                    pla
                    tay
                    lda #4
                    rts

\ ---------------------------------------------------------------------------
\ *HELP
\ ---------------------------------------------------------------------------
\ With no keyword, print the title and the keyword this ROM answers to, then
\ pass the call on so other ROMs also report. With the keyword, print the
\ command list and claim the call.

.help               tya
                    pha
                    txa
                    pha
                    lda (line),y
                    cmp #&D
                    beq help_l2
                    ldx #3                      \ compare backwards against
.help_l1            lda (line),y                \ the reversed keyword
                    cmp commands,x
                    bne help_l3
                    iny
                    dex
                    bpl help_l1
                    jsr print_help
                    jmp call_claimed
.help_l2            jsr help_version
                    jsr printtext
                    equb &D,&20,&20
                    equs "WIFI",&D,&EA
.help_l3            pla
                    tax
                    pla
                    tay
                    lda #9
                    rts

\ ---------------------------------------------------------------------------
\ Reset
\ ---------------------------------------------------------------------------
\ Answered on reason 1 rather than 3, because a higher priority ROM may claim
\ 3 before this one sees it. The MOS banner is suppressed and replaced, so the
\ machine reports one identity rather than two.

.autorun            tya
                    pha
                    txa
                    pha


                    \ Nothing here may touch the AP5 JIM selector: the ROM scan
                    \ runs with another ROM's page possibly selected, and every
                    \ command selects its own page when it starts.
                    lda #&D7                    \ suppress the default banner
                    ldx #0
                    stx mux_status              \ no connection multiplexing yet
                    ldy #&7F
                    jsr osbyte

                    jsr printtext
                    equs "1MHz-WiFi 0.1.67",&EA

                    ldy #&FF                    \ OSBYTE &FD: last reset type
                    ldx #&00
                    lda #&FD
                    jsr osbyte
                    cpx #0                      \ soft reset stays quiet
                    beq autorun_l1
                    lda #7
                    jsr oswrch
                    cpx #1
                    bne autorun_l1
.autorun_l1         jsr print_logo
                    jsr printtext
                    equb &D,&EA
                    pla
                    tax
                    pla
                    tay
                    lda #1
                    rts

\ ---------------------------------------------------------------------------
\ Command table
\ ---------------------------------------------------------------------------
\ Longer names must precede any name they begin with, so LAPOPT is listed
\ before LAP and QUPRUN before QR.

.commandtable       equs "WGET"
                    equb >pi_wget_cmd, <pi_wget_cmd
                    equs "WIFI"
                    equb >wifi_cmd, <wifi_cmd
                    equs "VERSION"
                    equb >version_cmd, <version_cmd
                    equs "LAPOPT"
                    equb >lapopt_cmd, <lapopt_cmd
                    equs "LAP"
                    equb >lap_cmd, <lap_cmd
                    equs "IFCFG"
                    equb >ifcfg_cmd, <ifcfg_cmd
                    equs "DATE"
                    equb >date_cmd, <date_cmd
                    equs "TIME"
                    equb >time_cmd, <time_cmd
                    equs "PRD"
                    equb >pdump_cmd, <pdump_cmd
                    equs "ONLINE"
                    equb >online_cmd, <online_cmd
                    equs "JOIN"
                    equb >join_cmd, <join_cmd
                    equs "LEAVE"
                    equb >leave_cmd, <leave_cmd
                    equs "PING"
                    equb >ping_cmd, <ping_cmd
                    equs "NSLOOK"
                    equb >nslook_cmd, <nslook_cmd
                    equs "RDINIT"
                    equb >rd_init_cmd, <rd_init_cmd
                    equs "RDCAT"
                    equb >rd_cat_cmd, <rd_cat_cmd
                    equs "RDLOAD"
                    equb >rd_load_cmd, <rd_load_cmd
                    equs "RDSAVE"
                    equb >rd_save_cmd, <rd_save_cmd
                    equs "RDRUN"
                    equb >rd_run_cmd, <rd_run_cmd
                    equs "MODE"
                    equb >mode_cmd, <mode_cmd
                    equs "DISCONNECT"
                    equb >disconnect_cmd, <disconnect_cmd
                    equb >command_x6, <command_x6

\ Print the ROM title and version, with the separating zero shown as a space.
.help_version       ldx #0
.help_vl1           lda romtitle,x
                    bne help_vl2
                    lda #&20
.help_vl2           jsr osasci
                    inx
                    cpx #(copyright-romtitle)
                    bne help_vl1
                    rts

.print_help         jsr help_version
                    jsr printtext
                    equb &0D
                    \ 40 "----- This string is 40 characters -----"
                    equs " DATE      Print current date",&0D
                    equs " IFCFG     Print IP and MAC address",&0D
                    equs " JOIN      Join a network",&0D
                    equs " LAP       List access points",&0D
                    equs " LAPOPT    Set LAP options",&0D
                    equs " LEAVE     Disconnect from network",&0D
                    equs " MODE      Set device mode",&0D
                    equs " ONLINE    Show network readiness",&0D
                    equs " PING      ping a host on network",&0D
                    equs " NSLOOK    Resolve an IPv4 address",&0D
                    equs " PRD       Paged Ram Dump",&0D
                    equs " RDCAT     Catalogue the RAM disk",&0D
                    equs " RDINIT    Clear the RAM disk",&0D
                    equs " RDLOAD    Load from the RAM disk",&0D
                    equs " RDRUN     Run from the RAM disk",&0D
                    equs " RDSAVE    Save to the RAM disk",&0D
                    equs " TIME      Print current time",&0D
                    equs " VERSION   Print firmware version",&0D
                    equs " WGET      Get a file from a webserver",&0D
                    equs " WIFI      WiFi control ON|OFF|HR|SR",&0D
                    equb &EA
.print_help_end     rts

\ ---------------------------------------------------------------------------
\ OSWORD &65
\ ---------------------------------------------------------------------------
\ The public driver entry, so applications can call the WiFi functions the
\ commands use. This is the ElkWiFi compatibility interface and its shape is
\ fixed by the clients that already call it: &EF holds the OSWORD number and
\ &F0/&F1 point at a block whose first three bytes are the A, X and Y the
\ driver should see. Every register may be modified.

.osword65           lda &EF
                    cmp #&65
                    beq osword65_l1
                    lda #8                      \ not ours, pass it on
                    rts
.osword65_l1        tya
                    pha
                    txa
                    pha
                    ldy #0
                    lda (&F0),y                 \ function number
                    pha
                    iny
                    lda (&F0),y
                    tax
                    iny
                    lda (&F0),y
                    tay
                    pla
                    jsr wifidriver
                    jmp call_claimed

include "util.asm"
include "errors.asm"
include "serial.asm"
include "service_driver.asm"
include "net_transport.asm"   \ after service_driver.asm, which sizes its workspace
include "driver.asm"
include "version.asm"
include "time.asm"
include "lap.asm"
include "ifcfg.asm"
include "online.asm"
include "wificmd.asm"
include "pdump.asm"
include "join.asm"
include "mode.asm"
include "wget.asm"
include "net_wget.asm"
include "ping.asm"
include "nslook.asm"
include "ramdisk.asm"

rom_content_end = P%
ASSERT rom_content_end <= &BF00

skipto &C000
.romend

SAVE "1mhz-wifi-atm.rom", atmheader, romend
SAVE "1mhz-wifi.rom", romstart, romend
