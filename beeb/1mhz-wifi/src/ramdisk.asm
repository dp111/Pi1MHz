\ ramdisk.asm
\ 1MHz-WiFi ROM: a RAM disk in the Pi1MHz JIM window.
\
\ Written for the 1MHz-WiFi project. This is what replaces the cassette filing
\ system for getting a program into the machine: fetch it over WiFi, keep it in
\ the Pi's memory, and load or run it from there. No part of it derives from
\ anyone else's work.
\
\ The store is the low 64 KiB JIM window, which is the one window every target
\ machine can reach. An unmodified Electron AP5 forwards only bank 0, so a
\ format that needed the upper banks would work on the BBC family and not on
\ the Electron; select_public_page_a hides that difference and everything here
\ stays inside bank 0 so all four machines behave identically.
\
\ Layout. Page 0 is the service reply buffer that OSWORD &65 clients read, so
\ it is left alone. Page 1 is the catalogue. Files follow from page 2, each
\ starting on a page boundary, which is what makes a file's position a single
\ byte and the whole of the arithmetic below a page counter.
\
\   catalogue page   +0,1   signature
\                    +2     number of entries
\                    +3     next free page
\                    +16    fifteen entries of sixteen bytes
\
\   entry            +0..6  name, padded with spaces
\                    +7,8   load address
\                    +9,10  execution address
\                    +11,12 length
\                    +13    first page
\                    +14,15 spare
\
\ Sixteen bytes an entry means the offset of entry N is four shifts and an add,
\ which is why the entry is that size rather than the eleven bytes it needs.

rd_cat_page      = 1            \ catalogue page within the window
rd_first_page    = 2            \ files start here
rd_max_entries   = 15           \ what fits in one page after the header
rd_entry_size    = 16
rd_name_size     = 7
rd_sig_0         = 'R'
rd_sig_1         = 'D'

\ Command workspace. heap+&C0 to &DF is not claimed by any other command, and
\ no two of these commands run at once.
rd_page          = heap+&C0     \ page being read or written
rd_offset        = heap+&C1     \ offset within that page
\ The host memory pointer must be in zero page for (rd_addr),Y. load_addr is
\ the ROM's pointer pair for exactly this and no other command is running.
rd_addr          = load_addr
rd_entry         = heap+&C6     \ catalogue offset of the entry in hand
rd_count         = heap+&C7     \ entries in the catalogue
rd_next          = heap+&C8     \ next free page
rd_name          = heap+&C9     \ the name being matched, seven bytes

\ Load, execution and length are read and written as one six byte run, both
\ here and in the catalogue entry, so they must stay adjacent and in this
\ order. Putting the length elsewhere silently records a zero.
rd_load          = heap+&D0     \ two bytes
rd_exec          = heap+&D2     \ two bytes
rd_len           = heap+&D4     \ two bytes, counted down by the copy

rd_start         = heap+&D6     \ first page of the file in hand
rd_index         = heap+&D7     \ entry number while walking the catalogue
rd_temp          = heap+&D8
rd_hold          = heap+&D9     \ byte held across a Y restore

\ ===========================================================================
\ Window access
\ ===========================================================================
\ Read the byte at rd_page,rd_offset. Interrupts are held off across the pair
\ because the page register and the aperture are shared with other JIM users.
\ Returns the byte in A; Y is not preserved.

\ Y is preserved because Y is the MOS command line pointer, and every command
\ here reads the catalogue before it has finished parsing its arguments.

.rd_read            php
                    sei
                    tya
                    pha
                    lda rd_page
                    jsr select_public_page_a
                    ldy rd_offset
                    lda pageram,y
                    sta rd_hold
                    pla
                    tay
                    lda rd_hold
                    plp
                    rts

\ Write A at rd_page,rd_offset.

.rd_write           php
                    sei
                    sta rd_temp
                    tya
                    pha
                    lda rd_page
                    jsr select_public_page_a
                    ldy rd_offset
                    lda rd_temp
                    sta pageram,y
                    jsr bus_delay
                    pla
                    tay
                    plp
                    rts

\ Step the cursor on one byte, carrying into the page.

.rd_step            inc rd_offset
                    bne rd_step_done
                    inc rd_page
.rd_step_done       rts

\ Point the cursor at the catalogue byte in A.

.rd_cat_at          sta rd_offset
                    lda #rd_cat_page
                    sta rd_page
                    rts

\ ===========================================================================
\ Catalogue
\ ===========================================================================
\ Read the header. Carry clear and rd_count/rd_next loaded if the signature is
\ present; carry set if the window holds no RAM disk.

