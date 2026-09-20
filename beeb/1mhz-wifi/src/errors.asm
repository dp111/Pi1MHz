\ ElkWiFi-compatible MOS error construction.
\
\ The original Electron ROM used &0100 as a temporary BRK block. That is the
\ processor stack, so a driver error entered by an application could overwrite
\ live return addresses and subsequently report Bad program. 1MHzWifi removes
\ the network printer and therefore owns its original 32-byte `netprt` block.
\ The longest emitted error, including BRK, number and terminator, fits there.

error_workspace = netprt

.error
    lda #&00
    tay
    sta error_workspace
    lda #&00
    sta error_workspace+1
.error_loop
    lda error_table,x
    cmp #&0D
    beq error_exec
    sta error_workspace+2,y
    inx
    iny
    bne error_loop
.error_exec
    lda #&00
    sta error_workspace+2,y
    jmp error_workspace

.error_table
.error_device_not_found equs "Device not found",&0D
.error_no_response      equs "No response from device",&0D
.error_buffer_full      equs "Buffer full",&0D
.error_buffer_empty     equs "Buffer empty",&0D
.error_no_date_time     equs "No date/time received",&0D
.error_no_version       equs "No version received",&0D
.error_not_implemented  equs "Not implemented",&0D
.error_bad_option       equs "Unknown option",&0D
.error_bad_protocol     equs "Unknown protocol",&0D
.error_http_status      equs "HTTP error",&0D
.error_no_pagedram      equs "No paged ram",&0D
.error_disabled         equs "Wifi is disabled",&0D
.error_opencon          equs "Connect error",&0D
.error_bad_param        equs "Wrong parameter",&0D
