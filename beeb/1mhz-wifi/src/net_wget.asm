\ Pi1MHz-native WGET transport.
\
\ The original ElkWiFi command talks to its 16C2552 at &FC30.  That address
\ is not forwarded by the AP5 and is unsafe from a Tube parasite.  This
\ implementation uses Pi1MHz's URL service through the FCA6 mailbox and
\ accesses the AP5-forwarded FRED services window from the I/O processor.


net_cmd_url_open = 60
net_cmd_url_read = 61
net_cmd_url_close = 63
net_cmd_url_status = 64
net_result_eof = &20
net_result_http_status = &30
net_result_unsupported = &27
net_cmd_copy_public = 58

wget_OSFIND = &FFCE
wget_OSBPUT = &FFD4

net_count = heap+&E8
net_cli_y = heap+&E9
\ The raw ElkWiFi OSWORD receive path shares these counters and the cursor
\ helpers below. They must not occupy the &0900 ADFS/application workspace.
net_result = heap+&EE
net_transfer_ok = heap+&EF
net_received = heap+&F0
net_bytes_lo = heap+&F3
net_bytes_hi = heap+&F4
net_paged_page = heap+&E4
net_paged_offset = heap+&E5
\ heap+&E6 belongs to the host BASIC transition workspace. Use &E7.
net_primary_page = heap+&E7
net_file_handle = heap+&E0
net_file_mode = heap+&E1
net_bytes_bank = heap+&E2
\ heap+&E3 and &E7 are otherwise unused across the whole ROM build.
\ &F5-&F7 collide with wget.asm's proto/newln/clptr, and &E6 belongs to the
\ host BASIC transition workspace. Both share the same "heap" workspace.

\ Close the ElkWiFi-compatible raw socket and display the Pi response. The
\ inherited wget_close routine is also an internal silent cleanup path.
.disconnect_cmd
 lda #14
 jmp generic_cmd

.pi_wget_cmd
 jsr detect_jim_machine
 lda #0
 sta net_transfer_ok
 sta net_received
 sta net_bytes_lo
 sta net_bytes_hi
 sta net_paged_page
 sta net_paged_offset
 sta net_primary_page
 sta net_file_handle
 sta net_file_mode
 sta net_bytes_bank
 sta tflag
 sta sflag
 sta aflag
 sta pflag
 sta uflag
 sta proto
 sta load_addr
 sta load_addr+1
 sta laddr
 sta laddr+1
 lda #&0D
 sta newln

.pi_wget_param
 jsr skipspace1
 jsr read_cli_param
 cpx #0
 bne pi_wget_have_param
 jmp pi_wget_usage

.pi_wget_have_param
 lda strbuf
 cmp #'-'
 bne pi_wget_url
 lda strbuf+1
 ora #&20
 cmp #'t'
 beq pi_wget_text
 cmp #'x'
 beq pi_wget_unix_text
 cmp #'u'
 beq pi_wget_uef
 cmp #'s'
 beq pi_wget_sideways
 \ Container formats still use the original decoder, which requires the
 \ cartridge UART.  Fail explicitly instead of touching &FC30 on AP5/Tube.
 ldx #(error_not_implemented-error_table)
 jmp error

.pi_wget_uef
 lda #1
 sta uflag
 bne pi_wget_param

.pi_wget_sideways
 lda #1
 sta sflag
 bne pi_wget_param

.pi_wget_unix_text
 lda #&0A
 sta newln
.pi_wget_text
 lda #1
 sta tflag
 jmp pi_wget_param

.pi_wget_url
 \ Preserve the MOS command-line index while building the service request.
 sty net_cli_y
 jsr net_command_address
 lda #net_cmd_url_open
 jsr net_write_a
 lda #4                    \ NET_OPEN_READ
 jsr net_write_a
 ldx #0
.pi_wget_url_copy
 stx net_count
 lda strbuf,x
 cmp #&0D
 bne pi_wget_url_char
 lda #0
.pi_wget_url_char
 jsr net_write_a
 cmp #0
 beq pi_wget_url_done
 ldx net_count
 inx
 bne pi_wget_url_copy
 ldx #(error_bad_param-error_table)
 jmp error

.pi_wget_url_done
 \ Ordinary WGET writes a named file through the active MOS filing system.
 \ Text mode needs no destination, -U owns the public JIM window, and -S uses
 \ its final parameter as the sideways RAM slot number.
 ldy net_cli_y
 jsr skipspace1
 jsr read_cli_param
 lda tflag
 bne pi_wget_address_ok
 lda uflag
 bne pi_wget_address_ok
 lda sflag
 bne pi_wget_parse_slot
 cpx #0
 bne pi_wget_file_name
 jmp pi_wget_usage
