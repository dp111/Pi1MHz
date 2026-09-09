\ join.asm
\ 1MHz-WiFi ROM: *JOIN and *LEAVE.
\
\ Written for the 1MHz-WiFi project, replacing the inherited version.
\
\   *JOIN <ssid> [password]     associate, prompting for the password if it was
\                               not given on the command line
\   *JOIN ?                     report the network currently associated
\   *LEAVE                      disassociate
\
\ Both build a parameter block at heap and hand it to the driver, which is what
\ generic_cmd expects in X and Y.

.join_cmd           jsr skipspace1
                    jsr read_cli_param          \ X is the parameter length
                    cpx #&00
                    bne join_have_ssid
                    jsr printtext
                    equs "Usage: JOIN <ssid> [password]",&0D,&EA
                    jmp call_claimed

.join_have_ssid     ldx #0                      \ start the parameter block
                    jsr copy_to_heap
                    lda heap
                    cmp #'?'
                    beq query_network

                    stx save_x                  \ where the password will go
                    jsr skipspace1
                    jsr read_cli_param
                    cpx #0
                    bne copy_password

\ No password on the command line, so read one from the keyboard without
\ echoing it. read_cli_param left X at zero, but set it here rather than rely
\ on that.
                    jsr printtext
                    equs "Enter password: ",&EA
                    ldx #0
.enter_password     jsr osrdch
                    cmp #&7F
                    beq delete
                    sta strbuf,x                \ the carriage return is stored
                    pha                         \ too, and terminates the copy
                    lda #'*'
                    jsr oswrch
                    pla
                    inx
                    bmi copy_password           \ 128 characters is enough
                    cmp #&0D
                    bne enter_password
                    jsr osnewl

.copy_password      ldx save_x
                    jsr copy_to_heap
                    jmp join_network

.delete             cpx #0
                    beq enter_password          \ nothing to rub out yet
                    jsr oswrch
                    dex
                    bpl enter_password

\ *JOIN ? asks the driver which network is associated, which it reports when
\ the parameter block is empty.
.query_network      lda #0
                    sta heap

.join_network       ldx #>heap
                    ldy #<heap
                    lda #&04
                    jmp generic_cmd

.leave_cmd          lda #&05
                    jmp generic_cmd
