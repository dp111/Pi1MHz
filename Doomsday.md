Doomsday notes


TODO notes in no order

Check mouse offsets and fix VFS ROM bug
*MOUSE appears to not always work, see if original VFS has this bug and fix
See if boot time can be improved

More Fcode support (including VP4 and VP5 )
See if power consumption of the Pi can be improved
check and fix YUV scaling
composite video out ?
50Hz HDMI modes
Check 1MHz timing / add DMBs ( sometimes bad FSMAP)
Check beeb screen offset vertical appears off by a pixel ?
Menu system
Eject sort out

Default VFS layout

  ┌───────────┬────────────────────────────────────────┬─────────────┐
  │ BeebVFS   │                  Disc                  │    Code     │
  ├───────────┼────────────────────────────────────────┼─────────────┤
  │ 1 / 2     │ Community South / North                │ 1066 / 1067 │
  ├───────────┼────────────────────────────────────────┼─────────────┤
  │ 3 / 4     │ National A / B (B cfg-only, video CLV) │ 1986 / 1987 │
  ├───────────┼────────────────────────────────────────┼─────────────┤
  │ 5 / (6)   │ Volcanoes / spare                      │ 1986        │
  ├───────────┼────────────────────────────────────────┼─────────────┤
  │ 7 / 8     │ EcoDisc S1 / S2 (S2 cfg-only)          │ 1988 / NONE │
  ├───────────┼────────────────────────────────────────┼─────────────┤
  │ 9 / 10    │ Countryside S1 / S2                    │ 1991 / 1992 │
  ├───────────┼────────────────────────────────────────┼─────────────┤
  │ 11 / (12) │ Financial 1 / reserved                 │ 1993        │
  ├───────────┼────────────────────────────────────────┼─────────────┤
  │ 13 / (14) │ Financial 2 / reserved                 │ 1994        │
  ├───────────┼────────────────────────────────────────┼─────────────┤
  │ 15 / 16   │ Culture 1 sides 1+2                    │ 1995        │
  ├───────────┼────────────────────────────────────────┼─────────────┤
  │ 17 / (18) │ Culture 2 / reserved                   │ 1996        │
  ├───────────┼────────────────────────────────────────┼─────────────┤
  │ 19 / 20   │ BGB 1 / 2 (cfg-only)                   │ NONE        │
  └───────────┴────────────────────────────────────────┴─────────────┘


The World, the UN and you Usercode NONE
North Polar Expedition Usercode NONE

Fcodes that need supporting to boot

fcodeReadBuffer
 0x0d 0x00 0x00 0x00 0x00 0x00 fcodeClearBuffer
F-Code: Received bytes: 0x49 0x30 0x0d
F-Code: Received F-Code 0x49 = Local front panel buttons disabled
fcodeReadBuffer
 0x41 0x0d 0x00 0x00 0x00 0x00 fcodeClearBuffer
F-Code: Received bytes: 0x4a 0x30 0x0d
F-Code: Received F-Code 0x4a = Remote control disabled for player control
fcodeReadBuffer
 0x41 0x0d 0x00 0x00 0x00 0x00 fcodeClearBuffer
F-Code: Received bytes: 0x24 0x30 0x0d
F-Code: Received F-Code 0x24 = Replay switch disable

fcodeReadBuffer
 0x41 0x0d 0x00 0x00 0x00 0x00 fcodeClearBuffer
F-Code: Received bytes: 0x3f 0x55 0x0d
F-Code: Received F-Code 0x3f = User code request
<UCD>fcodeReadBuffer
 0x55 0x31 0x3d 0x30 0x36 0x36 fcodeClearBuffer
fcodeReadBuffer
 0x0d 0x31 0x3d 0x30 0x36 0x36 fcodeClearBuffer
F-Code: Received bytes: 0x56 0x50 0x33 0x0d
F-Code: Received F-Code 0x56 = Video overlay mode 3 (Hard-keyed)
fcodeReadBuffer
 0x56 0x50 0x33 0x0d 0x36 0x36 fcodeClearBuffer
F-Code: Received bytes: 0x56 0x50 0x33 0x0d
F-Code: Received F-Code 0x56 = Video overlay mode 3 (Hard-keyed)
plot 4 0 964
plot 99 1279 59
fcodeReadBuffer
 0x56 0x50 0x33 0x0d 0x36 0x36 fcodeClearBuffer
F-Code: Received bytes: 0x41 0x31 0x0d
F-Code: Received F-Code 0x41 = Audio-1 on                <- TODO ->
F-Code: Received bytes: 0x42 0x31 0x0d
F-Code: Received F-Code 0x42 = Audio-2 on               <- TODO ->
fcodeReadBuffer
 0x41 0x0d 0x33 0x0d 0x36 0x36 fcodeClearBuffer
F-Code: Received bytes: 0x46 0x35 0x31 0x52 0x0d
F-Code: Received F-Code 0x46 = Load/Goto picture number : 51 op: R = Still picture   <- TODO ->
fcodeReadBuffer
 0x41 0x30 0x0d 0x0d 0x36 0x36 fcodeClearBuffer
F-Code: Received bytes: 0x45 0x31 0x0d
F-Code: Received F-Code 0x45 = Video on
F-Code: Received bytes: 0x46 0x31 0x36 0x30 0x30 0x53 0x0d
F-Code: Received F-Code 0x46 = Load/Goto picture number : 1600 op: S = Stop Register  <- TODO ->
F-Code: Received bytes: 0x4e 0x0d
F-Code: Received F-Code 0x4e = Play forward
fcodeReadBuffer
 0x41 0x32 0x0d 0x0d 0x36 0x36 fcodeClearBuffer
fcodeReadBuffer
 0x0d 0x32 0x0d 0x0d 0x36 0x36 fcodeClearBuffer
F-Code: Received bytes: 0x41 0x30 0x0d
F-Code: Received F-Code 0x41 = Audio-1 off
F-Code: Received bytes: 0x42 0x30 0x0d
F-Code: Received F-Code 0x42 = Audio-2 off

fcodeReadBuffer
 0x0d 0x32 0x0d 0x0d 0x36 0x36 fcodeClearBuffer
F-Code: Received bytes: 0x46 0x31 0x38 0x37 0x39 0x32 0x52 0x0d
F-Code: Received F-Code 0x46 = Load/Goto picture number : 18792 op: R = Still picture  <- TODO ->
fcodeReadBuffer
 0x41 0x30 0x0d 0x0d 0x36 0x36 fcodeClearBuffer
fcodeReadBuffer
 0x0d 0x30 0x0d 0x0d 0x36 0x36 fcodeClearBuffer
F-Code: Received bytes: 0x45 0x31 0x0d
F-Code: Received F-Code 0x45 = Video on

fcodeReadBuffer
 0x0d 0x30 0x0d 0x0d 0x36 0x36 fcodeClearBuffer
F-Code: Received bytes: 0x46 0x31 0x38 0x37 0x39 0x31 0x52 0x0d
F-Code: Received F-Code 0x46 = Load/Goto picture number : 18791 op: R = Still picture
