/* bin.c binary output driver for vasm */
/* (c) in 2002-2009,2013-2024 by Volker Barthelmann and Frank Wille */

#include "vasm.h"

#ifdef OUTBIN
static char *copyright="vasm binary output module 2.3c (c) 2002-2024 Volker Barthelmann and Frank Wille";

enum {
  BINFMT_RAW,           /* no header */
  BINFMT_CBMPRG,        /* Commodore VIC-20/C-64 PRG format */
  BINFMT_ATARICOM,      /* Atari 800 DOS COM format */
  BINFMT_APPLEBIN,      /* Apple DOS 3.3 binary file */
  BINFMT_DRAGONBIN,     /* Dragon DOS binary format */
  BINFMT_COCOML,        /* Tandy Color Computer machine lang. file */
  BINFMT_ORICMC,        /* Oric machine code file */
  BINFMT_ORICMCX,       /* Oric machine code file auto-exec */
  BINFMT_FOENIXPGX,     /* C256 Foenix single-section format */
  BINFMT_FOENIXPGZ      /* C256 Foenix multi-section format */
};

static int binfmt = BINFMT_RAW;
static char *exec_symname;
static taddr exec_addr;
static int addrbits,coalesce;


static int orgcmp(const void *sec1,const void *sec2)
{
  if (((utaddr)(*(section **)sec1)->org) > ((utaddr)(*(section **)sec2)->org))
    return 1;
  if (((utaddr)(*(section **)sec1)->org) < ((utaddr)(*(section **)sec2)->org))
    return -1;
  return 0;
}


