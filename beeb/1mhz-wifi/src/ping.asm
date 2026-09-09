\ ElkWiFi-compatible PING through the Pi1MHz service.

\ Never use errorspace (&0100) as persistent state: it is the CPU stack page.
\ PING does not run concurrently with another driver command, so it can reuse
\ the service driver's volatile heap range.
ping_wait_count = heap+&B0
ping_request_count = heap+&B1

.ping_cmd
 jsr skipspace1
 jsr read_cli_param
 cpx #&00
 bne ping_start
 jsr printtext
 equs "Usage: *PING <hostname or IP>",&0D,&EA
 jmp call_claimed

.ping_start
 ldx #5
 stx ping_request_count
\ The inherited ROM set a UART timeout at time_out here. That address is &0144,
\ inside the processor stack, and nothing has read it since the UART went, so
\ the store is removed rather than relocated.
.ping_loop
 ldx #>strbuf
 ldy #<strbuf
 lda #28
 jsr wifidriver
 lda drv_svc_cancelled
 bne ping_cancelled

 jsr reset_buffer
 jsr read_buffer
 cmp #'+'
 bne ping_error
 jsr read_buffer
 cmp #'t'
 beq ping_time_out
 jsr printtext
 equs "Received response in ",&EA
 jsr reset_buffer
 jsr read_buffer              \ skip the leading '+'
.ping_print_ms
 jsr read_buffer              \ reselect FCFF after every MOS call
 cmp #&0D
 beq ping_ms
 sta save_y
 jsr oswrch
 lda save_y
 bpl ping_print_ms
.ping_ms
 jsr printtext
 equs " ms",&0D,&EA
.ping_wait
 ldx #50
 stx ping_wait_count
.ping_wait_loop
 jsr check_esc
 bcs ping_cancelled
 lda #19
 jsr osbyte
 dec ping_wait_count
 bne ping_wait_loop

 dec ping_request_count
 bne ping_loop
.ping_cancelled
 jmp call_claimed

.ping_error
 jsr printtext
 equs "Host error (dns or network)",&0D,&EA
 jmp ping_wait

.ping_time_out
 jsr printtext
 equs "No response received from host",&0D,&EA
 jmp ping_wait
