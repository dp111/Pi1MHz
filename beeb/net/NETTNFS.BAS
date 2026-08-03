   10 REM ---------------------------------------------------------------
   20 REM  Pi1MHz N: device - TNFS demo: list a share, then read a file.
   30 REM  Uses the URL verbs (60-64): a trailing '/' lists a directory
   40 REM  (one entry name per read), else it reads the file's bytes.
   50 REM  Needs net_enable=1 and a TNFS server.  See NETHTTP.BAS.
   60 REM ---------------------------------------------------------------
   70 net=&FCA6 : cmd=net+4 : dat=net+3
   80 nOK=0 : nEOF=&20 : nBUSY=&80 : nPEND=1
   90 rxb=&010000
  100 :
  110 REM ===== edit these ============================================
  120 dir$="TNFS://192.168.0.89/" : file$="TNFS://192.168.0.89/HELLO.TXT"
  130 H=6
  140 :
  150 MODE 7 : PRINT "Pi1MHz TNFS demo" : PRINT
  160 PRINT "--- dir ";dir$;" ---"
  170 IF FNurl_open(H,dir$)<>nOK THEN PRINT "open dir failed" : END
  180 REPEAT
  190   R=FNurl_read(H,rxb,64)
  200   IF nlen>0 THEN PROCaddr(rxb):FOR I=1 TO nlen:VDU ?dat:NEXT:PRINT
  210 UNTIL R>=nEOF
  220 PROCurl_close(H)
  230 PRINT : PRINT "--- read ";file$;" ---"
  240 IF FNurl_open(H,file$)<>nOK THEN PRINT "open file failed" : END
  250 REPEAT
  260   R=FNurl_read(H,rxb,200)
  270   IF nlen>0 THEN PROCaddr(rxb):FOR I=1 TO nlen:VDU ?dat:NEXT
  280 UNTIL R>=nEOF
  290 PRINT : PRINT "--- end (code &";~R;") ---"
  300 PROCurl_close(H)
  310 END
  320 :
  330 REM ===== N: device library =====================================
  340 DEF PROCaddr(A) : ?net=A AND &FF : ?(net+1)=(A DIV &100)AND &FF : ?(net+2)=(A DIV &10000)AND &FF : ENDPROC
  350 DEF FNblk(N)=&FFF000+N*&100
  360 DEF PROCwr(V) : ?dat=V AND &FF : ENDPROC
  370 DEF PROCwr24(V) : PROCwr(V):PROCwr(V DIV &100):PROCwr(V DIV &10000):ENDPROC
  380 DEF PROCwr32(V) : PROCwr24(V):PROCwr(V DIV &1000000):ENDPROC
  390 DEF FNrd24 : LOCAL l,m,h : l=?dat:m=?dat:h=?dat : =l+m*256+h*65536
  400 REM dispatch handle N, spin the transient NET_BUSY, re-issue while PENDING
  410 DEF FNurl_go(N) : LOCAL R : REPEAT ?cmd=&F0+N : REPEAT R=?cmd : UNTIL(R AND nBUSY)=0 : UNTIL R<>nPEND : =R
  420 :
  430 DEF FNurl_open(N,U$)
  440 LOCAL I : PROCaddr(FNblk(N)):PROCwr(60):PROCwr(0)
  450 FOR I=1 TO LEN(U$):PROCwr(ASC(MID$(U$,I,1))):NEXT:PROCwr(0)
  460 =FNurl_go(N)
  470 :
  480 DEF FNurl_read(N,B,MX)
  490 LOCAL R : PROCaddr(FNblk(N)):PROCwr(61):PROCwr24(MX):PROCwr32(B)
  500 R=FNurl_go(N):PROCaddr(FNblk(N)+1):nlen=FNrd24
  510 =R
  520 :
  530 DEF PROCurl_close(N) : PROCaddr(FNblk(N)):PROCwr(63):R=FNurl_go(N):ENDPROC