.rd_open            lda #0
                    jsr rd_cat_at
                    jsr rd_read
                    cmp #rd_sig_0
                    bne rd_open_bad
                    lda #1
                    jsr rd_cat_at
                    jsr rd_read
                    cmp #rd_sig_1
                    bne rd_open_bad
                    lda #2
                    jsr rd_cat_at
                    jsr rd_read
                    sta rd_count
                    lda #3
                    jsr rd_cat_at
                    jsr rd_read
                    sta rd_next
                    clc
                    rts
.rd_open_bad        sec
                    rts

\ Write the header back from rd_count and rd_next.

.rd_commit          lda #2
                    jsr rd_cat_at
                    lda rd_count
                    jsr rd_write
                    lda #3
                    jsr rd_cat_at
                    lda rd_next
                    jmp rd_write

\ Set rd_entry to the catalogue offset of entry rd_index.
\ offset = 16 + index * 16, which cannot exceed 255 for fifteen entries.

.rd_entry_offset    lda rd_index
                    asl a
                    asl a
                    asl a
                    asl a
                    clc
                    adc #rd_entry_size
                    sta rd_entry
                    rts

\ Load the entry in hand into rd_load, rd_exec, rd_len and rd_start.

.rd_entry_fields    jsr rd_entry_offset
                    ldx #0
.rd_field_loop      lda rd_entry
                    clc
                    adc #rd_name_size
                    sta rd_temp
                    txa
                    clc
                    adc rd_temp
                    jsr rd_cat_at
                    jsr rd_read
                    sta rd_load,x           \ load, exec and length are stored
                    inx                     \ in that order and read as a run
                    cpx #6
                    bne rd_field_loop
                    lda rd_entry
                    clc
                    adc #13
                    jsr rd_cat_at
                    jsr rd_read
                    sta rd_start
                    rts

\ ===========================================================================
\ Name handling
\ ===========================================================================
\ Copy the command line parameter in strbuf into rd_name, padded with spaces
\ and truncated to the field width, so a comparison is a fixed seven bytes.

.rd_set_name        ldx #0
.rd_name_copy       lda strbuf,x
                    cmp #&0D
                    beq rd_name_pad
                    jsr rd_upper
                    sta rd_name,x
                    inx
                    cpx #rd_name_size
                    bne rd_name_copy
                    rts
.rd_name_pad        lda #' '
.rd_name_pad_loop   sta rd_name,x
                    inx
                    cpx #rd_name_size
                    bne rd_name_pad_loop
                    rts

\ Fold a to z to upper case so names match however they were typed.

.rd_upper           cmp #'a'
                    bcc rd_upper_done
                    cmp #'z'+1
                    bcs rd_upper_done
                    and #&DF
.rd_upper_done      rts

\ Find rd_name in the catalogue. Carry clear with rd_index set and the entry
\ fields loaded if it is there, carry set if it is not.

.rd_find            lda #0
                    sta rd_index
.rd_find_entry      lda rd_index
                    cmp rd_count
                    bcs rd_find_missing
                    jsr rd_entry_offset
                    ldx #0
.rd_find_char       txa
                    clc
                    adc rd_entry
                    jsr rd_cat_at
                    jsr rd_read
                    cmp rd_name,x
                    bne rd_find_next
                    inx
                    cpx #rd_name_size
                    bne rd_find_char
                    jsr rd_entry_fields
                    clc
                    rts
.rd_find_next       inc rd_index
                    bne rd_find_entry       \ always: rd_count is at most 15
.rd_find_missing    sec
                    rts

\ ===========================================================================
\ *RDINIT
\ ===========================================================================
\ Write an empty catalogue. This discards whatever the window held, which is
\ the only way to reclaim space, since entries are never removed individually.

.rd_init_cmd        jsr detect_jim_machine
                    lda #0
                    jsr rd_cat_at
                    lda #rd_sig_0
                    jsr rd_write
                    lda #1
                    jsr rd_cat_at
                    lda #rd_sig_1
                    jsr rd_write
                    lda #0
                    sta rd_count
                    lda #rd_first_page
                    sta rd_next
                    jsr rd_commit
                    jsr printtext
                    equs "RAM disk cleared",&0D,&EA
                    jmp call_claimed

\ ===========================================================================
\ *RDCAT
\ ===========================================================================

.rd_cat_cmd         jsr detect_jim_machine
                    jsr rd_open
                    bcc rd_cat_have_disk
                    jmp rd_no_disk
.rd_cat_have_disk
                    jsr printtext
                    equs "RAM disk",&0D,&EA
                    lda #0
                    sta rd_index
.rd_cat_next        lda rd_index
                    cmp rd_count
                    bcs rd_cat_free
                    jsr rd_entry_fields
                    ldx #0
