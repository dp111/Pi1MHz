/* supp.c miscellaneous support routines */
/* (c) in 2008-2024 by Frank Wille */

#include <math.h>
#include "vasm.h"
#include "supp.h"


void initlist(struct list *l)
/* initializes a list structure */
{
  l->first = (struct node *)&l->dummy;
  l->dummy = NULL;
  l->last = (struct node *)&l->first;
}


void addtail(struct list *l,struct node *n)
/* add node as last element of list */
{
  struct node *ln = l->last;

  n->next = ln->next;
  ln->next = n;
  n->pred = ln;
  l->last = n;
}


struct node *remnode(struct node *n)
/* remove a node from a list */
{
  n->next->pred = n->pred;
  n->pred->next = n->next;
  return n;
}


struct node *remhead(struct list *l)
/* remove first node in list and return a pointer to it */
{
  struct node *n = l->first;

  if (n->next) {
    l->first = n->next;
    n->next->pred = n->pred;
    return n;
  }
  return NULL;
}


void *mymalloc(size_t sz)
{
  size_t *p;

  /* workaround for Electric Fence on 64-bit RISC */
  if (sz)
    sz = (sz + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1);

  if (debug) {
    if (sz == 0) {
      printf("Warning! Allocating 0 bytes. Adjusted to 1 byte.\n");
      sz = 1;
    }
    p = malloc(sz+2*sizeof(size_t));
    if (!p)
      general_error(17);
    p++;
    *p++ = sz;
    memset(p,0xdd,sz);  /* make it crash, when using uninitialized memory */
  }
  else {
    p = malloc(sz?sz:1);
    if(!p)
      general_error(17);
  }
  return p;
}


void *mycalloc(size_t sz)
{
  void *p = mymalloc(sz);

  memset(p,0,sz);
  return p;
}


void *myrealloc(const void *old,size_t sz)
{
  size_t *p;

  if (debug) {
    p = realloc(old?((size_t *)old)-2:0,sz+2*sizeof(size_t));
    if (!p)
      general_error(17);
    p++;
    *p++ = sz;
  }
  else {
    p = realloc((void *)old,sz);
    if (!p)
      general_error(17);
  }
  return p;
}


void myfree(void *p)
{
  if (p) {
    if (debug) {
      size_t *myp = (size_t *)p;
      size_t sz = *(--myp);
      memset(p,0xff,sz);  /* make it crash, when reusing deallocated memory */
      free(--myp);
    }
    else
      free(p);
  }
}


taddr bf_sign_extend(taddr val,int numbits)
/* sign-extend a bitfield value which fits into numbits bits */
{
  taddr himask = ~MAKEMASK(numbits);

  if (!(val & himask) && (val & (1LL<<(numbits-1))))
    val |= himask;  /* extend bitfield-sign over the taddr type */
  return val;
}


#if BITSPERBYTE != 8

utaddr readbyte(void *src)
/* reads a target byte */
{
  uint8_t *s = src;
  utaddr val = 0;
  int len = octetsperbyte;

  while (len--) {
    val <<= 8;
    val += (utaddr)*s++;
  }
  return val;
}


void writebyte(void *dest,utaddr val)
/* writes a target byte */
{
  uint8_t *d = dest;
  int len = octetsperbyte;

  d += len;
  while (len--) {
    *(--d) = (uint8_t)val;
    val >>= 8;
  }
}

#endif /* BITSPERBYTE != 8 */


uint64_t readval(int be,void *src,size_t size)
/* read value with given endianness and size in target-bytes */
{
  uint8_t *s = src;
  uint64_t val = 0;

  if (OCTETS(size) > sizeof(uint64_t))
    ierror(0);
  if (be) {
    while (size--) {
      val <<= BITSPERBYTE;
      val += (uint64_t)readbyte(s);
      s += OCTETS(1);
    }
  }
  else {
    s += OCTETS(size);
    while (size--) {
      s -= OCTETS(1);
      val <<= BITSPERBYTE;
      val += (uint64_t)readbyte(s);
    }
  }
  return val;
}


void *setval(int be,void *dest,size_t size,uint64_t val)
/* write value to destination with desired endianness */
{
  uint8_t *d = dest;

  if (OCTETS(size) > sizeof(uint64_t))
    ierror(0);
  if (be) {
    d += OCTETS(size);
    dest = d;
    while (size--) {
      d -= OCTETS(1);
      writebyte(d,(utaddr)val);
      val >>= BITSPERBYTE;
    }
  }
  else {
    while (size--) {
      writebyte(d,(utaddr)val);
      val >>= BITSPERBYTE;
      d += OCTETS(1);
    }
    dest = d;
  }
  return dest;
}