.pi_wget_file_name
 lda #&FF
 sta net_file_mode
 jmp pi_wget_address_ok
.pi_wget_parse_slot
 cpx #0
 bne pi_wget_have_slot
 jmp pi_wget_usage
.pi_wget_have_slot
 ldx #load_addr
 jsr string2hex
 sta net_result
 lda load_addr
 sta laddr
 lda load_addr+1
 sta laddr+1
.pi_wget_address_ok
.pi_wget_address_ready
 jsr net_dispatch_wait
 cmp #0
 beq pi_wget_opened
 jsr pi_wget_network_error

.pi_wget_opened
 lda net_file_mode
 beq pi_wget_output_ready
 lda #&80                  \ open output file through the current filing system
 ldx #<strbuf
 ldy #>strbuf
 jsr wget_OSFIND
 sta net_file_handle
 bne pi_wget_output_ready
 jsr pi_wget_close
 jsr printtext
 equs "Cannot create file",&0D,&EA
 jmp call_claimed
.pi_wget_output_ready
 lda #0
 sta net_empty_lo
 sta pr_r
 sta pr_y
 sta sbufl
 sta sbufh
 lda #10
 sta net_empty_hi

.pi_wget_read
 jsr net_command_address
 lda #net_cmd_url_read
 jsr net_write_a
 lda #240                  \ maximum bytes in the scratch page
 jsr net_write_a
 lda #0
 jsr net_write_a
 jsr net_write_a
 lda #0                    \ JIM offset &00FFF100, little endian
 jsr net_write_a
 lda #&F1
 jsr net_write_a
 lda #&FF
 jsr net_write_a
 lda #0
 jsr net_write_a
 jsr net_dispatch_wait
 cmp #net_result_eof
 bne pi_wget_not_eof
 jmp pi_wget_done
.pi_wget_not_eof
 cmp #0
 beq pi_wget_read_length
 jsr pi_wget_network_error

.pi_wget_read_length
 jsr net_command_address
 lda #1
 jsr net_address_low
 jsr net_read_a
 sta net_count
 bne pi_wget_have_bytes
 jmp pi_wget_empty
.pi_wget_have_bytes
 lda #0
 sta net_empty_lo
 lda #10
 sta net_empty_hi
 lda tflag
 bne pi_wget_copy_legacy
 lda uflag
 ora sflag
 beq pi_wget_copy_legacy
 \ Raw paged transfers are already in the Pi service scratch page. Copy the
 \ complete chunk to its final public JIM address in one Pi-side operation.
 \ This avoids both a host byte loop and the unsafe page-&FF staging alias.
 clc
 lda net_paged_offset
 adc net_count
 lda net_paged_page
 adc #0
 bcc pi_wget_copy_paged_bulk
 jsr pi_wget_close
 ldx #(error_buffer_full-error_table)
 jmp error
.pi_wget_copy_paged_bulk
 jsr net_command_address
 lda #net_cmd_copy_public
 jsr net_write_a
 lda net_count
 jsr net_write_a
 lda net_paged_offset
 jsr net_write_a
 lda net_paged_page
 jsr net_write_a
 jsr net_dispatch_wait
 cmp #0
 bne pi_wget_copy_paged_not_ok
 jmp pi_wget_copy_paged_advance
.pi_wget_copy_paged_not_ok
 cmp #net_result_unsupported
 bne pi_wget_copy_paged_error
 \ A previous Pi1MHz kernel has no command 58. Retain the original safe,
 \ byte-at-a-time path so ROM and kernel updates need not be atomic.
.pi_wget_copy_legacy
 jsr net_scratch_address

.pi_wget_copy
 jsr net_read_a
 ldx tflag
 beq pi_wget_store
 cmp newln
 bne pi_wget_print
 lda #&0D
.pi_wget_print
 jsr osasci
 jmp pi_wget_copied
.pi_wget_store
 pha
 lda net_file_mode
 bne pi_wget_store_file
 pla
 jsr pi_wget_store_paged
 jmp pi_wget_copied
.pi_wget_store_file
 pla
 ldy net_file_handle
 jsr wget_OSBPUT
 jmp pi_wget_copied
.pi_wget_copied
 lda #&FF
 sta net_received
 inc net_bytes_lo
 bne pi_wget_counted
 inc net_bytes_hi
 bne pi_wget_counted
 inc net_bytes_bank
