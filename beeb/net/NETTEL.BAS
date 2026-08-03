   10 REM ---------------------------------------------------------------
   20 REM  Pi1MHz N: device - TELNET demo: connect to a telnet server and
   30 REM  print the banner.  The net service strips IAC option-negotiation
   40 REM  and answers it, so the Beeb sees clean text.  Needs net_enable=1.
   50 REM  See NETHTTP.BAS for the URL-verb library this reuses.
   60 REM ---------------------------------------------------------------
   70 net=&FCA6 : cmd=net+4 : dat=net+3
   80 nOK=0 : nEOF=&20 : nBUSY=&80 : nPEND=1
   90 rxb=&010000
  100 :
  110 url$="TELNET://192.168.0.89:2323/" : H=6
  120 :
  130 MODE 7 : PRINT "Pi1MHz TELNET demo" : PRINT
  140 PRINT "connecting to ";url$
  150 IF FNurl_open(H,url$)<>nOK THEN PRINT "connect failed" : END
  160 PRINT "--- banner ---"
  170 REPEAT
  180   R=FNurl_read(H,rxb,200)
  190   IF nlen>0 THEN PROCaddr(rxb):FOR I=1 TO nlen:VDU ?dat:NEXT
  200 UNTIL R>=nEOF
  210 PRINT : PRINT "--- end (code &";~R;") ---"
  220 PROCurl_close(H)
  230 END
  240 :
  250 REM ===== N: device library =====================================
  260 DEF PROCaddr(A) : ?net=A AND &FF : ?(net+1)=(A DIV &100)AND &FF : ?(net+2)=(A DIV &10000)AND &FF : ENDPROC
  270 DEF FNblk(N)=&FFF000+N*&100
  280 DEF PROCwr(V) : ?dat=V AND &FF : ENDPROC
  290 DEF PROCwr24(V) : PROCwr(V):PROCwr(V DIV &100):PROCwr(V DIV &10000):ENDPROC
  300 DEF PROCwr32(V) : PROCwr24(V):PROCwr(V DIV &1000000):ENDPROC
  310 DEF FNrd24 : LOCAL l,m,h : l=?dat:m=?dat:h=?dat : =l+m*256+h*65536
  320 DEF PROCcmdhdr(N,C) : PROCaddr(FNblk(N)):PROCwr(C) : ENDPROC
  330 DEF FNurl_go(N) : LOCAL R : REPEAT ?cmd=&F0+N : REPEAT R=?cmd : UNTIL(R AND nBUSY)=0 : UNTIL R<>nPEND : =R
  340 :
  350 DEF FNurl_open(N,U$)
  360 LOCAL I : PROCaddr(FNblk(N)):PROCwr(60):PROCwr(0)
  370 FOR I=1 TO LEN(U$):PROCwr(ASC(MID$(U$,I,1))):NEXT:PROCwr(0)
  380 =FNurl_go(N)
  390 :
  400 DEF FNurl_read(N,B,MX)
  410 LOCAL R : PROCcmdhdr(N,61):PROCwr24(MX):PROCwr32(B)
  420 R=FNurl_go(N):PROCaddr(FNblk(N)+1):nlen=FNrd24
  430 =R
  440 :
  450 DEF PROCurl_close(N) : PROCcmdhdr(N,63):R=FNurl_go(N):ENDPROC
