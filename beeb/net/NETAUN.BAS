   10 REM ---------------------------------------------------------------
   20 REM  Pi1MHz AUN test suite - Econet immediate operations over AUN.
   30 REM  Run it on a Beeb with an AUNFS ROM fitted, after logging in to
   40 REM  the fileserver (*I AM SYST).  Everything here goes through the
   50 REM  ROM's OSWORD &10 transmit path, so it tests the ROM AND the
   60 REM  firmware AUN engine together, the way real commands do.
   70 REM
   80 REM  The 4-way immediates (POKE &82, JSR &83, USRPROC &84, OSPROC
   90 REM  &85) are what *NOTIFY / *REMOTE / *VIEW are built on.  AUN
  100 REM  cannot express a 4-way, so the firmware carries them as DATA
  110 REM  datagrams to port 0 with the scout extras first in the payload
  120 REM  - the PiEconetBridge / BeebEm convention.  See
  130 REM  src/AUN/aun_design.md.
  140 REM ---------------------------------------------------------------
  150 stn=254 : net=0 : REM <-- the peer to test against (your bridge/FS)
  160 :
  170 MODE 7 : PRINT "Pi1MHz AUN test suite" : PRINT STRING$(21,"-")
  180 PRINT "peer ";net;".";stn : PRINT
  190 pass=0 : fail=0
  200 DIM cb% 20, buf% 260, rep% 260
  210 :
  220 REM --- 1: machine peek (&88).  A two-way immediate: it must still
  230 REM     go as a type-5 AUN immediate, and the peer answers with a
  240 REM     4-byte machine id.  This is the control case for the 4-way
  250 REM     work - if this breaks, the re-typing leaked too far.
  260 PRINT "1 machine peek (&88)";
  270 R=FNimm(&88,0,0,0,0)
  280 IF R<>0 THEN PROCno("tx err &"+STR$~R) ELSE PROCyes("id "+FNhex(rep%?0)+FNhex(rep%?1)+" v"+FNhex(rep%?3)+"."+FNhex(rep%?2))
  290 :
  300 REM --- 2: *NOTIFY, character by character (OSPROC &85, subcode 0).
  310 REM     Each character is one 4-way immediate: args [0,0,ch,&0F]
  320 REM     then the character again as the data phase.  A bridge
  330 REM     assembles them into a string and shows it on the target.
  340 PRINT "2 notify ""HI"" (&85)";
  350 ok=TRUE
  360 FOR I=1 TO 2
  370   C=ASC(MID$("HI",I,1))
  380   ?buf%=C
  390   R=FNimm4(&85,0,0,C,&0F,buf%,1)
  400   IF R<>0 THEN ok=FALSE
  410 NEXT
  420 IF ok THEN PROCyes("both chars sent") ELSE PROCno("tx err &"+STR$~R)
  430 :
  440 REM --- 3: remote peek (&81).  Two-way: reads memory from the peer.
  450 REM     Asks for 4 bytes of the peer's zero page.
  460 PRINT "3 remote peek (&81)";
  470 R=FNpeek(&0000,4)
  480 IF R<>0 THEN PROCno("tx err &"+STR$~R) ELSE PROCyes(FNhex(rep%?0)+" "+FNhex(rep%?1)+" "+FNhex(rep%?2)+" "+FNhex(rep%?3))
  490 :
  500 REM --- 4: remote poke (&82).  A 4-way: [start][count] scout extras
  510 REM     then the data.  Writes 2 bytes into the peer's screen RAM.
  520 REM     Firmware splices the byte count in; the ROM only supplies
  530 REM     the start address and the data.
  540 PRINT "4 remote poke (&82)";
  550 buf%?0=ASC"P" : buf%?1=ASC"i"
  560 R=FNimm4(&82,&00,&7C,&00,&00,buf%,2)
  570 IF R<>0 THEN PROCno("tx err &"+STR$~R) ELSE PROCyes("2 bytes written")
  580 :
  590 REM --- 5: unreachable station must FAIL, not hang.  Station 1 on
  600 REM     our net should not answer; the ROM's poll budget is ~1s, so
  610 REM     this returns an error quickly rather than wedging.
  620 PRINT "5 dead station fails fast";
  630 T=TIME
  640 R=FNimmto(&88,1,net,0,0,0,0)
  650 T=(TIME-T)/100
  660 IF R=0 THEN PROCno("unexpected reply") ELSE IF T>5 THEN PROCno("took "+STR$T+"s") ELSE PROCyes("err &"+STR$~R+" in "+STR$T+"s")
  670 :
  680 PRINT : PRINT "passed ";pass;"  failed ";fail
  690 IF fail=0 THEN PRINT "ALL TESTS PASSED"
  700 END
  710 :
  720 REM ===== helpers ================================================
  730 REM TXCB: +0 ctrl, +1 port, +2 stn, +3 net, +4..7 buffer start,
  740 REM +8..11 buffer end, +12..15 the four immediate argument bytes.
  750 REM Bytes 6/7 and 10/11 = &FF&FF select I/O processor memory.
  760 DEF FNimm(c,a0,a1,a2,a3)=FNimmto(c,stn,net,a0,a1,a2,a3)
  770 :
  780 DEF FNimmto(c,s,n,a0,a1,a2,a3)
  790 cb%?0=c : cb%?1=0 : cb%?2=s : cb%?3=n
  800 cb%!4=rep% : cb%?6=&FF : cb%?7=&FF
  810 cb%!8=rep%+4 : cb%?10=&FF : cb%?11=&FF
  820 cb%?12=a0 : cb%?13=a1 : cb%?14=a2 : cb%?15=a3
  830 A%=&10 : X%=cb% MOD 256 : Y%=cb% DIV 256
  840 CALL &FFF1
  850 =cb%?0
  860 :
  870 REM A 4-way immediate carrying a data block: same TXCB, but the
  880 REM buffer holds the data phase rather than a reply.
  890 DEF FNimm4(c,a0,a1,a2,a3,b,l)
  900 cb%?0=c : cb%?1=0 : cb%?2=stn : cb%?3=net
  910 cb%!4=b : cb%?6=&FF : cb%?7=&FF
  920 cb%!8=b+l : cb%?10=&FF : cb%?11=&FF
  930 cb%?12=a0 : cb%?13=a1 : cb%?14=a2 : cb%?15=a3
  940 A%=&10 : X%=cb% MOD 256 : Y%=cb% DIV 256
  950 CALL &FFF1
  960 =cb%?0
  970 :
  980 REM Remote peek: the four argument bytes are the remote start
  990 REM address; the reply buffer size sets how much is read back.
 1000 DEF FNpeek(ad,l)
 1010 cb%?0=&81 : cb%?1=0 : cb%?2=stn : cb%?3=net
 1020 cb%!4=rep% : cb%?6=&FF : cb%?7=&FF
 1030 cb%!8=rep%+l : cb%?10=&FF : cb%?11=&FF
 1040 cb%?12=ad AND &FF : cb%?13=(ad DIV 256)AND &FF : cb%?14=0 : cb%?15=0
 1050 A%=&10 : X%=cb% MOD 256 : Y%=cb% DIV 256
 1060 CALL &FFF1
 1070 =cb%?0
 1080 :
 1090 DEF PROCyes(m$) : PRINT TAB(24);"ok  ";m$ : pass=pass+1 : ENDPROC
 1100 DEF PROCno(m$) : PRINT TAB(24);"FAIL ";m$ : fail=fail+1 : ENDPROC
 1110 DEF FNhex(v)=RIGHT$("0"+STR$~v,2)
