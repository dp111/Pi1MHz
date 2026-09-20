\ Pi1MHz-backed DATE/TIME.  Command 89 obtains UTC from NTP cooperatively;
\ no external HTTP endpoint or downloadable parser is involved.

.time_cmd
 jsr service_driver_time
 jmp generic_response

.date_cmd
 jsr service_driver_date
 jmp generic_response
