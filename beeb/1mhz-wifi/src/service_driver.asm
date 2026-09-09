\ ElkWiFi driver calls implemented through the Pi1MHz services mailbox.
\ This service ROM executes in the host I/O processor, including when invoked
\ by a Tube parasite, so use the 1MHz-bus FRED window directly.

drv_svc_addr_lo = &A6
drv_svc_addr_mid = &A7
drv_svc_addr_hi = &A8
drv_svc_data = &A9
drv_svc_command = &AA
drv_svc_guard_image = 86  \publish the host filing-vector guard
drv_uef_sbuft = &F5
drv_uef_sbufl = &F8
drv_uef_sbufh = &F9
drv_uef_pr_y = &C7
drv_uef_pr_r = &C8

drv_svc_status = 80
drv_svc_scan = 81
drv_svc_join = 82
drv_svc_ifcfg = 83
drv_svc_lapopt = 87
drv_svc_ping = 88
drv_svc_datetime = 89
drv_svc_cancel = 90
drv_svc_radio = 91
drv_svc_online = 92
drv_svc_vector_mirror = 86
drv_svc_uef_normalize = 93
drv_svc_uef_op_probe = 0
drv_svc_uef_op_begin = 1
drv_svc_uef_op_append = 2
drv_svc_uef_op_finalize = 3
drv_svc_uef_op_rewind = 4
drv_svc_uef_op_refill = 5
drv_svc_uef_op_close = 6
drv_svc_uef_op_republish = 7
uef_first_page = 1
drv_net_copy_public = 58
drv_net_unsupported = &27

\ Sixteen DEX/BNE iterations take 79 processor cycles. Even at the host's
\ maximum 2 MHz rate this is at least 39.5 us, comfortably beyond the
\ measured 6.8 us Pi Zero page-publication worst case. No FRED/JIM access is
\ permitted in this delay because every such access posts a newer FIQ event.
drv_svc_settle_iterations = 16

\ `errorspace` is &0100 on the Electron, which is the CPU stack page. The
\ general `heap` is &0900 and belongs to the current language or application.
\ Neither is safe for an OSWORD driver entered by an arbitrary application.
\ Reuse the original ElkWiFi network-printer workspace instead. PRINTER is not
\ part of 1MHzWifi and the original ROM reserves &0D90-&0DAF for `netprt`.
\ This gives the compatibility driver private transient state without writing
\ into ElkChat code or its live return stack.
drv_svc_workspace = netprt
drv_svc_timeout_lo = drv_svc_workspace+0
drv_svc_timeout_hi = drv_svc_workspace+1
drv_svc_saved_x = drv_svc_workspace+2
drv_svc_strings = drv_svc_workspace+3
drv_net_index = drv_svc_workspace+4
drv_net_port_lo = drv_svc_workspace+5
drv_net_port_hi = drv_svc_workspace+6
drv_net_chunk = drv_svc_workspace+7
drv_svc_cursor = drv_svc_workspace+8
drv_net_copy_count = drv_svc_workspace+9
drv_net_buf_x = drv_svc_workspace+10
drv_svc_response_count = drv_svc_workspace+11
drv_svc_timeout_outer = drv_svc_workspace+12
drv_svc_command_copy = drv_svc_workspace+13
drv_svc_cancelled = drv_svc_workspace+14

\ Function 8 must retain all four DNS bytes while subsequent mailbox reads
\ continue to use the shared cursor. These bytes complete the 19-byte block
\ inside the original 32-byte `netprt` allocation.
drv_net_ip = drv_svc_workspace+15
drv_net_type = drv_svc_workspace+24
drv_net_protocol_second = drv_svc_workspace+25
drv_uef_op = drv_svc_workspace+26
drv_uef_stream_pending = drv_svc_workspace+27
drv_uef_upload_active = drv_svc_workspace+28
drv_uef_format = drv_svc_workspace+29
drv_uef_generation_lo = drv_svc_workspace+30
drv_uef_generation_hi = drv_svc_workspace+31
drv_uef_generation_record_lo = 26
drv_uef_generation_record_hi = 27

drv_net_open = 45
drv_net_dns = 46
drv_net_connect = 47
drv_net_send = 50
drv_net_recv = 51
drv_net_close = 53
drv_net_status = 54

.service_driver_init
\Functions 0 and 1 reset the original ESP module. Pi1MHz has no separate
\cartridge processor to reset, so close volatile TCP state and return a
\normal driver response without disturbing the saved WiFi association.
.service_driver_reset
 jsr service_driver_net_close_silent
 ldx #<service_driver_ok_text
 ldy #>service_driver_ok_text
 jmp service_driver_rom_response

.service_driver_version
 lda #drv_svc_status
 jmp service_driver_begin

.service_driver_scan
 lda #drv_svc_scan
 jmp service_driver_begin

.service_driver_ifcfg
 lda #drv_svc_ifcfg
 jmp service_driver_begin

.service_driver_online
 lda #drv_svc_online
 jmp service_driver_begin

\Normalize the UEF window in place. Return only the first response character
\in A: copying the response into pageram would overwrite the UEF header.
.service_driver_uef_normalize
 lda #drv_svc_uef_normalize
 jsr service_driver_write_command
 \ Old Pi firmware left the capability byte undefined. Clear it before the
 \ legacy request so fallback cannot be enabled by stale mailbox contents.
 lda #17
 sta drv_svc_cursor
 ldy #0
 jsr service_driver_write_y
 jsr service_driver_dispatch
 pha
 lda #17
 sta drv_svc_cursor
 jsr service_driver_read_a
 cmp #'1'
 bne service_driver_uef_normalize_legacy
 lda #&80
 sta drv_uef_stream_pending
 bne service_driver_uef_normalize_done
.service_driver_uef_normalize_legacy
 lda #0
 sta drv_uef_stream_pending
