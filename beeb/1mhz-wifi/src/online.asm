\ One-line network readiness check.
\ Syntax: *ONLINE

.online_cmd
 jsr service_driver_online
 jmp generic_response
