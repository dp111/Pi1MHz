   10 REM ECOTEST - Pi1MHz AUNFS bring-up test (BBC BASIC)
   20 REM Drives the econet command interface at FRED &FCA6-&FCAA
   30 REM directly, so it works with or without the AUNFS ROM fitted.
   40 REM Run each step and report PASS/FAIL. See econet_design.md.
   50 :
   60 PASS%=0:FAIL%=0
   70 PRINT "Pi1MHz AUNFS bring-up test"
   80 PRINT STRING$(34,"-")
   90 :
  100 REM --- step 1: INIT (cmd 30), station from Pi config, IRQ+imm off
  110 REM     (flags 0 here: ECOTEST polls; the ROM uses flags 3)
  120 PROCblk(&E8):PROCw(30):FOR I%=1 TO 8:PROCw(0):NEXT
  130 R%=FNcmd(&E8)
  140 IF R%=6 PRINT "INIT: Pi network not ready - is WiFi up?":END
  150 PROCchk("INIT",R%=0)
  160 :
  170 REM --- step 2: STATUS (cmd 31) - read our station and IP
  180 PROCblk(&E8):PROCw(31)
  190 R%=FNcmd(&E8)
  200 PROCjim(&FFE804):S%=?&FCA9:N%=?&FCA9
  210 PROCjim(&FFE808):I1%=?&FCA9:I2%=?&FCA9:I3%=?&FCA9:I4%=?&FCA9
  220 PRINT "  station ";N%;".";S%;"  ip ";I1%;".";I2%;".";I3%;".";I4%
  230 PROCchk("STATUS",R%=0 AND S%<>0)
  240 :
  250 REM --- step 3: loopback test responder on (cmd 40, station 0.99)
  260 PROCblk(&E8):PROCw(40):PROCw(1):PROCw(99):PROCw(0)
  270 PROCchk("TEST responder",FNcmd(&E8)=0)
  280 :
  290 REM --- step 4: open wildcard receive (cmd 34) handle 0,
  300 REM     buffer at JIM &FD8000 size &2000
  310 PROCblk(&E8):PROCw(34):PROCw(0):PROCw(0):PROCw(0)
  320 PROCw(&FF):PROCw(&FF):PROCw(0):PROCw(0)
  330 PROCw(&00):PROCw(&80):PROCw(&FD):PROCw(0)
  340 PROCw(&00):PROCw(&20):PROCw(0):PROCw(0)
  350 PROCchk("RX_OPEN",FNcmd(&E8)=0)
  360 :
  370 REM --- step 5: write test payload to JIM &FD0000
  380 M$="PI1MHZ-AUNFS-OK"
  390 PROCjim(&FD0000):FOR I%=1 TO LEN(M$):?&FCA9=ASC(MID$(M$,I%,1)):NEXT
  400 :
  410 REM --- step 6: transmit to the loopback station (cmd 32)
  420 PROCblk(&E8):PROCw(32):PROCw(0):PROCw(&85):PROCw(&77)
  430 PROCw(99):PROCw(0):PROCw(0):PROCw(0)
  440 PROCw(&00):PROCw(&00):PROCw(&FD):PROCw(0)
  450 PROCw(LEN(M$)):PROCw(0):PROCw(0):PROCw(0)
  460 PROCchk("TX accepted",FNcmd(&E8)=0)
  470 :
  480 REM --- step 7: poll transmit completion (cmd 33)
  490 T%=TIME:REPEAT:PROCblk(&E9):PROCw(33):R%=FNcmd(&E9)
  500 UNTIL R%<>&80 OR TIME-T%>200
  510 PROCchk("TX completed",R%=0)
  520 :
  530 REM --- step 8: the frame comes back via the loopback (cmd 35)
  540 PROCblk(&E8):PROCw(35):PROCw(0)
  550 R%=FNcmd(&E8)
  560 PROCjim(&FFE803):P%=?&FCA9:SS%=?&FCA9
  570 PROCjim(&FFE80C):L%=?&FCA9
  580 PROCchk("RX frame (port/src/len)",R%=0 AND P%=&77 AND SS%=99 AND L%=LEN(M$))
  590 PROCjim(&FD8000):T$=""
  600 FOR I%=1 TO L%:T$=T$+CHR$(?&FCA9):NEXT
  610 PROCchk("RX payload intact",T$=M$)
  620 :
  630 REM --- step 9: release the frame (cmd 42, verdict 0 = delivered)
  640 PROCblk(&E8):PROCw(42):PROCw(0):PROCw(0)
  650 PROCchk("RX_DONE",FNcmd(&E8)=0)
  660 :
  670 REM --- step 10: machine peek immediate to the loopback (cmd 38)
  680 PROCblk(&E8):PROCw(38):PROCw(0):PROCw(&88):PROCw(0)
  690 PROCw(99):PROCw(0):PROCw(0):PROCw(0)
  700 PROCw(0):PROCw(0):PROCw(&FD):PROCw(0)
  710 PROCw(0):PROCw(0):PROCw(0):PROCw(0)
  720 PROCw(&00):PROCw(&A0):PROCw(&FD):PROCw(0)
  730 PROCw(8):PROCw(0):PROCw(0):PROCw(0)
  740 R%=FNcmd(&E8)
  750 PROCblk(&E9):PROCw(33):R2%=FNcmd(&E9)
  760 PROCjim(&FFE908):RL%=?&FCA9
  770 PROCjim(&FDA000):M1%=?&FCA9:M2%=?&FCA9
  780 PRINT "  machine type &";~(M2%*256+M1%)
  790 PROCchk("Machine peek",R%=0 AND R2%=0 AND RL%=4)
  800 :
  810 REM --- summary
  820 PRINT STRING$(34,"-")
  830 PRINT "PASS ";PASS%;"  FAIL ";FAIL%
  840 IF FAIL%=0 PRINT "All good - try a real station next."
  850 END
  860 :
  870 DEF PROCjim(A%):?&FCA6=A% AND &FF:?&FCA7=(A% DIV 256) AND &FF:?&FCA8=(A% DIV 65536) AND &FF:ENDPROC
  880 DEF PROCblk(P%):PROCjim(&FF0000+P%*256):ENDPROC
  890 DEF PROCw(B%):?&FCA9=B%:ENDPROC
  900 DEF FNcmd(P%):?&FCAA=P%:REPEAT UNTIL ?&FCAA<&E0:=?&FCAA
  910 DEF PROCchk(N$,OK%):IF OK% PASS%=PASS%+1:PRINT "PASS ";N$ ELSE FAIL%=FAIL%+1:PRINT "FAIL ";N$
  920 ENDPROC