.pi_wget_counted
 dec net_count
 bne pi_wget_copy
 jsr check_esc
 bcs pi_wget_copy_cancel
 jmp pi_wget_read
.pi_wget_copy_paged_error
 jsr pi_wget_network_error
.pi_wget_copy_paged_advance
 lda #&FF
 sta net_received
 clc
 lda net_paged_offset
 adc net_count
 sta net_paged_offset
 lda net_paged_page
 adc #0
 sta net_paged_page
 clc
 lda net_bytes_lo
 adc net_count
 sta net_bytes_lo
 lda net_bytes_hi
 adc #0
 sta net_bytes_hi
 jsr check_esc
 bcs pi_wget_copy_cancel
 jmp pi_wget_read
.pi_wget_copy_cancel
 jsr pi_wget_close
 jmp call_claimed

.pi_wget_empty
 \ An open HTTP stream may legitimately have no bytes yet.  Allow roughly
 \ 50 seconds without progress, while still honouring Escape.
 jsr check_esc
 bcc pi_wget_empty_wait
 jmp pi_wget_copy_cancel
.pi_wget_empty_wait
 lda #19
 jsr osbyte
 dec net_empty_lo
 beq pi_wget_empty_high
 jmp pi_wget_read
.pi_wget_empty_high
 dec net_empty_hi
 beq pi_wget_empty_timeout
 jmp pi_wget_read
.pi_wget_empty_timeout
 jsr pi_wget_timeout

.pi_wget_done
 lda net_received
 bne pi_wget_has_response
 jmp pi_wget_empty_response
.pi_wget_has_response
 lda #&FF
 sta net_transfer_ok
 lda uflag
 ora sflag
 bne pi_wget_paged_finish
 jmp pi_wget_finish_close
.pi_wget_paged_finish
 lda #0
 sta pr_r
 sta pr_y
 lda net_bytes_lo
 sta sbufl
 lda net_bytes_hi
 sta sbufh
 php
 sei
 jsr set_bank_1
 lda #&FF
 sta pagereg
 jsr bus_delay
 lda net_bytes_lo
 sta &FDFE
 jsr bus_delay
 lda net_bytes_hi
 sta &FDFF
 jsr bus_delay
 plp
 lda uflag
 beq pi_wget_normalized
 \ ElkWiFi -U means "store in paged RAM"; it does not promise that the
 \ payload is a UEF. Only ask the Pi to expand inputs carrying a gzip or ZIP
 \ signature; ordinary raw paged-RAM downloads must remain byte-exact.
 \ This also keeps ordinary -U transfers compatible with kernels predating
 \ the optional normalisation service.
 lda #'R'
 sta net_result
 php
 sei
 lda #0
 sta pagereg
 jsr bus_delay
 lda pageram
 sta zp
 lda pageram+1
 sta zp+1
 plp
 lda zp
 cmp #&1F
 bne pi_wget_check_zip
 lda zp+1
 cmp #&8B
 beq pi_wget_normalize_compressed
.pi_wget_check_zip
 lda zp
 cmp #'P'
 bne pi_wget_raw_paged
 lda zp+1
 cmp #'K'
 bne pi_wget_raw_paged
.pi_wget_normalize_compressed
 jsr service_driver_uef_normalize
 cmp #'I'
 bne pi_wget_not_invalid_uef
 jsr pi_wget_close
 jsr set_bank_0
 jmp pi_wget_invalid_uef
.pi_wget_not_invalid_uef
 cmp #'T'
 bne pi_wget_normalize_ok
 jsr pi_wget_close
 jsr set_bank_0
 jmp pi_wget_too_large
.pi_wget_normalize_ok
 sta net_result
 \ Command 93 rewrites the authoritative trailer with the expanded length.
 php
 sei
 jsr set_bank_1
 lda #&FF
 sta pagereg
 jsr bus_delay
 lda &FDFE
 sta net_bytes_lo
 sta sbufl
 lda &FDFF
 sta net_bytes_hi
 sta sbufh
 plp
 jmp pi_wget_normalized
.pi_wget_raw_paged
 php
 sei
 lda #&FF
 sta pagereg
 jsr bus_delay
 plp
.pi_wget_normalized
 php
 sei
 jsr set_bank_0
 lda net_primary_page
 sta pagereg
 jsr bus_delay
 plp
 lda sflag
 beq pi_wget_finish_close
 jsr wget_copy_file_to_swr
.pi_wget_finish_close
 jsr pi_wget_close
 jsr printtext
 equs "WGET ",&EA
 lda uflag
 beq pi_wget_report_format_done
 lda net_result
 cmp #'G'
 bne pi_wget_report_zip
 jsr printtext
 equs "GZIP ",&EA
 jmp pi_wget_report_format_done