void *setval_signext(int be,void *dest,size_t extsz,size_t valsz,int64_t val)
/* write a sign-extended value to destination with desired endianness */
{
  uint8_t *d = dest;
  int sign = val<0 ? 0xff : 0;

  if (OCTETS(valsz) > sizeof(uint64_t))
    ierror(0);
  if (be) {
    memset(d,sign,OCTETS(extsz));
    d += OCTETS(extsz + valsz);
    dest = d;
    while (valsz--) {
      d -= OCTETS(1);
      writebyte(d,(utaddr)val);
      val >>= BITSPERBYTE;
    }
  }
  else {
    while (valsz--) {
      writebyte(d,(utaddr)val);
      val >>= BITSPERBYTE;
      d += OCTETS(1);
    }
    memset(d,sign,OCTETS(extsz));
    dest = d + OCTETS(extsz);
  }
  return dest;
}


uint64_t readbits(int be,void *p,unsigned bfsize,unsigned offset,unsigned size)
/* read value from a bitfield (max. 64 bits) */
{
  if ((bfsize&(BITSPERBYTE-1))==0 && offset+size<=bfsize) {
    uint64_t mask = MAKEMASK(size);
    uint64_t val = readval(be,p,bfsize/BITSPERBYTE);

    return be ? ((val >> (bfsize-(offset+size))) & mask)
              : ((val >> offset) & mask);
  }
  ierror(0);
  return 0;
}


void setbits(int be,void *p,unsigned bfsize,unsigned offset,unsigned size,
             uint64_t d)
/* write value to a bitfield (max. 64 bits) */
{
  if (bfsize%BITSPERBYTE==0 && offset+size<=bfsize) {
    uint64_t mask = MAKEMASK(size);
    uint64_t val = readval(be,p,bfsize/BITSPERBYTE);
    int s = be ? bfsize - (offset + size) : offset;

    setval(be,p,bfsize/BITSPERBYTE,(val & ~(mask<<s)) | ((d & mask) << s));
  }
  else
    ierror(0);
}


int countbits(taddr val)
/* count number of bits in val */
{
  int cnt = 0;
  int len = sizeof(taddr) << 3;

  while (len--) {
    if (val & 1)
      cnt++;
    val >>= 1;
  }

  return cnt;
}


int tffs(taddr val)
/* first first bit set in a taddr - similar to POSIX ffs() */
{
  int i,n=sizeof(taddr)<<3;

  if (val == 0)
    return 0;
  for (i=0; i<n; i++) {
    if (val & 1)
      break;
    val >>= 1;
  }
  return i;
}


void copy_cpu_taddr(void *dest,taddr val,size_t tbytes)
/* copy 'tbytes' low-order target-bytes from val to dest in cpu's endianness */
{
  uint8_t *d = dest;

  if (OCTETS(tbytes) > sizeof(taddr))
    ierror(0);
  if (BIGENDIAN) {
    d += OCTETS(tbytes);
    while (tbytes--) {
      d -= OCTETS(1);
      writebyte(d,val);
      val >>= BITSPERBYTE;
    }
  }
  else if (LITTLEENDIAN) {
    while (tbytes--) {
      writebyte(d,val);
      val >>= BITSPERBYTE;
      d += OCTETS(1);
    }
  }
  else
    ierror(0);
}


#if FLOAT_PARSER
void conv2ieee32(int be,uint8_t *buf,tfloat f)
/* single precision */
{
  union {
    float sp;
    uint32_t x;
  } conv;

  conv.sp = (float)f;
  setval(be,buf,32/BITSPERBYTE,conv.x);
}


void conv2ieee64(int be,uint8_t *buf,tfloat f)
/* double precision */
{
  union {
    double dp;
    uint64_t x;
  } conv;

  conv.dp = (double)f;
  setval(be,buf,64/BITSPERBYTE,conv.x);
}


/* check if float can be represented by bits, signed or unsigned,
   ignoring the fractional part */
int flt_chkrange(tfloat f,int bits)
{
  if (bits <= sizeof(taddr)*8) {
    tfloat max = (utaddr)1LL<<(bits-1);
    return (f<2.0*max && f>=-max);
  }
  ierror(0);  /* FIXME - shouldn't happen? */
  return 0;
}
#endif /* FLOAT_PARSER */


void fw8(FILE *f,uint8_t x)
{
  if (fputc(x,f) == EOF)
    output_error(2);  /* write error */
}


void fw16(FILE *f,uint16_t x,int be)
{
  if (be) {
    fw8(f,(x>>8) & 0xff);
    fw8(f,x & 0xff);
  }
  else {
    fw8(f,x & 0xff);
    fw8(f,(x>>8) & 0xff);
  }
}


void fw24(FILE *f,uint32_t x,int be)
{
  if (be) {
    fw8(f,(x>>16) & 0xff);
    fw16(f,(uint16_t)x,1);
  }
  else {
    fw16(f,(uint16_t)x,0);
    fw8(f,(x>>16) & 0xff);
  }
}


