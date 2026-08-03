   10 REM ---------------------------------------------------------------
   20 REM  Pi1MHz N: device - HTTP GET a URL and print the body.
   30 REM  Uses the net service's URL verbs (commands 60-64): open a URL
   40 REM  like a file, then read.  The HTTP adapter sends the request and
   50 REM  strips the response headers, so the Beeb sees only the body.
   60 REM  Needs net_enable=1 in Pi1MHz.cfg.  See NETDEMO.BAS for raw sockets.
   70 REM ---------------------------------------------------------------
   80 net=&FCA6 : cmd=net+4 : dat=net+3
   90 nEOF=&20 : nBUSY=&80 : nPEND=1
  100 rxb=&010000 : REM a JIM read buffer, clear of the command pages
  110 :
  120 REM ===== edit these ============================================
  130 url$="HTTP://192.168.0.1/" : H=0
  140 :
  150 MODE 7 : PRINT "Pi1MHz N: HTTP demo" : PRINT
  160 R=FNurl_open(H,url$)
  170 IF R<>0 THEN PRINT "open failed, code &";~R : END
  180 REM  the HTTP status code is only known after the first read parses
  185 REM  the response headers, so print it after the body loop, not before
  190 PRINT "--- body ---"
  200 REPEAT
  210   R=FNurl_read(H,rxb,200)
  220   IF nlen>0 THEN PROCaddr(rxb):FOR I=1 TO nlen:VDU ?dat:NEXT
  230 UNTIL R>=nEOF
  235 PRINT : PRINT "HTTP status ";FNurl_httpcode(H)
  240 PRINT "--- end (code &";~R;") ---"
  250 PROCurl_close(H)
  260 END
  270 :
  280 REM ===== N: device library =====================================
  290 DEF PROCaddr(A) : ?net=A AND &FF : ?(net+1)=(A DIV &100)AND &FF : ?(net+2)=(A DIV &10000)AND &FF : ENDPROC
  300 DEF FNblk(N)=&FFF000+N*&100
  310 DEF PROCwr(V) : ?dat=V AND &FF : ENDPROC
  320 DEF PROCwr24(V) : PROCwr(V):PROCwr(V DIV &100):PROCwr(V DIV &10000):ENDPROC
  330 DEF PROCwr32(V) : PROCwr24(V):PROCwr(V DIV &1000000):ENDPROC
  340 DEF FNrd24 : LOCAL l,m,h : l=?dat:m=?dat:h=?dat : =l+m*256+h*65536
  350 REM dispatch handle N, spin out the transient NET_BUSY, re-issue while PENDING
  360 DEF FNurl_go(N) : LOCAL R : REPEAT ?cmd=&F0+N : REPEAT R=?cmd : UNTIL(R AND nBUSY)=0 : UNTIL R<>nPEND : =R
  370 :
  380 REM  open URL$ on handle N (async: resolves + connects + sends the request)
  390 DEF FNurl_open(N,U$)
  400 LOCAL I : PROCaddr(FNblk(N)):PROCwr(60):PROCwr(0)
  410 FOR I=1 TO LEN(U$):PROCwr(ASC(MID$(U$,I,1))):NEXT:PROCwr(0)
  420 =FNurl_go(N)
  430 :
  440 REM  read up to MX body bytes into buffer B ; sets nlen ; returns result
  450 DEF FNurl_read(N,B,MX)
  460 LOCAL R : PROCaddr(FNblk(N)):PROCwr(61):PROCwr24(MX):PROCwr32(B)
  470 R=FNurl_go(N):PROCaddr(FNblk(N)+1):nlen=FNrd24
  480 =R
  490 :
  500 REM  HTTP status code from the last opened URL handle (url_status +3..4)
  510 DEF FNurl_httpcode(N)
  520 LOCAL R,l,h : PROCaddr(FNblk(N)):PROCwr(64):R=FNurl_go(N)
  530 PROCaddr(FNblk(N)+3):l=?dat:h=?dat:=l+h*256
  540 :
  550 DEF PROCurl_close(N) : PROCaddr(FNblk(N)):PROCwr(63):R=FNurl_go(N):ENDPROC