.rd_cat_name        txa
                    clc
                    adc rd_entry
                    jsr rd_cat_at
                    jsr rd_read
                    jsr osasci
                    inx
                    cpx #rd_name_size
                    bne rd_cat_name
                    lda #' '
                    jsr osasci
                    lda rd_load+1           \ load address, high byte first
                    jsr printhex
                    lda rd_load
                    jsr printhex
                    lda #' '
                    jsr osasci
                    lda rd_exec+1
                    jsr printhex
                    lda rd_exec
                    jsr printhex
                    lda #' '
                    jsr osasci
                    lda rd_len+1
                    jsr printhex
                    lda rd_len
                    jsr printhex
                    jsr osnewl
                    inc rd_index
                    bne rd_cat_next
.rd_cat_free        jsr printtext
                    equs "Pages used ",&EA
                    lda rd_next
                    jsr printhex
                    jsr printtext
                    equs " of FF",&0D,&EA
                    jmp call_claimed

.rd_no_disk         jsr printtext
                    equs "No RAM disk; use *RDINIT",&0D,&EA
                    jmp call_claimed

.rd_not_found       jsr printtext
                    equs "Not found",&0D,&EA
                    jmp call_claimed

\ ===========================================================================
\ *RDLOAD <name> [address]
\ ===========================================================================
\ Copy a file out of the window into host memory, at its recorded load address
\ or at one given on the command line.

.rd_load_cmd        jsr detect_jim_machine
                    jsr rd_open
                    bcc rd_load_have_disk
                    jmp rd_no_disk
.rd_load_have_disk
                    jsr skipspace1
                    jsr read_cli_param
                    cpx #0
                    beq rd_usage_load
                    jsr rd_set_name
                    jsr rd_find
                    bcc rd_load_found
                    jmp rd_not_found
.rd_load_found

                    jsr skipspace1          \ an address overrides the entry's
                    jsr read_cli_param
                    cpx #0
                    beq rd_load_at_entry
                    jsr rd_hex_to_addr
                    bcc rd_load_go
.rd_load_at_entry   lda rd_load
                    sta rd_addr
                    lda rd_load+1
                    sta rd_addr+1
.rd_load_go         jsr rd_copy_out
                    jmp call_claimed

.rd_usage_load      jsr printtext
                    equs "Usage: RDLOAD <name> [addr]",&0D,&EA
                    jmp call_claimed

\ Convert the parameter in strbuf to rd_addr. Carry clear if it was a number.

.rd_hex_to_addr     ldx #<zp
                    jsr string2hex
                    cmp #0
                    beq rd_hex_none
                    lda zp
                    sta rd_addr
                    lda zp+1
                    sta rd_addr+1
                    clc
                    rts
.rd_hex_none        sec
                    rts

\ Copy rd_len bytes from page rd_start to rd_addr.
\
\ The page is selected once per page rather than once per byte. Selecting
\ costs two JIM selector writes and three bus settling delays on a BBC family
\ host, so doing it per byte would spend far longer settling the bus than
\ moving data; a sixteen kilobyte file would take seconds. Y indexes the
\ window and the destination together, which is why rd_addr is kept pointing
\ at the byte matching offset zero of the current page rather than at the next
\ byte to write.

.rd_copy_out        lda rd_start
                    sta rd_page
                    ldy #0
.rd_out_page        lda rd_len
                    ora rd_len+1
                    beq rd_copy_done
                    php
                    sei                     \ the selector is shared, so hold
                    lda rd_page             \ it across the page
                    jsr select_public_page_a
.rd_out_byte        lda pageram,y
                    sta (rd_addr),y
                    lda rd_len
                    bne rd_out_low
                    dec rd_len+1
.rd_out_low         dec rd_len
                    lda rd_len
                    ora rd_len+1
                    beq rd_out_page_done
                    iny
                    bne rd_out_byte
.rd_out_page_done   plp
                    tya
                    bne rd_copy_done        \ stopped mid page: nothing to step
                    inc rd_page
                    inc rd_addr+1
                    jmp rd_out_page
.rd_copy_done       rts

\ ===========================================================================
\ *RDSAVE <name> <start> <end> [exec]
\ ===========================================================================

.rd_save_cmd        jsr detect_jim_machine
                    jsr rd_open
                    bcc rd_save_have_disk
                    jmp rd_no_disk
.rd_save_have_disk
                    lda rd_count
                    cmp #rd_max_entries
                    bcc rd_save_room
                    jmp rd_full