.service_driver_uef_normalize_done
 pla
 rts

\ Rewind or refill the Pi-private UEF stream. These calls deliberately use
\ command 93 with an exact guarded request, preserving commands 94-113 for
\ the secure NetTools service. On return sbuf is the published window length,
\ sbuft bit 6 says it is the final window, and pr_r/pr_y select its first byte.
.service_driver_uef_stream_republish
 lda #drv_svc_uef_op_republish
 bne service_driver_uef_stream
.service_driver_uef_stream_rewind
 lda #drv_svc_uef_op_rewind
 bne service_driver_uef_stream
.service_driver_uef_stream_refill
 lda #drv_svc_uef_op_refill
.service_driver_uef_stream
 sta drv_uef_op
 cmp #drv_svc_uef_op_refill
 beq service_driver_uef_stream_restore_generation
 cmp #drv_svc_uef_op_append
 bne service_driver_uef_stream_generation_ready
.service_driver_uef_stream_restore_generation
 jsr service_driver_uef_generation_load
.service_driver_uef_stream_generation_ready
 lda #drv_svc_uef_normalize
 jsr service_driver_write_command
 ldx #0
.service_driver_uef_stream_request
 lda service_driver_uef_stream_template,x
 cpx #5
 beq service_driver_uef_stream_operation
 cpx #10
 beq service_driver_uef_stream_generation_lo
 cpx #11
 beq service_driver_uef_stream_generation_hi
 bne service_driver_uef_stream_byte
.service_driver_uef_stream_operation
 lda drv_uef_op
 jmp service_driver_uef_stream_byte
.service_driver_uef_stream_generation_lo
 lda drv_uef_op
 cmp #drv_svc_uef_op_refill
 beq service_driver_uef_stream_generation_lo_send
 cmp #drv_svc_uef_op_append
 bne service_driver_uef_stream_zero
.service_driver_uef_stream_generation_lo_send
 lda drv_uef_generation_lo
 jmp service_driver_uef_stream_byte
.service_driver_uef_stream_generation_hi
 lda drv_uef_op
 cmp #drv_svc_uef_op_refill
 beq service_driver_uef_stream_generation_hi_send
 cmp #drv_svc_uef_op_append
 bne service_driver_uef_stream_zero
.service_driver_uef_stream_generation_hi_send
 lda drv_uef_generation_hi
 jmp service_driver_uef_stream_byte
.service_driver_uef_stream_zero
 lda #0
.service_driver_uef_stream_byte
 tay
 jsr service_driver_write_y
 inx
 cpx #20
 bne service_driver_uef_stream_request
 jsr service_driver_dispatch
 cmp #'I'
 bne service_driver_uef_stream_invalid
 ldx #0
.service_driver_uef_stream_signature
 jsr service_driver_read_a
 cmp service_driver_uef_stream_reply,x
 bne service_driver_uef_stream_invalid
 inx
 cpx #4
 bne service_driver_uef_stream_signature
 \ A republish only repairs bytes the host has already been handed. Window
 \ length, final flag, generation and read cursor all still describe where the
 \ host is, so take none of them from the reply.
 lda drv_uef_op
 cmp #drv_svc_uef_op_republish
 beq service_driver_uef_stream_repaired
 ldx #4
.service_driver_uef_stream_skip_token
 jsr service_driver_read_a
 dex
 bne service_driver_uef_stream_skip_token
 jsr service_driver_read_a
 sta drv_uef_generation_lo
 jsr service_driver_read_a
 sta drv_uef_generation_hi
 jsr service_driver_uef_generation_save
 jsr service_driver_read_a
 jsr service_driver_read_a
 jsr service_driver_read_a
 sta drv_uef_sbufl
 jsr service_driver_read_a
 sta drv_uef_sbufh
 jsr service_driver_read_a
 beq service_driver_uef_stream_not_final
 lda drv_uef_sbuft
 ora #&40
 bne service_driver_uef_stream_store_flags
.service_driver_uef_stream_not_final
 lda drv_uef_sbuft
 and #&BF
.service_driver_uef_stream_store_flags
 ora #&80
 sta drv_uef_sbuft
 \ The reply buffer owns JIM page 0 in full: ElkChat and other OSWORD &65
 \ clients read up to 241 contiguous bytes from it, so the published stream
 \ starts at page 1 instead. Nothing then has to be repaired when a service
 \ reply lands while a UEF is being read.
 lda #0
 sta drv_uef_pr_y
 lda #uef_first_page
 sta drv_uef_pr_r
 jsr service_driver_read_a
 sta drv_uef_format
 clc
 rts
.service_driver_uef_stream_repaired
 clc
 rts
.service_driver_uef_stream_invalid
 sec
 rts

\ The active window generation must survive arbitrary cassette loaders. Some
\ Electron loaders occupy &0900-&10FF, which includes the original netprt
\ scratch allocation. Keep the authoritative copy next to the transactional
\ WiCFS record in Pi-private JIM, outside host RAM. Every refill restores this
\ copy before constructing its request, so overwriting netprt is harmless.
.service_driver_uef_generation_load
 php
 sei
 pha
 txa
 pha
 ldx #drv_uef_generation_record_lo
 jsr pi_state_address_x
 lda &FCA9
 jsr bus_delay
 sta drv_uef_generation_lo
 ldx #drv_uef_generation_record_hi
 jsr pi_state_address_x
 lda &FCA9
 jsr bus_delay
 sta drv_uef_generation_hi
 pla
 tax
 pla
 plp
 rts

.service_driver_uef_generation_save
 php
 sei
 pha
 txa
 pha
 ldx #drv_uef_generation_record_lo
 jsr pi_state_address_x
 lda drv_uef_generation_lo
 sta &FCA9
 jsr bus_delay
 ldx #drv_uef_generation_record_hi
 jsr pi_state_address_x
 lda drv_uef_generation_hi
 sta &FCA9
 jsr bus_delay
 pla
 tax
 pla
 plp
 rts