void fw32(FILE *f,uint32_t x,int be)
{
  if (be) {
    fw8(f,(x>>24) & 0xff);
    fw8(f,(x>>16) & 0xff);
    fw8(f,(x>>8) & 0xff);
    fw8(f,x & 0xff);
  }
  else {
    fw8(f,x & 0xff);
    fw8(f,(x>>8) & 0xff);
    fw8(f,(x>>16) & 0xff);
    fw8(f,(x>>24) & 0xff);
  }
}


void fwdata(FILE *f,const void *d,size_t n)
/* n is in 8-bit bytes */
{
  if (n) {
    if (!fwrite(d,1,n,f))
      output_error(2);  /* write error */
  }
}


void fwbytes(FILE *f,void *buf,size_t n)
/* write target-bytes in selected endianess; n is in target-bytes */
{
  if (output_bytes_le) {
    uint8_t *p = buf;
    int i;

    while (n--) {
      for (i=octetsperbyte; i>0; fw8(f,p[--i]));
      p += octetsperbyte;
    }
  }
  else
    fwdata(f,buf,OCTETS(n));
}


void fwsblock(FILE *f,sblock *sb)
{
  size_t i;

  for (i=0; i<sb->space; i++)
    fwbytes(f,sb->fill,sb->size);
}


void fwspace(FILE *f,size_t n)
/* n is in 8-bit bytes */
{
  size_t i;

  for (i=0; i<n; i++) {
    if (fputc(0,f) == EOF)
      output_error(2);  /* write error */
  }
}


void fwalign(FILE *f,taddr n,taddr align)
/* n and align are in target-bytes */
{
  fwspace(f,OCTETS(balign(n,align)));
}


int fwpattern(FILE *f,taddr n,uint8_t *pat,int patlen)
/* n and patlen are in target-bytes */
{
  int align_warning = 0;

  while (n % patlen) {
    align_warning = 1;
    fwspace(f,OCTETS(1));
    n--;
  }

  /* write alignment pattern */
  while (n >= patlen) {
    fwbytes(f,pat,patlen);
    n -= patlen;
  }

  while (n--) {
    align_warning = 1;
    fwspace(f,OCTETS(1));
  }
  
#if 0
  if (align_warning)
    output_error(9,sec->name,(unsigned long)n,(unsigned long)patlen,
                 ULLTADDR(pc));
#endif
  return align_warning;
}


taddr fwpcalign(FILE *f,atom *a,section *sec,taddr pc)
{
  taddr n = balign(pc,a->align);
  taddr patlen;
  uint8_t *pat;

  if (n == 0)
    return pc;

  if (a->type==SPACE && a->content.sb->space==0) {  /* space align atom */
    if (a->content.sb->maxalignbytes!=0 && n>a->content.sb->maxalignbytes)
      return pc;
    pat = a->content.sb->fill;
    patlen = a->content.sb->size;
  }
  else {
    pat = sec->pad;
    patlen = sec->padbytes;
  }

  fwpattern(f,n,pat,patlen);

  return pc+n;
}


size_t filesize(FILE *fp)
/* @@@ Warning! filesize() only works reliably on binary streams! @@@ */
{
  long size;

  if (fgetc(fp) != EOF)
    if (fseek(fp,0,SEEK_END) >= 0)
      if ((size = ftell(fp)) >= 0)
        if (fseek(fp,0,SEEK_SET) >= 0)
          return (size_t)size;
  return 0;
}


int abs_path(const char *path)
/* return true, when path is absolute */
{
  return *path=='/' || *path=='\\' || strchr(path,':')!=NULL;
}


int stricmp(const char *str1,const char *str2)
{
  while (tolower((unsigned char)*str1) == tolower((unsigned char)*str2)) {
    if (!*str1) return 0;
    str1++; str2++;
  }
  return tolower(*(unsigned char *)str1) - tolower(*(unsigned char *)str2);
}


int strnicmp(const char *str1,const char *str2,size_t n)
{
  if (n==0) return 0;
  while (--n && tolower((unsigned char)*str1) == tolower((unsigned char)*str2)) {
    if (!*str1) return 0;
    str1++; str2++;
  }
  return tolower(*(unsigned char *)str1) - tolower(*(unsigned char *)str2);
}


char *mystrdup(const char *name)
{
  char *p=mymalloc(strlen(name)+1);
  strcpy(p,name);
  return p;
}


char *cnvstr(const char *name,int l)
/* converts a pair of pointer/length to a null-terminated string */
{
  char *p=mymalloc(l+1);
  memcpy(p,name,l);
  p[l]=0;
  return p;
}