.rd_save_room
                    jsr skipspace1
                    jsr read_cli_param
                    cpx #0
                    beq rd_save_usage_near
                    jsr rd_set_name

                    jsr skipspace1          \ start address
                    jsr read_cli_param
                    cpx #0
                    beq rd_save_usage_near
                    jsr rd_hex_to_addr
                    bcs rd_save_usage_near
                    lda rd_addr
                    sta rd_load
                    lda rd_addr+1
                    sta rd_load+1

                    jsr skipspace1          \ end address, exclusive
                    jsr read_cli_param
                    cpx #0
                    beq rd_save_usage_near
                    ldx #<zp
                    jsr string2hex
                    sec                     \ length = end - start
                    lda zp
                    sbc rd_load
                    sta rd_len
                    lda zp+1
                    sbc rd_load+1
                    sta rd_len+1

                    lda rd_load             \ execution address defaults to
                    sta rd_exec             \ the load address
                    lda rd_load+1
                    sta rd_exec+1
                    jsr skipspace1
                    jsr read_cli_param
                    cpx #0
                    beq rd_save_store
                    ldx #<zp
                    jsr string2hex
                    lda zp
                    sta rd_exec
                    lda zp+1
                    sta rd_exec+1
                    jmp rd_save_store       \ or the exec case falls into usage

.rd_save_usage_near jmp rd_usage_save

\ Record the entry before copying, because the copy counts rd_len down to zero
\ and the entry has to carry the real length. Writing it early is safe: an
\ entry is not visible until rd_count is raised, which only happens once the
\ copy has succeeded.
.rd_save_store      lda rd_next
                    sta rd_start
                    jsr rd_entry_write
                    jsr rd_copy_in
                    bcc rd_save_stored
                    jmp rd_full
.rd_save_stored     inc rd_count
                    jsr rd_commit
                    jmp call_claimed

.rd_usage_save      jsr printtext
                    equs "Usage: RDSAVE <name> <start> <end> [exec]",&0D,&EA
                    jmp call_claimed

.rd_full            jsr printtext
                    equs "RAM disk full",&0D,&EA
                    jmp call_claimed

\ Copy rd_len bytes from rd_load into the window at rd_start, advancing
\ rd_next past them. Carry set if the window would overflow. The page is
\ selected once per page, as above.

.rd_copy_in         lda rd_load
                    sta rd_addr
                    lda rd_load+1
                    sta rd_addr+1
                    lda rd_start
                    sta rd_page
                    ldy #0
.rd_in_page         lda rd_len
                    ora rd_len+1
                    beq rd_in_done
                    lda rd_page
                    beq rd_in_overflow      \ wrapped past the top of the window
                    php
                    sei
                    lda rd_page
                    jsr select_public_page_a
.rd_in_byte         lda (rd_addr),y
                    sta pageram,y
                    lda rd_len
                    bne rd_in_low
                    dec rd_len+1
.rd_in_low          dec rd_len
                    lda rd_len
                    ora rd_len+1
                    beq rd_in_page_done
                    iny
                    bne rd_in_byte
.rd_in_page_done    jsr bus_delay           \ settle the last write of the page
                    plp
                    tya
                    beq rd_in_whole_page
                    inc rd_page             \ part of a page used; round up
                    jmp rd_in_done
.rd_in_whole_page   inc rd_page
                    inc rd_addr+1
                    jmp rd_in_page
.rd_in_done         lda rd_page
                    sta rd_next
                    clc
                    rts
.rd_in_overflow     sec
                    rts

\ Write the entry described by rd_name, rd_load, rd_exec, rd_len and rd_start
\ at index rd_count.

.rd_entry_write     lda rd_count
                    sta rd_index
                    jsr rd_entry_offset
                    ldx #0
.rd_write_name      txa
                    clc
                    adc rd_entry
                    jsr rd_cat_at
                    lda rd_name,x
                    jsr rd_write
                    inx
                    cpx #rd_name_size
                    bne rd_write_name
                    ldx #0
.rd_write_fields    lda rd_entry
                    clc
                    adc #rd_name_size
                    sta rd_temp
                    txa
                    clc
                    adc rd_temp
                    jsr rd_cat_at
                    lda rd_load,x
                    jsr rd_write
                    inx
                    cpx #6
                    bne rd_write_fields
                    lda rd_entry
                    clc
                    adc #13
                    jsr rd_cat_at
                    lda rd_start
                    jmp rd_write

\ ===========================================================================
\ *RDRUN <name>
\ ===========================================================================
\ Load a file and enter it at its execution address. The jump is indirect
\ through the zero page pair, so a program may be entered anywhere.

.rd_run_cmd         jsr detect_jim_machine
                    jsr rd_open
                    bcc rd_run_have_disk
                    jmp rd_no_disk
.rd_run_have_disk
                    jsr skipspace1
                    jsr read_cli_param
                    cpx #0
                    beq rd_usage_run
                    jsr rd_set_name
                    jsr rd_find
                    bcc rd_run_found
                    jmp rd_not_found
.rd_run_found
                    lda rd_load
                    sta rd_addr
                    lda rd_load+1
                    sta rd_addr+1
                    jsr rd_copy_out
                    lda rd_exec
                    sta zp
                    lda rd_exec+1
                    sta zp+1
                    jmp (zp)

.rd_usage_run       jsr printtext
                    equs "Usage: RDRUN <name>",&0D,&EA
                    jmp call_claimed