.service_driver_uef_stream_template
 equs "IUEF"
 equb 1,0
 equb 0,0,0,0               \ current session token: zero means active
 equb 0,0,0,0               \ synchronous next-window request
 equb 0,0                    \ no upload payload
 equb 0,0,0,0               \ no upload CRC
.service_driver_uef_stream_reply
 equs "UEF"
 equb 1

.service_driver_uef_stream_probe
 lda #drv_svc_uef_op_probe
 jmp service_driver_uef_stream
.service_driver_uef_stream_begin
 lda #drv_svc_uef_op_begin
 jmp service_driver_uef_stream
.service_driver_uef_stream_append
 lda #drv_svc_uef_op_append
 jmp service_driver_uef_stream
.service_driver_uef_stream_finalize
 lda #drv_svc_uef_op_finalize
 jmp service_driver_uef_stream
.service_driver_uef_stream_close
 lda #drv_svc_uef_op_close
 jmp service_driver_uef_stream

.service_driver_lapopt
 lda #drv_svc_lapopt
 jsr service_driver_write_command
 ldy #0
 lda (paramblok),y
 cmp #'7'
 bne service_driver_lapopt_127
 iny
 lda (paramblok),y
 cmp #&0D
 bne service_driver_lapopt_bad
 ldy #7
 bne service_driver_lapopt_send
.service_driver_lapopt_127
 ldy #0
 lda (paramblok),y
 cmp #'1'
 bne service_driver_lapopt_bad
 iny
 lda (paramblok),y
 cmp #'2'
 bne service_driver_lapopt_bad
 iny
 lda (paramblok),y
 cmp #'7'
 bne service_driver_lapopt_bad
 iny
 lda (paramblok),y
 cmp #&0D
 bne service_driver_lapopt_bad
 ldy #127
.service_driver_lapopt_send
 jsr service_driver_write_y
 jmp service_driver_dispatch
.service_driver_lapopt_bad
 jmp service_driver_error_parameter

.service_driver_mode
 ldy #0
 lda (paramblok),y
 beq service_driver_wifi_mode_query
 cmp #'1'
 bne service_driver_wifi_mode_error
 iny
 lda (paramblok),y
 cmp #&0D
 bne service_driver_wifi_mode_error
 ldx #<service_driver_ok_text
 ldy #>service_driver_ok_text
 jmp service_driver_rom_response
.service_driver_wifi_mode_query
 ldx #<service_driver_mode_text
 ldy #>service_driver_mode_text
 jmp service_driver_rom_response
.service_driver_wifi_mode_error
 ldx #<service_driver_error_text
 ldy #>service_driver_error_text
 jmp service_driver_rom_response

\ Functions 10 (CWLIF) and 12 (CIPSTATUS) share the original cartridge's
\ connection-status behaviour. Pi1MHz reports the raw handle state in byte
\ one. Render the ESP-compatible status class without exposing Pi internals.
.service_driver_connection_status
 jsr net_command_address
 lda #drv_net_status
 jsr net_write_a
 jsr net_dispatch_wait
 cmp #0
 beq service_driver_status_read
 jmp service_driver_net_error
.service_driver_status_read
 jsr net_command_address
 lda #1
 jsr net_address_low
 jsr net_read_a
 cmp #4                      \ NET_ST_CONNECTED
 beq service_driver_status_connected
 cmp #7                      \ NET_ST_ERROR
 beq service_driver_status_error
 ldx #<service_driver_status_ip_text
 ldy #>service_driver_status_ip_text
 jmp service_driver_rom_response
.service_driver_status_connected
 ldx #<service_driver_status_connected_text
 ldy #>service_driver_status_connected_text
 jmp service_driver_rom_response
.service_driver_status_error
 ldx #<service_driver_status_error_text
 ldy #>service_driver_status_error_text
 jmp service_driver_rom_response

\ Function 11 is the cartridge's local response-buffer finaliser. Preserve
\ the caller's low-byte length exactly as ElkWiFi 0.23 does. The Pi/AP5 page
\ selector is write-only, so the ROM's authoritative page shadow supplies
\ the high byte.
.service_driver_set_buffer
 ldx driver_entry_x
 jmp set_buffer

\ The original 0.23 source routes functions 15, 16 and 17 through the same
\ CIOBAUD query. There is no UART on Pi1MHz, so return its stable observable
\ response rather than raising a MOS error.
.service_driver_baud_compat
 ldx #<service_driver_baud_text
 ldy #>service_driver_baud_text
 jmp service_driver_rom_response

\ Watchdog and SSL-buffer controls have no Pi-side equivalent. They are safe
\ compatibility no-ops: the Pi owns its watchdog and dynamically sizes secure
\ service buffers.
.service_driver_ok
 ldx #<service_driver_ok_text
 ldy #>service_driver_ok_text
 jmp service_driver_rom_response

\ Function 27 controls transparent transfer mode. Pi1MHz supports normal
\ framed mode only, so query/set mode zero succeeds and mode one is rejected.
.service_driver_mode_unsupported
 ldy #0
 lda (paramblok),y
 beq service_driver_mode_query
 cmp #'0'
 bne service_driver_mode_error
 iny
 lda (paramblok),y
 cmp #&0D
 bne service_driver_mode_error
 ldx #<service_driver_ok_text
 ldy #>service_driver_ok_text
 jmp service_driver_rom_response
.service_driver_mode_query
 ldx #<service_driver_cipmode_text
 ldy #>service_driver_cipmode_text
 jmp service_driver_rom_response
.service_driver_mode_error
 ldx #<service_driver_error_text
 ldy #>service_driver_error_text
 jmp service_driver_rom_response

