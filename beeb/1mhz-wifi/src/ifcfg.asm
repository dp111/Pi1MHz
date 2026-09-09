\ ifcfg.asm
\ 1MHz-WiFi ROM: *IFCFG.
\
\ Written for the 1MHz-WiFi project, replacing the inherited version.
\
\   *IFCFG          report the interface address configuration
\
\ Driver call 18 takes no parameters and prints its own reply.

.ifcfg_cmd          lda #18
                    jmp generic_cmd
