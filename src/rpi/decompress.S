
.syntax             unified

/* License: Public Domain - I cannot be held responsible for what it does or does not do if you use it, whether it's modified or not. */

/* Entry point = unlz4. On entry: r0 = source, r1 = destination. The first two bytes of the source must contain the length of the compressed data. */

                    .func               unlz4
                    .global             unlz4
                    .type               unlz4,%function

unlz4:              ldrh                r2,[r0],#2          /* get length of compressed data */

                    push                {r4-r6,lr}          /* save r4, r5, r6 and return-address */
                    add                 r5,r2,r0            /* point r5 to end of compressed data */

getToken:           ldrb                r6,[r0],#1          /* get token */
                    // stall
                    // stall
                    lsrs                r4,r6,#4            /* get literal length, keep token in r6 */
                    // stall
                    beq                 getOffset           /* jump forward if there are no literals */
                    bl                  getLength           /* get length of literals */
                    movs                r2,r0               /* point r2 to literals */
                    bl                  copyData            /* copy literals (r2=src, r1=dst, r4=len) */
                    movs                r0,r2               /* update source pointer */

getOffset:          ldrh                r3,[r0],#2          /* get match offset */
                    and                 r4,r6,#0x0f         /* get match length */
                    // stall
                    sub                 r2,r1,r3            /* subtract from destination; this will become the match position */

                    bl                  getLength           /* get length of match data */
                    add                 r4,r4,#4            /* minimum match length is 4 bytes */
                    bl                  copyData            /* copy match data (r2=src, r1=dst, r4=len) */
                    cmp                 r0,r5               /* check if we've reached the end of the compressed data */
                    blt                 getToken            /* if not, go get the next token */

                    pop                 {r4-r6,pc}          /* restore r4, r5 and r6, then return */

getLength:          cmp                 r4,#0x0f            /* if length is 15, then more length info follows */
                    bne                 gotLength           /* jump forward if we have the complete length */

getLengthLoop:      ldrb                r3,[r0],#1          /* read another byte */
                    // stall
                    // stall
                    cmp                 r3,#0xff            /* check if end reached */
                    add                 r4,r4,r3            /* add byte to length */
                    beq                 getLengthLoop       /* if not, go round loop */

gotLength:          bx                  lr                  /* return */

copyData:

                    // think about using ldrh and ldr for longer copies

copyDataLoop:       ldrb                r3,[r2],#1          /* read byte from source */
                    subs                r4,r4,#1            /* decrement index */
                    // stall
                    strb                r3,[r1],#1          /* store byte in destination */
                    bne                 copyDataLoop        /* keep going until index wraps to 0 */
                    bx                  lr                  /* return */

                    .endfunc