\ Function 9 selects connection multiplexing. Pi1MHz exposes exactly one raw
\ TCP connection, so the original single-connection request is a successful
\ no-op. Reject multiplexed mode rather than silently promising five sockets.
.service_driver_cpmux
 ldy #0
 lda (paramblok),y
 beq service_driver_cpmux_query
 cmp #'0'
 bne service_driver_cpmux_bad
 iny
 lda (paramblok),y
 cmp #&0D
 bne service_driver_cpmux_bad
 ldx #<service_driver_ok_text
 ldy #>service_driver_ok_text
 jmp service_driver_rom_response
.service_driver_cpmux_query
 ldx #<service_driver_cipmux_text
 ldy #>service_driver_cipmux_text
 jmp service_driver_rom_response
.service_driver_cpmux_bad
 jmp service_driver_error_parameter

.service_driver_unsupported
 ldx #(error_not_implemented-error_table)
 jmp error

.service_driver_mux_channel
 ldy #&FF                  \ single-connection mode
 clc
 rts

.service_driver_wifi_control
 ldx driver_entry_x
 \ `*WIFI ON` is the quickest positive Pi-link diagnostic. Do not return a
 \ ROM-local OK: require command 91 to make a complete FCA6/FCA9 round trip.
 beq service_driver_wifi_off
 lda #drv_svc_radio
 jmp service_driver_begin
.service_driver_wifi_off
 lda #drv_svc_join
 jsr service_driver_write_command
 ldy #3
 jsr service_driver_write_y
 jmp service_driver_dispatch

.service_driver_ping
 lda #0
 sta drv_svc_cancelled
 lda #drv_svc_ping
 jsr service_driver_write_command
 ldx #0
.service_driver_ping_copy
 txa
 tay
 lda (paramblok),y
 cmp #&0D
 bne service_driver_ping_char
 lda #0
.service_driver_ping_char
 stx drv_svc_saved_x
 tay
 jsr service_driver_write_y
 ldx drv_svc_saved_x
 cmp #0
 bne service_driver_ping_more
 jmp service_driver_dispatch
.service_driver_ping_more
 inx
 bne service_driver_ping_copy
 jmp service_driver_error_parameter

.service_driver_date
 ldy #0
 beq service_driver_datetime
.service_driver_time
 ldy #1
.service_driver_datetime
 lda #drv_svc_datetime
 jsr service_driver_write_command
 jsr service_driver_write_y
 jmp service_driver_dispatch

\ Raw TCP/UDP compatibility used by OSWORD &65 and the stock DATE/TIME tools.
.service_driver_cipstart
 \ X/Y point at CR-separated protocol, hostname and port strings. Parse the
 \ protocol before opening a Pi handle, so a rejected type leaves no live
 \ transport behind. SSL is never downgraded to plaintext.
 ldy #0
 lda (paramblok),y
 and #&DF
 cmp #'T'
 beq service_driver_protocol_tcp
 cmp #'U'
 beq service_driver_protocol_udp
 jmp service_driver_protocol_unsupported
.service_driver_protocol_tcp
 lda #0
 sta drv_net_type
 lda #'C'
 bne service_driver_protocol_second
.service_driver_protocol_udp
 lda #1
 sta drv_net_type
 lda #'D'
.service_driver_protocol_second
 sta drv_net_protocol_second
 iny
 lda (paramblok),y
 and #&DF
 cmp drv_net_protocol_second
 bne service_driver_protocol_unsupported
 iny
 lda (paramblok),y
 and #&DF
 cmp #'P'
 bne service_driver_protocol_unsupported
 iny
 lda (paramblok),y
 cmp #&0D
 bne service_driver_protocol_unsupported
 iny
 sty drv_net_index
 jmp service_driver_protocol_valid
.service_driver_protocol_unsupported
 ldx #<service_driver_error_text
 ldy #>service_driver_error_text
 jmp service_driver_rom_response
.service_driver_protocol_valid
 jsr service_driver_net_close_silent
 jsr net_command_address
 lda #drv_net_open
 jsr net_write_a
 lda drv_net_type
 jsr net_write_a
 jsr net_dispatch_wait
 cmp #0
 beq service_driver_open_ok
 jmp service_driver_net_error
.service_driver_open_ok
 jsr net_command_address
 lda #drv_net_dns
 jsr net_write_a
.service_driver_copy_host
 ldy drv_net_index
 lda (paramblok),y
 iny
 bne service_driver_host_index_ok
 jmp service_driver_error_parameter
.service_driver_host_index_ok
 sty drv_net_index
 cmp #&0D
 bne service_driver_host_char
 lda #0
.service_driver_host_char
 jsr net_write_a
 cmp #0
 bne service_driver_copy_host
 jsr net_dispatch_wait
 cmp #0
 beq service_driver_dns_ok
 jmp service_driver_net_error
.service_driver_dns_ok

 \ Save resolved address before reusing the command block.
 jsr net_command_address
 lda #4
 jsr net_address_low
 lda #0
 sta drv_net_copy_count
.service_driver_copy_ip
 jsr net_read_a
 ldx drv_net_copy_count
 sta drv_net_ip,x
 inc drv_net_copy_count
 lda drv_net_copy_count
 cmp #4
 bne service_driver_copy_ip

 \ Decimal TCP port follows the hostname.
 lda #0
 sta drv_net_port_lo
 sta drv_net_port_hi
.service_driver_port_digit
 ldy drv_net_index
 lda (paramblok),y
 cmp #&0D
 beq service_driver_connect
 sec
 sbc #'0'
 cmp #10
 bcc service_driver_port_valid
 jmp service_driver_net_error