.pi_wget_report_zip
 cmp #'Z'
 bne pi_wget_report_raw
 jsr printtext
 equs "ZIP ",&EA
 jmp pi_wget_report_format_done
.pi_wget_report_raw
 jsr printtext
 equs "RAW ",&EA
.pi_wget_report_format_done
 jsr printtext
 equs "OK &",&EA
 lda net_file_mode
 beq pi_wget_report_16bit_size
 lda net_bytes_bank
 jsr printhex
.pi_wget_report_16bit_size
 lda net_bytes_hi
 jsr printhex
 lda net_bytes_lo
 jsr printhex
 jsr printtext
 equs " bytes",&EA
 lda tflag
 bne pi_wget_report_end
 lda net_file_mode
 bne pi_wget_report_file
 jmp pi_wget_report_jim
.pi_wget_report_file
 jsr printtext
 equs " saved",&EA
 jmp pi_wget_report_end
.pi_wget_report_jim
 jsr printtext
 equs " in JIM",&EA
.pi_wget_report_end
 jsr osnewl
 jmp call_claimed

.pi_wget_empty_response
 jsr printtext
 equs "Empty response",&0D,&EA
 jmp pi_wget_close_claimed

.pi_wget_store_paged
 php
 sei                         \ FCFF and the JIM aperture are shared by AP5 users
 pha
 jsr set_bank_1
 lda net_paged_page
 sta pagereg
 jsr bus_delay
 ldy net_paged_offset
 pla
 sta pageram,y
 jsr bus_delay
 iny
 bne pi_wget_paged_pointer_ok
 inc net_paged_page
 beq pi_wget_paged_full
 lda net_paged_page
 sta pagereg
 jsr bus_delay
.pi_wget_paged_pointer_ok
 sty net_paged_offset
 jsr set_bank_0
 lda net_primary_page
 sta pagereg
 jsr bus_delay
 plp
 rts
.pi_wget_paged_full
 jsr set_bank_0
 lda net_primary_page
 sta pagereg
 jsr bus_delay
 plp
 jsr pi_wget_close
 ldx #(error_buffer_full-error_table)
 jmp error

.pi_wget_close
 pha
 lda net_file_handle
 beq pi_wget_file_closed
 tay
 lda #0
 sta net_file_handle
 jsr wget_OSFIND
.pi_wget_file_closed
 pla
 jsr net_command_address
 lda #net_cmd_url_close
 jsr net_write_a
 jsr net_dispatch_wait
 rts

.pi_wget_usage
 jsr printtext
 equs "Usage: WGET <url> <file>",&0D
 equs "       WGET [-TXUS] <url> [slot]",&0D,&EA
 jmp call_claimed

.pi_wget_timeout
 jsr printtext
 equs "Network timeout",&0D,&EA
 jmp pi_wget_close_claimed

.pi_wget_network_error
 jsr print_network_error
 lda net_result
 cmp #net_result_http_status
 bne pi_wget_close_claimed
 \ Preserve the server's actual HTTP status before URL_CLOSE clears the
 \ handle.  Error &30 alone cannot distinguish a redirect or rejection from
 \ a malformed/corrupted response on physical Pi1MHz hardware.
 jsr net_command_address
 lda #net_cmd_url_status
 jsr net_write_a
 jsr net_dispatch_wait
 cmp #0
 bne pi_wget_close_claimed
 lda #7
 jsr net_address_low
 jsr net_read_a
 sta load_addr
 jsr net_read_a
 sta load_addr+1
 jsr printtext
 equs "HTTP status &",&EA
 lda load_addr+1
 jsr printhex
 lda load_addr
 jsr printhex
 jsr osnewl
.pi_wget_close_claimed
 jsr pi_wget_close
 jmp call_claimed

.print_network_error
 sta net_result
 jsr printtext
 equs "Network error &",&EA
 lda net_result
 jsr printhex
 jmp osnewl

\ The Pi normaliser rejected the downloaded image, or it will not fit in the
\ JIM window. *WGET -U reports that itself: the filing system ROM is a
\ separate image and may not even be fitted.

.pi_wget_invalid_uef
 jsr printtext
 equs "Invalid UEF, gzip or ZIP file",&0D,&EA
 jmp call_claimed

.pi_wget_too_large
 jsr printtext
 equs "Expanded UEF exceeds &FFFE bytes",&0D,&EA
 jmp call_claimed