char *strbuf_alloc(strbuf *buf,size_t sz)
/* make sure static strbuf has space for 'sz' bytes */
{
  if (sz > buf->size) {
    buf->size = (sz+(STRBUFINC-1)) & ~(STRBUFINC-1);
    return buf->str = myrealloc(buf->str,buf->size);
  }
  return buf->str;
}


char *cutstr(strbuf *buf,const char *name,size_t len)
{
  if (len >= buf->size) {
    buf->size = (len+STRBUFINC) & ~(STRBUFINC-1);
    buf->str = myrealloc(buf->str,buf->size);
  }
  buf->str[len] = 0;
  buf->len = len;
  return memcpy(buf->str,name,len);
}


char *strtolower(char *s)
/* convert a whole string to lower case */
{
  char *p;

  for (p=s; *p; p++)
    *p = tolower((unsigned char)*p);
  return s;
}


int str_is_graph(const char *s)
/* tests if whole string has printable characters and no spaces */
{
  while (*s != '\0') {
    if (!isgraph((unsigned char)*s))
      return 0;
    s++;
  }
  return 1;
}


const char *trim(const char *s)
/* trim blanks before s */
{
  while (isspace((unsigned char )*(s-1)))
    s--;
  return s;
}


char *get_str_arg(const char *s)
/* get string argument from the command line, optionally in quotes */
{
  int term = 0;
  char *e;

  if (*s == '\"')
    term = *s++;
  if (!(e = strchr(s,term)))
    e = strchr(s,0);
  return cnvstr(s,e-s);
}


taddr balign(taddr addr,taddr a)
/* return number of bytes required to achieve alignment */
{
  if (a) {
    if (addr %= a)
      return a - addr;
  }
  return 0;
}


taddr palign(taddr addr,int a)
/* return number of bytes required to achieve alignment */
{
  return balign(addr,((taddr)1)<<a);
}


taddr pcalign(atom *a,taddr pc)
{
  taddr n = balign(pc,a->align);

  if (a->type==SPACE && a->content.sb->maxalignbytes!=0)
    if (n > a->content.sb->maxalignbytes)
      n = 0;
  return pc + n;
}


int make_padding(taddr val,uint8_t *pad,int maxbits)
/* fill a padding array with maxbits bits from a given padding value,
   return length in target-bytes */
{
  utaddr uval;
  int len;

  for (len=0,uval=(utaddr)val; uval!=0; uval>>=BITSPERBYTE,len++);
  if (len*BITSPERBYTE > maxbits)
    len = maxbits / BITSPERBYTE;
  copy_cpu_taddr(pad,val,len);
  return len;
}


size_t chk_sec_overlap(section *s)
/* fatal error when section address ranges overlap, return number of sect. */
{
  section *s2;
  size_t nsecs;

  for (nsecs=0; s!=NULL; s=s->next) {
    for (s2=s->next; s2; s2=s2->next) {
      if (((ULLTADDR(s2->org) >= ULLTADDR(s->org) &&
            ULLTADDR(s2->org) < ULLTADDR(s->pc)) ||
           (ULLTADDR(s2->pc) > ULLTADDR(s->org) &&
            ULLTADDR(s2->pc) <= ULLTADDR(s->pc))))
        output_error(0,s->name,ULLTADDR(s->org),ULLTADDR(s->pc),
                     s2->name,ULLTADDR(s2->org),ULLTADDR(s2->pc));
    }
    nsecs++;
  }
  return nsecs;
}

taddr get_sym_value(symbol *s)
/* determine symbol's value, returns alignment for common symbols */
{
  if (s->flags & COMMON) {
    return (taddr)s->align;
  }
  else if (s->type == LABSYM) {
    return s->pc;
  }
  else if (s->type == EXPRESSION) {
    if (s->expr) {
      taddr val;

      eval_expr(s->expr,&val,NULL,0);
      return val;
    }
    else
      ierror(0);
  }
  return 0;
}


taddr get_sym_size(symbol *s)
/* determine symbol's size */
{
  if (s->size) {
    taddr val;

    eval_expr(s->size,&val,NULL,0);
    return val;
  }
  return 0;
}


utaddr get_sec_size(section *sec)
{
  /* section size is assumed to be in in (sec->pc - sec->org), otherwise
     we would have to calculate it from the atoms and store it there */
  return sec ? (utaddr)sec->pc - (utaddr)sec->org : 0;
}


int get_sec_type(section *s)
/* determine section type from its attributes */
{
  char *a = s->attr;

  if (s->flags & ABSOLUTE)
    return S_ABS;
  while (*a) {
    switch (*a++) {
      case 'c':
        return S_TEXT;
      case 'd':
        return S_DATA;
      case 'u':
        return S_BSS;
    }
  }
  return S_MISS;  /* type is missing */
}