.service_driver_port_valid
 pha
 lda drv_net_port_lo       \ port = port*10 + digit
 sta zp
 lda drv_net_port_hi
 sta zp+1
 asl drv_net_port_lo
 rol drv_net_port_hi
 asl drv_net_port_lo
 rol drv_net_port_hi
 lda drv_net_port_lo
 clc
 adc zp
 sta drv_net_port_lo
 lda drv_net_port_hi
 adc zp+1
 sta drv_net_port_hi
 asl drv_net_port_lo
 rol drv_net_port_hi
 pla
 clc
 adc drv_net_port_lo
 sta drv_net_port_lo
 bcc service_driver_port_next
 inc drv_net_port_hi
.service_driver_port_next
 inc drv_net_index
 bne service_driver_port_digit
 jmp service_driver_error_parameter

.service_driver_connect
 jsr net_command_address
 lda #drv_net_connect
 jsr net_write_a
 lda #0
 sta drv_net_index
.service_driver_write_ip
 ldx drv_net_index
 lda drv_net_ip,x
 jsr net_write_a
 inc drv_net_index
 lda drv_net_index
 cmp #4
 bne service_driver_write_ip
 lda drv_net_port_lo
 jsr net_write_a
 lda drv_net_port_hi
 jsr net_write_a
 jsr net_dispatch_wait
 cmp #0
 beq service_driver_connect_ok
 jmp service_driver_net_error
.service_driver_connect_ok
 ldx #<service_driver_connect_text
 ldy #>service_driver_connect_text
 jmp service_driver_rom_response

.service_driver_cipsend
 ldx driver_entry_x
 lda &0000,x
 sta data_pointer
 lda &0001,x
 sta data_pointer+1
 lda &0002,x
 sta data_counter
 lda &0003,x
 sta data_counter+1
 lda &0004,x
 sta data_counter+2
 lda #&FF
 sta drv_svc_timeout_hi
.service_driver_send_more
 lda data_counter
 ora data_counter+1
 ora data_counter+2
 bne service_driver_send_pending
 jmp service_driver_receive
.service_driver_send_pending
 lda #240
 sta drv_net_chunk
 lda data_counter+1
 ora data_counter+2
 bne service_driver_copy_send
 lda data_counter
 cmp #240
 bcs service_driver_copy_send
 sta drv_net_chunk
.service_driver_copy_send
 \ Build the scratch block without consuming the caller's source.  SEND may
 \ accept only a prefix, so the authoritative pointer and count advance only
 \ after Pi1MHz reports the exact number queued in command bytes 1..3.
 lda data_pointer
 pha
 lda data_pointer+1
 pha
 lda data_counter
 pha
 lda data_counter+1
 pha
 lda data_counter+2
 pha
 jsr net_scratch_address
 lda drv_net_chunk
 sta drv_net_copy_count
.service_driver_copy_send_byte
 ldy #0
 lda (data_pointer),y
 jsr net_write_a
 inc data_pointer
 bne service_driver_send_pointer_ok
 inc data_pointer+1
.service_driver_send_pointer_ok
 jsr dec_data_counter
 dec drv_net_copy_count
 bne service_driver_copy_send_byte

 pla
 sta data_counter+2
 pla
 sta data_counter+1
 pla
 sta data_counter
 pla
 sta data_pointer+1
 pla
 sta data_pointer

 jsr net_command_address
 lda #drv_net_send
 jsr net_write_a
 lda drv_net_chunk
 jsr net_write_a
 lda #0
 jsr net_write_a
 jsr net_write_a
 lda #0                    \ source offset &00FFF100
 jsr net_write_a
 lda #&F1
 jsr net_write_a
 lda #&FF
 jsr net_write_a
 lda #0
 jsr net_write_a
 jsr net_dispatch_wait
 cmp #0
 beq service_driver_send_ok
 jmp service_driver_net_error
.service_driver_send_ok
 jsr net_command_address
 lda #1
 jsr net_address_low
 jsr net_read_a
 sta drv_net_copy_count
 jsr net_read_a
 bne service_driver_send_count_bad
 jsr net_read_a
 bne service_driver_send_count_bad
 lda drv_net_copy_count
 beq service_driver_send_backpressure
 cmp drv_net_chunk
 beq service_driver_send_count_valid
 bcc service_driver_send_count_valid
.service_driver_send_count_bad
 jmp service_driver_net_error
.service_driver_send_count_valid
 lda #&FF
 sta drv_svc_timeout_hi
.service_driver_advance_send
 inc data_pointer
 bne service_driver_advance_send_pointer_ok
 inc data_pointer+1
.service_driver_advance_send_pointer_ok
 jsr dec_data_counter
 dec drv_net_copy_count
 bne service_driver_advance_send
 jmp service_driver_send_more
.service_driver_send_backpressure
 lda #19
 jsr osbyte
 dec drv_svc_timeout_hi
 beq service_driver_send_backpressure_timeout
 jmp service_driver_send_more
.service_driver_send_backpressure_timeout
 jmp service_driver_no_response

.service_driver_ipd
.service_driver_receive
 ldx #0
 stx driver_page_shadow
 stx drv_net_buf_x
 stx net_empty_lo
 lda #2                     \ up to 512 frames for the first response byte
 sta net_empty_hi
.service_driver_receive_more
 jsr net_command_address
 lda #drv_net_recv
 jsr net_write_a
 lda #240
 jsr net_write_a
 lda #0
 jsr net_write_a
 jsr net_write_a
 lda #0
 jsr net_write_a
 lda #&F1
 jsr net_write_a
 lda #&FF
 jsr net_write_a
 lda #0
 jsr net_write_a
 jsr net_dispatch_wait
 cmp #&20
 bne service_driver_receive_not_done
 jmp service_driver_receive_done
.service_driver_receive_not_done
 cmp #0
 beq service_driver_receive_ok
 jmp service_driver_net_error
