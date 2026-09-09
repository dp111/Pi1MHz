\ 1MHz-WiFi version and shared simple-command response handler.

\ Syntax: *VERSION

.version_cmd
  jsr printtext
 equs "1MHz-WiFi 0.1.67 (C) 2026 Peter Clarke",&0D
  equs "Parts from ElkWiFi (C) 2020 Roland Leurs",&0D,&EA

  \ Print the Pi1MHz service version after the two ROM attribution lines.
  lda #2

.generic_cmd
  jsr wifidriver
.generic_response
  jsr reset_buffer
  jsr read_buffer
  beq no_device
  dex
.version_l2
  jsr read_buffer
  jsr oswrch
  lda datalen+1
  cmp driver_page_shadow
  bne version_l2
  cpx datalen
  bne version_l2

.version_end
  jmp call_claimed

.no_device
  ldx #(error_no_response-error_table)
  jmp error