static void write_output(FILE *f,section *sec,symbol *sym)
{
  section *s,**seclist,**slp;
  unsigned long long pc=0,npc;
  size_t nsecs;
  long hdroffs;
  char *nptr;
  atom *p;

  if (sec == NULL)
    return;

  for (; sym; sym=sym->next) {
    if (sym->type==IMPORT)
      output_error(6,sym->name);  /* undefined symbol */

    if (exec_symname!=NULL && !strcmp(exec_symname,sym->name)) {
      exec_addr = sym->pc;
      exec_symname = NULL;  /* found the start-symbol */
    }
  }
  if (exec_symname != NULL)
    output_error(6,exec_symname);  /* start-symbol not found */

  /* we don't support overlapping sections, count sections */
  nsecs = chk_sec_overlap(sec);

  /* make an array of section pointers, sorted by their start address */
  seclist = (section **)mymalloc(nsecs * sizeof(section *));
  for (s=sec,slp=seclist; s!=NULL; s=s->next)
    *slp++ = s;
  if (nsecs > 1)
    qsort(seclist,nsecs,sizeof(section *),orgcmp);

  sec = *seclist;  /* sec is now the first section in memory */

  /* write an optional header first */
  switch (binfmt) {
    case BINFMT_APPLEBIN:
      /* Apple DOS binary file header:
       * 00-01: load address (little endian)
       * 02-03: file length (little endian)
       */
      fw16(f,sec->org,0);
      hdroffs = ftell(f);  /* remember location of file length */
      fw16(f,0,0);         /* skip file length, will be patched later */
      break;

    case BINFMT_ATARICOM:
      /* ATARI COM header: $FFFF */
      fw16(f,0xffff,0);
      break;

    case BINFMT_CBMPRG:
      /* Commodore 6502 PRG header:
       * 00: LSB of load address
       * 01: MSB of load address
       */
      fw16(f,sec->org,0);
      break;

    case BINFMT_DRAGONBIN:
      /* Dragon DOS file header:
       * 00:    $55
       * 01:    filetype binary $02
       * 02-03: load address (big endian)
       * 04-05: file length (big endian)
       * 06-07: execution address (big endian)
       * 08:    $AA
       */
      fw16(f,0x5502,1);
      fw16(f,sec->org,1);
      hdroffs = ftell(f);  /* remember location of file length */
      fw16(f,0,1);         /* skip file length, will be patched later */
      fw16(f,exec_addr?exec_addr:sec->org,1);
      fw8(f,0xaa);
      break;

    case BINFMT_FOENIXPGX:
      /* C256 Foenix single-segment header PGX
       * 00:    "PGX"
       * 03:    $01 for 65816, $03 for 65C02 CPU
       * 04-07: load/execute address (little endian)
       */
#if defined(VASM_CPU_650X)
      if (cpu_type & (WDC65816|M65C02)) {
        uint8_t id[4] = { 'P','G','X' };

        id[3] = (cpu_type & WDC65816) ? 1 : 3;
        fwdata(f,id,4);
        fw32(f,sec->org,0);
        break;
      }
#endif
      cpu_error(1,cpuname);
      return;

    case BINFMT_FOENIXPGZ:
      /* C256 Foenix multi-segment header PGZ: "Z" followed by seg. headers */
      fw8(f,addrbits<32 ? 'Z' : 'z');	/* Z: 16/24-bit, z: 32-bit addresses */
      break;

    case BINFMT_ORICMC:
    case BINFMT_ORICMCX:
      /* Oric machine code file header:
       * 00-03: sync: $16,$16,$16,$16
       * 04:    end of sync: $24
       * 05-06: two reserved bytes
       * 07:    file data type ($00 BASIC, $80 machine code)
       * 08:    how to execute ($80 RUN as BASIC, $c7 execute as machine code)
       * 09-10: last address of data stored (big-endian)
       * 11-12: first address of data stored (big-endian)
       * 13:    reserved
       * 14-  : null-terminated file name, maximum of 15 characters in name
       */
      fw32(f,0x16161616,1);
      fw8(f,0x24);
      fw16(f,0,1);
      fw8(f,0x80);
      fw8(f,binfmt==BINFMT_ORICMCX?0xc7:0);  /* auto-exec or not */
      hdroffs = ftell(f);  /* remember location of last address */
      fw16(f,0,1);         /* skip last address, will be patched later */
      fw16(f,sec->org,1);
      fw8(f,0);
      nptr = outname;
      while (*nptr) {
        if (!stricmp(nptr,".tap") || (nptr-outname)>=15)
          break;  /* remove .tap extension, no more than 15 characters */
        fw8(f,toupper((unsigned char)*nptr++));
      }
      fw8(f,0);
      break;
  }

  for (slp=seclist; nsecs>0; nsecs--) {
    s = *slp++;

    /* strip uninitialized space atoms from section */
    if (s->last)
      s->last->next = NULL;
    else
      s->first = NULL;

    /* write optional section header or pad to next section start */
    switch (binfmt) {
      case BINFMT_ATARICOM:
        /* for each section
         * 00-01: address of first byte (little endian)
         * 02-03: address of last byte (little endian)
         */
        fw16(f,s->org,0);
        fw16(f,s->pc-1,0);
        break;

      case BINFMT_COCOML:
        /* segment header with length and load address
         * 00:    $00
         * 01-02: length of segment in bytes (big endian)
         * 03-04: load address of segment (big endian)
         */
        fw8(f,0);
        fw16(f,s->pc-s->org,1);
        fw16(f,s->org,1);
        break;

      case BINFMT_FOENIXPGZ:
        /* 00-02/00-03: load-address of segment (little endian)
         * 03-05/04-07: size of segment (little endian)
         */
        if (addrbits < 32) {
          fw24(f,s->org,0);
          fw24(f,s->pc-s->org,0);
        }
        else {
          fw32(f,s->org,0);
          fw32(f,s->pc-s->org,0);
        }
        break;

      default:
        /* fill gap between sections with pad-bytes */
        if (!coalesce && s!=seclist[0] && ((unsigned long long)s->org) > pc)
          fwpattern(f,((unsigned long long)s->org)-pc,s->pad,s->padbytes);
        break;
    }

    /* write section contents */
    for (p=s->first,pc=(unsigned long long)s->org; p; p=p->next) {
      npc = fwpcalign(f,p,s,pc);

      if (p->type == DATA)
        fwdblock(f,p->content.db);
      else if (p->type == SPACE)
        fwsblock(f,p->content.sb);

      pc = npc + atom_size(p,s,npc);
    }
  }

  /* patch the header or write trailer */
  switch (binfmt) {
    case BINFMT_APPLEBIN:
      fseek(f,hdroffs,SEEK_SET);
      fw16(f,pc-sec->org,0);  /* total file length */
      break;

    case BINFMT_DRAGONBIN:
      fseek(f,hdroffs,SEEK_SET);
      fw16(f,pc-sec->org,1);  /* total file length */
      break;

    case BINFMT_COCOML:
      /* Color Computer machine language trailer
       * 00-02: end of program token: $ff,$00,$00
       * 03-04: execution address (big endian)
       */
      fw8(f,0xff);
      fw16(f,0,1);
      fw16(f,exec_addr?exec_addr:sec->org,1);
      break;

    case BINFMT_FOENIXPGZ:
      /* trailer with execute-address and a zero-size to indicate that
         no more segments follow */
      if (addrbits < 32) {
        fw24(f,exec_addr?exec_addr:sec->org,0);
        fw24(f,0,0);
      }
      else {
        fw32(f,exec_addr?exec_addr:sec->org,0);
        fw32(f,0,0);
      }
      break;

    case BINFMT_ORICMC:
    case BINFMT_ORICMCX:
      fseek(f,hdroffs,SEEK_SET);
      fw16(f,pc-1,1);  /* last address of file */
      break;
  }

  myfree(seclist);
}


