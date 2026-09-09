\ lap.asm
\ 1MHz-WiFi ROM: *LAP and *LAPOPT.
\
\ Written for the 1MHz-WiFi project, replacing the inherited version.
\
\   *LAP            list the access points the Pi can see
\   *LAPOPT [n]     set the scan options, defaulting to 127
\
\ Both are driver calls whose reply the driver prints, so there is nothing to
\ do here beyond preparing the parameter block.

.lap_cmd            lda #3
                    jmp generic_cmd

.lapopt_cmd         jsr skipspace1
                    jsr read_cli_param
                    cpx #&00
                    bne lapopt_param

\ No option given: put the default of 127 in the parameter block as the digits
\ the driver expects, terminated with a carriage return.
                    lda #'1'
                    sta heap
                    lda #'2'
                    sta heap+1
                    lda #'7'
                    sta heap+2
                    lda #&0D
                    sta heap+3
                    bne do_lapopt               \ always taken, A is not zero

.lapopt_param       ldx #0
                    jsr copy_to_heap

.do_lapopt          ldx #>heap
                    ldy #<heap
                    lda #25
                    jmp generic_cmd
