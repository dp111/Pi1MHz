   10 REM ---------------------------------------------------------------
   20 REM  Pi1MHz N: device - TNFS WRITE demo: create a file on a TNFS
   30 REM  share (url_open with the write bit, url_write), then read it
   40 REM  back to prove the round-trip.  Needs net_enable=1 + a TNFS
   50 REM  server that accepts writes.  See NETTNFS.BAS for read/list.
   60 REM ---------------------------------------------------------------
   70 net=&FCA6 : cmd=net+4 : dat=net+3
   80 nOK=0 : nEOF=&20 : nBUSY=&80 : nPEND=1
   90 txb=&000000 : rxb=&010000
  100 :
  110 file$="TNFS://192.168.0.89/SAVED.TXT" : H=6
  120 msg$="Written from the BBC over TNFS!"
  130 :
  140 MODE 7 : PRINT "Pi1MHz TNFS write demo" : PRINT
  150 PRINT "writing ";file$
  160 IF FNurl_open_m(H,file$,8)<>nOK THEN PRINT "open(write) failed" : END
  170 PRINT "wrote ";FNurl_write(H,msg$);" bytes"
  180 PROCurl_close(H)
  190 PRINT : PRINT "reading it back:"
  200 IF FNurl_open_m(H,file$,0)<>nOK THEN PRINT "open(read) failed" : END
  210 REPEAT
  220   R=FNurl_read(H,rxb,200)
  230   IF nlen>0 THEN PROCaddr(rxb):FOR I=1 TO nlen:VDU ?dat:NEXT
  240 UNTIL R>=nEOF
  250 PRINT : PRINT "--- end (code &";~R;") ---"
  260 PROCurl_close(H)
  270 END
  280 :
  290 REM ===== N: device library =====================================
  300 DEF PROCaddr(A) : ?net=A AND &FF : ?(net+1)=(A DIV &100)AND &FF : ?(net+2)=(A DIV &10000)AND &FF : ENDPROC
  310 DEF FNblk(N)=&FFF000+N*&100
  320 DEF PROCwr(V) : ?dat=V AND &FF : ENDPROC
  330 DEF PROCwr24(V) : PROCwr(V):PROCwr(V DIV &100):PROCwr(V DIV &10000):ENDPROC
  340 DEF PROCwr32(V) : PROCwr24(V):PROCwr(V DIV &1000000):ENDPROC
  350 DEF FNrd24 : LOCAL l,m,h : l=?dat:m=?dat:h=?dat : =l+m*256+h*65536
  360 DEF PROCcmdhdr(N,C) : PROCaddr(FNblk(N)):PROCwr(C) : ENDPROC
  370 DEF FNurl_go(N) : LOCAL R : REPEAT ?cmd=&F0+N : REPEAT R=?cmd : UNTIL(R AND nBUSY)=0 : UNTIL R<>nPEND : =R
  380 :
  390 REM  open URL$ on handle N with open-mode M (bit 3 = write)
  400 DEF FNurl_open_m(N,U$,M)
  410 LOCAL I : PROCaddr(FNblk(N)):PROCwr(60):PROCwr(M)
  420 FOR I=1 TO LEN(U$):PROCwr(ASC(MID$(U$,I,1))):NEXT:PROCwr(0)
  430 =FNurl_go(N)
  440 :
  450 REM  write S$ ; returns bytes the server accepted
  460 DEF FNurl_write(N,S$)
  470 LOCAL R,I : PROCaddr(txb):FOR I=1 TO LEN(S$):PROCwr(ASC(MID$(S$,I,1))):NEXT
  480 PROCcmdhdr(N,62):PROCwr24(LEN(S$)):PROCwr32(txb)
  490 R=FNurl_go(N):PROCaddr(FNblk(N)+1):=FNrd24
  500 :
  510 DEF FNurl_read(N,B,MX)
  520 LOCAL R : PROCcmdhdr(N,61):PROCwr24(MX):PROCwr32(B)
  530 R=FNurl_go(N):PROCaddr(FNblk(N)+1):nlen=FNrd24
  540 =R
  550 :
  560 DEF PROCurl_close(N) : PROCcmdhdr(N,63):R=FNurl_go(N):ENDPROC
