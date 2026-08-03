   10 REM ---------------------------------------------------------------
   20 REM  Pi1MHz net service - raw UDP echo demo (commands 55/56).
   30 REM  Opens a UDP handle, binds a local port, sends a datagram and
   40 REM  prints the echo the peer sends back.  Needs net_enable=1 and a
   50 REM  UDP echo server on host:port.  See NETDEMO.BAS for TCP/DNS.
   60 REM ---------------------------------------------------------------
   70 net=&FCA6 : cmd=net+4 : dat=net+3
   80 nOK=0 : nPEND=1 : nBUSY=&80
   90 txb=&000000 : rxb=&010000
  100 :
  110 REM ===== edit these ============================================
  120 host$="192.168.0.89" : port=9999 : lport=7000 : H=6
  130 msg$="HELLO-FROM-BEEB"
  140 :
  150 MODE 7 : PRINT "Pi1MHz UDP echo demo" : PRINT
  160 IF FNopen(H,1)<>nOK THEN PRINT "open failed" : END
  170 IF FNbind(H,lport)<>nOK THEN PRINT "bind failed" : PROCclose(H) : END
  180 PRINT "sent ";FNudp_send(H,host$,port,msg$);" bytes to ";host$;":";port
  190 PRINT "--- reply ---"
  200 nlen=0
  210 FOR T=1 TO 1000
  220   R=FNudp_recv(H,rxb,200)
  230   IF nlen>0 THEN T=1000
  240 NEXT
  250 IF nlen=0 THEN PRINT "(no reply)" : PROCclose(H) : END
  260 PROCaddr(rxb) : FOR I=1 TO nlen : VDU ?dat : NEXT : PRINT
  270 PRINT "from ";rip0;".";rip1;".";rip2;".";rip3;":";rport;" (";nlen;" bytes)"
  280 PROCclose(H)
  290 END
  300 :
  310 REM ===== library ================================================
  320 DEF PROCaddr(A) : ?net=A AND &FF : ?(net+1)=(A DIV &100)AND &FF : ?(net+2)=(A DIV &10000)AND &FF : ENDPROC
  330 DEF FNblk(N)=&FFF000+N*&100
  340 DEF PROCwr(V) : ?dat=V AND &FF : ENDPROC
  350 DEF PROCwr24(V) : PROCwr(V):PROCwr(V DIV &100):PROCwr(V DIV &10000):ENDPROC
  360 DEF PROCwr32(V) : PROCwr24(V):PROCwr(V DIV &1000000):ENDPROC
  370 DEF FNrd24 : LOCAL l,m,h : l=?dat:m=?dat:h=?dat : =l+m*256+h*65536
  380 DEF PROCcmdhdr(N,C) : PROCaddr(FNblk(N)):PROCwr(C) : ENDPROC
  390 REM  dispatch handle N, spin the transient NET_BUSY
  400 DEF FNgo(N) : LOCAL R : ?cmd=&F0+N : REPEAT R=?cmd : UNTIL (R AND nBUSY)=0 : =R
  410 REM  re-issue an async command until it settles (or ESCAPE)
  420 DEF FNgo_wait(N) : LOCAL R : REPEAT R=FNgo(N) : UNTIL R<>nPEND : =R
  430 :
  440 DEF FNopen(N,T) : PROCcmdhdr(N,45):PROCwr(T) : =FNgo(N)
  450 DEF FNbind(N,P) : PROCcmdhdr(N,48):PROCwr(P AND &FF):PROCwr(P DIV &100) : =FNgo(N)
  460 DEF PROCclose(N) : PROCcmdhdr(N,53):R=FNgo(N) : ENDPROC
  470 :
  480 REM  send S$ to IP$:P from handle N ; returns bytes queued (or <0 on error)
  490 DEF FNudp_send(N,IP$,P,S$)
  500 LOCAL R,I,a,b,c,d,p1
  510 p1=INSTR(IP$,".") : a=VAL(LEFT$(IP$,p1-1)) : IP$=MID$(IP$,p1+1)
  520 p1=INSTR(IP$,".") : b=VAL(LEFT$(IP$,p1-1)) : IP$=MID$(IP$,p1+1)
  530 p1=INSTR(IP$,".") : c=VAL(LEFT$(IP$,p1-1)) : d=VAL(MID$(IP$,p1+1))
  540 PROCaddr(txb):FOR I=1 TO LEN(S$):PROCwr(ASC(MID$(S$,I,1))):NEXT
  550 PROCcmdhdr(N,55):PROCwr(a):PROCwr(b):PROCwr(c):PROCwr(d)
  560 PROCwr(P AND &FF):PROCwr(P DIV &100):PROCwr24(LEN(S$)):PROCwr32(txb)
  570 R=FNgo_wait(N) : IF R<>nOK THEN =-1
  580 PROCaddr(FNblk(N)+7) : =FNrd24
  590 :
  600 REM  recv one datagram into B (max MX) ; sets nlen, rip0..3, rport
  610 DEF FNudp_recv(N,B,MX)
  620 LOCAL R,l,h : PROCaddr(FNblk(N)):PROCwr(56)
  630 PROCaddr(FNblk(N)+7):PROCwr24(MX):PROCwr32(B)
  640 R=FNgo(N)
  650 PROCaddr(FNblk(N)+1):rip0=?dat:rip1=?dat:rip2=?dat:rip3=?dat
  660 l=?dat:h=?dat:rport=l+h*256
  670 PROCaddr(FNblk(N)+7):nlen=FNrd24
  680 =R
