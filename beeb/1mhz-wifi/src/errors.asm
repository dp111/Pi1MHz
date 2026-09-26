\ ElkWiFi-compatible MOS error construction.
\
\ The error block is built at &0100, the bottom of the stack page, as Acorn's
\ own ROMs do. It must be in main memory: by the time the language's error
\ handler reads the message, a different ROM is paged in, so a block inside
\ this image (where the workspace now lives) reads back as that ROM's bytes.
\ The *WGET -S copy also borrows &0100, but only with interrupts off and puts
\ it back before any error can be raised.

error_workspace = &0100

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
.error_not_swram        equs "Not swram",&0D
.error_stack_deep       equs "Stack full",&0D