.service_driver_receive_ok
 jsr net_command_address
 lda #1
 jsr net_address_low
 jsr net_read_a
 sta drv_net_chunk
 beq service_driver_receive_empty
 \ HTTP clients request Connection: close and normally receive explicit EOF.
 \ Retain a bounded ten-second gap for fragmented generic TCP responses.
 ldx #0
 stx net_empty_lo
 lda #2
 sta net_empty_hi
 jsr net_command_address
 lda #drv_net_copy_public
 jsr net_write_a
 lda drv_net_chunk
 jsr net_write_a
 lda drv_net_buf_x
 jsr net_write_a
 lda driver_page_shadow
 jsr net_write_a
 jsr net_dispatch_wait
 cmp #0
 beq service_driver_receive_bulk_ok
 cmp #drv_net_unsupported
 beq service_driver_receive_legacy_copy
 jmp service_driver_net_error
.service_driver_receive_bulk_ok
 clc
 lda drv_net_buf_x
 adc drv_net_chunk
 sta drv_net_buf_x
 bcs service_driver_receive_bulk_page
 jmp service_driver_receive_more
.service_driver_receive_bulk_page
 inc driver_page_shadow
 beq service_driver_receive_overflow
 jmp service_driver_receive_more
.service_driver_receive_legacy_copy
 \ Kernels predating command 58 still expose the received bytes through the
 \ original private scratch window. Preserve mixed ROM/kernel deployment by
 \ falling back to ElkWiFi's byte-at-a-time copy only for UNSUPPORTED.
 jsr net_scratch_address
.service_driver_receive_legacy_byte
 jsr net_read_a
 ldx drv_net_buf_x
 jsr service_driver_write_paged
 inx
 stx drv_net_buf_x
 bne service_driver_receive_legacy_next
 inc driver_page_shadow
 beq service_driver_receive_overflow
.service_driver_receive_legacy_next
 dec drv_net_chunk
 bne service_driver_receive_legacy_byte
 jmp service_driver_receive_more
.service_driver_receive_overflow
 jmp service_driver_net_error
.service_driver_receive_empty
 jsr check_esc
 bcs service_driver_receive_done
 lda #19
 jsr osbyte
 dec net_empty_lo
 beq service_driver_receive_empty_high
 jmp service_driver_receive_more
.service_driver_receive_empty_high
 dec net_empty_hi
 beq service_driver_receive_done
 jmp service_driver_receive_more
.service_driver_receive_done
 ldx drv_net_buf_x
 lda #0
 jsr service_driver_write_paged
 jmp restore_env

.service_driver_cipclose
 jsr service_driver_net_close_silent
 ldx #<service_driver_close_text
 ldy #>service_driver_close_text
 jmp service_driver_rom_response

.service_driver_net_close_silent
 jsr net_command_address
 lda #drv_net_close
 jsr net_write_a
 jsr net_dispatch_wait
 rts

.service_driver_net_error
 pha
 jsr service_driver_net_close_silent
 pla
 ldx #0
 stx driver_page_shadow
 lda #'E'
 jsr service_driver_write_paged
 inx
 lda #'R'
 jsr service_driver_write_paged
 inx
 jsr service_driver_write_paged
 inx
 lda #&0D
 jsr service_driver_write_paged
 inx
 lda #0
 jsr service_driver_write_paged
 jmp restore_env

\ X/Y point at a NUL-terminated ROM string.
.service_driver_rom_response
 stx zp
 sty zp+1
 ldx #0
 stx driver_page_shadow
 ldy #0
.service_driver_rom_response_copy
 lda (zp),y
 jsr service_driver_write_paged
 inx
 iny
 cmp #0
 bne service_driver_rom_response_copy
 dex
 jmp restore_env

.service_driver_connect_text
 equs "CONNECT",&0D,&0A,&0D,&0A,"OK",&0D,&0A,&0D,&0A,0
.service_driver_close_text
 equs "CLOSED",&0D,&0A,&0D,&0A,"OK",&0D,&0A,&0D,&0A,0
.service_driver_mode_text
 equs "+CWMODE:1",&0D,&0A,&0D,&0A,"OK",&0D,&0A,0
.service_driver_ok_text
 equs "OK",&0D,&0A,0
.service_driver_error_text
 equs "ERROR",&0D,&0A,0
.service_driver_status_ip_text
 equs "STATUS:2",&0D,&0A,&0D,&0A,"OK",&0D,&0A,0
.service_driver_status_connected_text
 equs "STATUS:3",&0D,&0A,&0D,&0A,"OK",&0D,&0A,0
.service_driver_status_error_text
 equs "STATUS:4",&0D,&0A,&0D,&0A,"OK",&0D,&0A,0
.service_driver_baud_text
 equs "+CIOBAUD:115200",&0D,&0A,&0D,&0A,"OK",&0D,&0A,0
.service_driver_cipmode_text
 equs "+CIPMODE:0",&0D,&0A,&0D,&0A,"OK",&0D,&0A,0
.service_driver_cipmux_text
 equs "+CIPMUX:0",&0D,&0A,&0D,&0A,"OK",&0D,&0A,0

.service_driver_join
 lda #drv_svc_join
 jsr service_driver_write_command
 ldy #0
 lda (paramblok),y
 beq service_driver_join_query
 ldy #1
 jsr service_driver_write_y
 lda #2
 sta drv_svc_strings
 ldx #0
.service_driver_join_copy
 txa
 tay
 lda (paramblok),y
 cmp #&0D
 bne service_driver_join_char
 lda #0
 dec drv_svc_strings
.service_driver_join_char
 stx drv_svc_saved_x
 tay
 jsr service_driver_write_y
 ldx drv_svc_saved_x
 lda drv_svc_strings
 bne service_driver_join_more
 jmp service_driver_dispatch
.service_driver_join_more
 inx
 bne service_driver_join_copy
 jmp service_driver_error
.service_driver_join_query
 ldy #0
 jsr service_driver_write_y
 jmp service_driver_dispatch

.service_driver_leave
 lda #drv_svc_join
 jsr service_driver_write_command
 ldy #2
 jsr service_driver_write_y
 jmp service_driver_dispatch