static int output_args(char *p)
{
  long long val;

  if (!strcmp(p,"-coalesced")) {
    coalesce = 1;
    return 1;
  }
  else if (!strncmp(p,"-exec=",6)) {
    exec_symname = p + 6;
    return 1;
  }
  else if (!strncmp(p,"-start=",7)) {
    sscanf(p+7,"%lli",&val);
    defsectorg = val;  /* set start of default section */
    return 1;
  }
  else if (!strcmp(p,"-apple-bin")) {
    binfmt = BINFMT_APPLEBIN;
    return 1;
  }
  else if (!strcmp(p,"-atari-com")) {
    binfmt = BINFMT_ATARICOM;
    return 1;
  }
  else if (!strcmp(p,"-cbm-prg")) {
    binfmt = BINFMT_CBMPRG;
    return 1;
  }
  else if (!strcmp(p,"-coco-ml")) {
    binfmt = BINFMT_COCOML;
    return 1;
  }
  else if (!strcmp(p,"-dragon-bin")) {
    binfmt = BINFMT_DRAGONBIN;
    return 1;
  }
  else if (!strcmp(p,"-foenix-pgx")) {
    binfmt = BINFMT_FOENIXPGX;
    return 1;
  }
  else if (!strcmp(p,"-foenix-pgz")) {
    binfmt = BINFMT_FOENIXPGZ;
    return 1;
  }
  else if (!strcmp(p,"-oric-mc")) {
    binfmt = BINFMT_ORICMC;
    return 1;
  }
  else if (!strcmp(p,"-oric-mcx")) {
    binfmt = BINFMT_ORICMCX;
    return 1;
  }
  return 0;
}


int init_output_bin(char **cp,void (**wo)(FILE *,section *,symbol *),int (**oa)(char *))
{
  *cp = copyright;
  *wo = write_output;
  *oa = output_args;
  defsecttype = emptystr;  /* default section is "org 0" */
  output_bitsperbyte = 1;  /* we do support BITSPERBYTE != 8 */
  addrbits = bytespertaddr * BITSPERBYTE;
  return 1;
}

#else

int init_output_bin(char **cp,void (**wo)(FILE *,section *,symbol *),int (**oa)(char *))
{
  return 0;
}
#endif
