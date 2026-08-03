   10 REM ---------------------------------------------------------------
   20 REM  Pi1MHz net service - TCP server demo (bind/listen/accept, cmd 49).
   30 REM  Binds a port, waits for one inbound connection, then prints what
   40 REM  the client sends until it closes.  Needs net_enable=1.  ESCAPE to
   50 REM  give up waiting.  See NETDEMO.BAS for the outbound (client) side.
   60 REM ---------------------------------------------------------------
   70 net=&FCA6 : cmd=net+4 : dat=net+3
   80 nOK=0 : nPEND=1 : nEOF=&20 : nBUSY=&80
   90 rxb=&010000
  100 :
  110 REM ===== edit these ============================================
  120 port=8000 : H=0
  130 :
  140 MODE 7 : PRINT "Pi1MHz TCP server demo" : PRINT
  150 IF FNopen(H,0)<>nOK THEN PRINT "open failed" : END
  160 IF FNbind(H,port)<>nOK THEN PRINT "bind failed" : PROCclose(H) : END
  170 PRINT "listening on port ";port;" ..."
  180 A=FNlisten(H)
  190 PRINT "accepted -> handle ";A
  200 PRINT "--- received ---"
  210 REPEAT
  220   R=FNrecv(A,rxb,200)
  230   IF nlen>0 THEN PROCaddr(rxb):FOR I=1 TO nlen:VDU ?dat:NEXT
  240 UNTIL R>=nEOF
  250 PRINT : PRINT "--- end (code &";~R;") ---"
  260 PROCclose(A) : PROCclose(H)
  270 END
  280 :
  290 REM ===== library ================================================
  300 DEF PROCaddr(A) : ?net=A AND &FF : ?(net+1)=(A DIV &100)AND &FF : ?(net+2)=(A DIV &10000)AND &FF : ENDPROC
  310 DEF FNblk(N)=&FFF000+N*&100
  320 DEF PROCwr(V) : ?dat=V AND &FF : ENDPROC
  330 DEF PROCwr24(V) : PROCwr(V):PROCwr(V DIV &100):PROCwr(V DIV &10000):ENDPROC
  340 DEF PROCwr32(V) : PROCwr24(V):PROCwr(V DIV &1000000):ENDPROC
  350 DEF FNrd24 : LOCAL l,m,h : l=?dat:m=?dat:h=?dat : =l+m*256+h*65536
  360 DEF PROCcmdhdr(N,C) : PROCaddr(FNblk(N)):PROCwr(C) : ENDPROC
  370 DEF FNgo(N) : LOCAL R : ?cmd=&F0+N : REPEAT R=?cmd : UNTIL (R AND nBUSY)=0 : =R
  380 DEF FNgo_wait(N) : LOCAL R : REPEAT R=FNgo(N) : UNTIL R<>nPEND : =R
  390 :
  400 DEF FNopen(N,T) : PROCcmdhdr(N,45):PROCwr(T) : =FNgo(N)
  410 DEF FNbind(N,P) : PROCcmdhdr(N,48):PROCwr(P AND &FF):PROCwr(P DIV &100) : =FNgo(N)
  420 DEF PROCclose(N) : PROCcmdhdr(N,53):R=FNgo_wait(N) : ENDPROC
  430 :
  440 REM  wait for one connection on listener N ; returns the accepted handle
  450 DEF FNlisten(N) : LOCAL R : PROCcmdhdr(N,49):R=FNgo_wait(N) : PROCaddr(FNblk(N)+1) : =?dat
  460 :
  470 REM  recv up to MX bytes from handle N into B ; sets nlen ; returns result
  480 DEF FNrecv(N,B,MX) : LOCAL R : PROCcmdhdr(N,51):PROCwr24(MX):PROCwr32(B)
  490 R=FNgo(N):PROCaddr(FNblk(N)+1):nlen=FNrd24 : =R