.service_driver_begin
 jsr service_driver_write_command
 jmp service_driver_dispatch

\ A contains the service command. Set command-page address and write byte 0.
.service_driver_write_command
 php
 sei
 pha
 lda #0
 sta &FC00+drv_svc_addr_lo
 jsr service_driver_bus_settle
 lda #&FF
 sta &FC00+drv_svc_addr_mid
 jsr service_driver_bus_settle
 sta &FC00+drv_svc_addr_hi
 jsr service_driver_bus_settle
 \ The services emulator makes all three address bytes readable.  Check them
 \ before touching the command block so an absent/unforwarded FCA6-FCA9 port
 \ is reported as a missing device, rather than timing out ambiguously.
 \ A settle is a CPU delay, not bus traffic, so the first read-back can still
 \ carry the pre-write value on a bus that publishes writes asynchronously.
 \ Poll the read-back the way the cursor wait does: only a port that never
 \ agrees is an absent device.
 txa
 pha
 ldx #0
.service_driver_addr_verify
 lda &FC00+drv_svc_addr_lo
 bne service_driver_addr_retry
 lda &FC00+drv_svc_addr_mid
 cmp #&FF
 bne service_driver_addr_retry
 lda &FC00+drv_svc_addr_hi
 cmp #&FF
 beq service_driver_addr_ok
.service_driver_addr_retry
 dex
 bne service_driver_addr_verify
 pla
 tax
 jmp service_driver_port_missing_near
.service_driver_addr_ok
 pla
 tax
 pla
 sta drv_svc_command_copy
 sta &FC00+drv_svc_data
 lda #1
 sta drv_svc_cursor
 jsr service_driver_wait_cursor
 \ Rewind byte zero and read it back.  The read auto-increments to byte one,
 \ which is exactly where callers expect to append their request arguments.
 lda #0
 sta &FC00+drv_svc_addr_lo
 jsr service_driver_bus_settle
 lda &FC00+drv_svc_data
 cmp drv_svc_command_copy
 bne service_driver_port_missing_after_command
 lda #1
 sta drv_svc_cursor
 jsr service_driver_wait_cursor
 plp
 rts
.service_driver_port_missing_near
 pla
 plp
 jmp service_driver_port_missing
.service_driver_port_missing_after_command
 plp
 jmp service_driver_port_missing
.service_driver_write_y
 php
 sei
 lda drv_svc_cursor
 sta &FC00+drv_svc_addr_lo
 jsr service_driver_bus_settle
 lda #&FF
 sta &FC00+drv_svc_addr_mid
 jsr service_driver_bus_settle
 sta &FC00+drv_svc_addr_hi
 jsr service_driver_bus_settle
 tya
 sta &FC00+drv_svc_data
 inc drv_svc_cursor
 jsr service_driver_wait_cursor
 plp
 rts

\ Read one response byte through an explicitly selected cursor. The value is
\ preserved while the asynchronous FCA9 increment is acknowledged.
.service_driver_read_a
 lda drv_svc_cursor
 sta &FC00+drv_svc_addr_lo
 jsr service_driver_bus_settle
 lda #&FF
 sta &FC00+drv_svc_addr_mid
 jsr service_driver_bus_settle
 sta &FC00+drv_svc_addr_hi
 jsr service_driver_bus_settle
 jsr service_driver_data_settle
 lda &FC00+drv_svc_data
 pha
 inc drv_svc_cursor
 jsr service_driver_wait_cursor
 pla
 rts

\ Selector read-back can precede publication of its FCA9 data byte. Wait using
\ processor-local instructions only. Reading a FRED register here would post
\ a newer Pi FIQ event and could destroy the pending selector write.
.service_driver_data_settle
 pha
 txa
 pha
 ldx #drv_svc_settle_iterations
.service_driver_data_settle_loop
 dex
 bne service_driver_data_settle_loop
 pla
 tax
 pla
 rts

\ Do not allow a late FCA9 callback to overwrite the selector for the next
\ byte. This is bounded so an absent or partially forwarded port still returns.
.service_driver_wait_cursor
 jsr service_driver_bus_settle
 pha
 txa
 pha
 ldx #0
.service_driver_wait_cursor_loop
 lda &FC00+drv_svc_addr_lo
 cmp drv_svc_cursor
 bne service_driver_wait_cursor_again
 lda &FC00+drv_svc_addr_mid
 cmp #&FF
 bne service_driver_wait_cursor_again
 lda &FC00+drv_svc_addr_hi
 cmp #&FF
 beq service_driver_wait_cursor_done
.service_driver_wait_cursor_again
 dex
 bne service_driver_wait_cursor_loop
.service_driver_wait_cursor_done
 pla
 tax
 pla
 rts

\ Allow the FIQ callback to publish selector writes and FCA9 increments.
\ FRED/JIM reads are not harmless delays: each one posts another bus event.
\ This helper is private to driver responses; it does not alter WiCFS or WGET.
.service_driver_bus_settle
 pha
 txa
 pha
 ldx #drv_svc_settle_iterations
.service_driver_bus_settle_loop
 dex
 bne service_driver_bus_settle_loop
 pla
 tax
 pla
 rts

.service_driver_dispatch
 php
 sei
 lda #&FF
 sta &FC00+drv_svc_command
 jsr service_driver_bus_settle
 plp
 lda #0
 sta drv_svc_timeout_lo
 lda #1
 sta drv_svc_timeout_outer
 \ Association and a real `*LAP` scan are asynchronous. Give those operations
 \ a long window. Fixed service handlers replace the
 \ request selector with their own BUSY or final status; an unchanged &FF is
 \ therefore diagnosed only when the bounded timeout expires.
 lda drv_svc_command_copy
 cmp #drv_svc_scan
 beq service_driver_long_timeout
 cmp #drv_svc_join
 beq service_driver_long_timeout
 cmp #drv_svc_ping
 beq service_driver_long_timeout
 cmp #drv_svc_datetime
 bne service_driver_short_timeout
