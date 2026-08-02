   10 REM ---------------------------------------------------------------
   20 REM  Pi1MHz IP/net service - BBC BASIC client library + HTTP GET demo
   30 REM  Talks to the &FCA6 services port (commands 45-56).  Needs
   40 REM  net_enable=1 in Pi1MHz.cfg.  See docs/dev/network-service-*.md.
   50 REM ---------------------------------------------------------------
   60 net=&FCA6 : cmd=net+4 : dat=net+3
   70 REM  result codes (net_service.h)
   80 nOK=0 : nPEND=1 : nEOF=&20 : nBUSY=&80
   90 REM  a data buffer well below the command blocks (at &FFFn00)
  100 txb=&000000 : rxb=&010000
  110 :
  120 REM ===== edit these for your target ==============================
  130 host$="93.184.216.34" : REM example.com (use PROCdns for a name)
  140 port=80 : path$="/"
  150 H=0 : REM socket handle (0-15)
  160 :
  170 MODE 7 : PRINT "Pi1MHz net demo": PRINT
  180 IF FNopen(H,0)<>nOK THEN PRINT "open failed":END
  190 PRINT "connecting to ";host$;":";port
  200 R=FNconnect_str(H,host$,port)
  210 IF R<>nOK THEN PRINT "connect failed, code &";~R : PROCclose(H):END
  220 PRINT "connected."
  230 req$="GET "+path$+" HTTP/1.0"+CHR$13+CHR$10
  240 req$=req$+"Host: "+host$+CHR$13+CHR$10+"Connection: close"+CHR$13+CHR$10+CHR$13+CHR$10
  250 IF FNsend(H,req$)<0 THEN PRINT "send failed":PROCclose(H):END
  260 PRINT "--- reply ---"
  270 REPEAT
  280   R=FNrecv(H,rxb,200)
  290   IF nlen>0 THEN PROCaddr(rxb):FOR I=1 TO nlen:VDU ?dat:NEXT
  300 UNTIL R>=nEOF : REM &20=EOF or any &21+ error
  310 PRINT : PRINT "--- end (code &";~R;") ---"
  320 PROCclose(H)
  330 END
  340 :
  350 REM ===== client library =========================================
  360 DEF PROCaddr(A) : ?net=A AND &FF : ?(net+1)=(A DIV &100)AND &FF : ?(net+2)=(A DIV &10000)AND &FF : ENDPROC
  370 DEF FNblk(N)=&FFF000+N*&100
  380 DEF PROCwr(V) : ?dat=V AND &FF : ENDPROC
  390 DEF PROCwr24(V) : PROCwr(V):PROCwr(V DIV &100):PROCwr(V DIV &10000):ENDPROC
  400 DEF PROCwr32(V) : PROCwr24(V):PROCwr(V DIV &1000000):ENDPROC
  410 DEF PROCcmdhdr(N,C) : PROCaddr(FNblk(N)):PROCwr(C):ENDPROC
  420 :
  430 REM  dispatch handle N and spin out the transient NET_BUSY
  440 DEF FNgo(N) : LOCAL R : ?cmd=&F0+N : REPEAT R=?cmd : UNTIL (R AND nBUSY)=0 : =R
  450 REM  re-issue an async command until it settles (or ESCAPE)
  460 DEF FNgo_wait(N) : LOCAL R : REPEAT R=FNgo(N) : UNTIL R<>nPEND : =R
  470 :
  480 DEF FNopen(N,T) : PROCcmdhdr(N,45):PROCwr(T):=FNgo_wait(N)
  490 DEF PROCclose(N) : PROCcmdhdr(N,53):R=FNgo_wait(N):ENDPROC
  500 :
  510 REM  connect to "a.b.c.d" : returns result code
  520 DEF FNconnect_str(N,IP$,P)
  530 LOCAL a,b,c,d,p1
  540 p1=INSTR(IP$,".") : a=VAL(LEFT$(IP$,p1-1)) : IP$=MID$(IP$,p1+1)
  550 p1=INSTR(IP$,".") : b=VAL(LEFT$(IP$,p1-1)) : IP$=MID$(IP$,p1+1)
  560 p1=INSTR(IP$,".") : c=VAL(LEFT$(IP$,p1-1)) : d=VAL(MID$(IP$,p1+1))
  570 PROCcmdhdr(N,47):PROCwr(a):PROCwr(b):PROCwr(c):PROCwr(d)
  580 PROCwr(P AND &FF):PROCwr(P DIV &100)
  590 =FNgo_wait(N)
  600 :
  610 REM  resolve a hostname on handle N, writing a.b.c.d into IP$ (byref via I0..3)
  620 DEF FNdns(N,HOST$)
  630 LOCAL R,I : PROCcmdhdr(N,46)
  640 FOR I=1 TO LEN(HOST$):PROCwr(ASC(MID$(HOST$,I,1))):NEXT:PROCwr(0)
  650 R=FNgo_wait(N)
  660 IF R=nOK THEN PROCaddr(FNblk(N)+4):dns0=?dat:dns1=?dat:dns2=?dat:dns3=?dat
  670 =R
  680 :
  690 REM  read a 24-bit LE value from the data port (3 auto-inc reads)
  700 DEF FNrd24 : LOCAL l,m,h : l=?dat:m=?dat:h=?dat : =l+m*256+h*65536
  710 :
  720 REM  send string S$ ; returns bytes queued, or -1 on error
  730 DEF FNsend(N,S$)
  740 LOCAL R,I : PROCaddr(txb):FOR I=1 TO LEN(S$):PROCwr(ASC(MID$(S$,I,1))):NEXT
  750 PROCcmdhdr(N,50):PROCwr24(LEN(S$)):PROCwr32(txb)
  760 R=FNgo_wait(N)
  770 IF R<>nOK THEN =-1
  780 PROCaddr(FNblk(N)+1):=FNrd24
  790 :
  800 REM  recv up to MX bytes into buffer B ; sets nlen ; returns result
  810 DEF FNrecv(N,B,MX)
  820 LOCAL R : PROCcmdhdr(N,51):PROCwr24(MX):PROCwr32(B)
  830 R=FNgo_wait(N)
  840 PROCaddr(FNblk(N)+1):nlen=FNrd24
  850 =R
