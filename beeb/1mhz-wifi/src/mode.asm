\ mode.asm
\ 1MHz-WiFi ROM: *MODE.
\
\ Written for the 1MHz-WiFi project, replacing the inherited version.
\
\   *MODE 1         station mode
\   *MODE ?         report the current mode
\
\ The inherited ROM offered access point and combined modes as well. Pi1MHz
\ associates as a station only, so those are not offered and the usage text
\ lists what the hardware will actually do.

.mode_cmd           jsr skipspace1
                    jsr read_cli_param
                    cpx #&00
                    bne mode_init_heap
                    jsr printtext
                    equs "Usage: *MODE <1|?>",&0D
                    equs "MODE 1 -> STATION",&0D
                    equb &EA
                    jmp call_claimed

.mode_init_heap     ldx #0
                    jsr copy_to_heap
                    lda heap
                    cmp #'?'
                    bne set_mode
                    lda #0                      \ an empty block is the query
                    sta heap

.set_mode           ldx #>heap
                    ldy #<heap
                    lda #&07
                    jmp generic_cmd