.service_driver_long_timeout
 lda #&FF
 sta drv_svc_timeout_hi
 lda #8
 sta drv_svc_timeout_outer
 jmp service_driver_wait
.service_driver_short_timeout
 lda #100                 \ two seconds at one yield per video frame
 bne service_driver_set_timeout
.service_driver_set_timeout
 sta drv_svc_timeout_hi
.service_driver_wait
 lda &FC00+drv_svc_command
 bpl service_driver_result
 dec drv_svc_timeout_lo
 bne service_driver_wait
 lda drv_svc_command_copy
 cmp #drv_svc_cancel
 beq service_driver_wait_yield
 jsr check_esc
 bcc service_driver_wait_yield
 lda #&FF
 sta drv_svc_cancelled
 lda #drv_svc_cancel
 jmp service_driver_begin
.service_driver_wait_yield
 lda #19                  \ yield so the Pi cooperative poll can run
 jsr osbyte
 dec drv_svc_timeout_hi
 bne service_driver_wait
 dec drv_svc_timeout_outer
 beq service_driver_timeout
 lda #&FF
 sta drv_svc_timeout_hi
 jmp service_driver_wait
.service_driver_timeout
 \ An unchanged request selector means no fixed service handler claimed this
 \ command. Any other negative value is a claimed operation which did not
 \ complete within its deadline.
 lda &FC00+drv_svc_command
 cmp #&FF
 bne service_driver_timeout_claimed
 jmp service_driver_service_unclaimed
.service_driver_timeout_claimed
 jmp service_driver_no_response

.service_driver_result
 cmp #0
 beq service_driver_result_ok
 jmp service_driver_error
.service_driver_result_ok
 lda drv_svc_command_copy
 cmp #drv_svc_uef_normalize
 beq service_driver_result_no_copy
 php
 sei
 lda #&FF
 sta &FC00+drv_svc_addr_hi
 sta &FC00+drv_svc_addr_mid
 lda #1
 sta drv_svc_cursor
 \ Every implemented service response starts with visible non-space ASCII.
 \ Reject a floating/unimplemented FCA9 port before relying on JIM paged RAM;
 \ AP5 open-bus values are commonly &20, &FF or &00.
 jsr service_driver_read_a
 cmp #&21
 bcs service_driver_response_visible
 plp
 jmp service_driver_no_response
.service_driver_response_visible
 cmp #&7F
 bcc service_driver_response_ascii
 plp
 jmp service_driver_no_response
.service_driver_response_ascii
 pha
 lda #0
 sta data_pointer
 lda #240
 sta drv_svc_response_count
 ldx #0
 stx driver_page_shadow
 pla
 jsr service_driver_write_paged
 inx
 stx data_pointer
 dec drv_svc_response_count
.service_driver_copy_response
 jsr service_driver_read_a
 beq service_driver_response_done
 ldx data_pointer
 jsr service_driver_write_paged
 inx
 stx data_pointer
 dec drv_svc_response_count
 bne service_driver_copy_response
 \ A Pi1MHz reply is limited to 239 bytes plus its terminator.  A missing
 \ terminator usually means an absent service/floating 1MHz bus; never walk
 \ through 64K of host memory or raise ElkWiFi's misleading Buffer full error.
 plp
 jmp service_driver_no_response
.service_driver_response_done
 ldx data_pointer
 lda #0
 jsr service_driver_write_paged
 plp
 jmp restore_env

\ FCFF is shared, write-only AP5 state. Reselect it with interrupts masked for
\ every response byte so an IRQ-side MMFS/ADFS user cannot redirect the write.
.service_driver_write_paged
 php
 sei
 pha
 lda driver_page_shadow
 jsr select_public_page_a
 pla
 sta pageram,x
 jsr service_driver_bus_settle
 plp
 rts
.service_driver_result_no_copy
 php
 sei
 lda #&FF
 sta &FC00+drv_svc_addr_hi
 sta &FC00+drv_svc_addr_mid
 lda #1
 sta drv_svc_cursor
 jsr service_driver_read_a
 plp
 rts

.service_driver_error
 cmp #&42
 beq service_driver_error_unsupported
 cmp #&40
 beq service_driver_error_parameter
 cmp #&43
 bne service_driver_error_not_connect
 lda drv_svc_command_copy
 cmp #drv_svc_datetime
 beq service_driver_error_datetime
 jmp service_driver_error_connect
.service_driver_error_not_connect
 cmp #&44
 beq service_driver_error_no_wifi
 ldx #(error_no_response-error_table)
 jmp error
.service_driver_error_unsupported
 ldx #(error_not_implemented-error_table)
 jmp error
.service_driver_error_parameter
 ldx #(error_bad_option-error_table)
 jmp error
.service_driver_error_connect
 ldx #(error_opencon-error_table)
 jmp error
.service_driver_error_datetime
 ldx #(error_no_date_time-error_table)
 jmp error
.service_driver_error_no_wifi
 ldx #(error_device_not_found-error_table)
 jmp error

.service_driver_port_missing
 \ Address/data read-back failed: FCA6-FCA9 is not being served by Pi1MHz.
 ldx #(error_device_not_found-error_table)
 jmp error

.service_driver_service_unclaimed
 \ The services port echoed the dispatch page but no registered range claimed
 \ command 80.  This specifically identifies an old/mismatched Pi kernel.
 ldx #(error_not_implemented-error_table)
 jmp error

.service_driver_no_response
 \ Do not communicate this through pageram: when the Pi/JIM service is absent
 \ there is nowhere for that write to land and generic_cmd prints open-bus
 \ spaces.  Raise the stock ElkWiFi error directly instead.
 ldx #(error_no_response-error_table)
 jmp error
