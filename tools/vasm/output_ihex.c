#include "vasm.h"

#ifdef OUTIHEX

/* ihex formats */
#define I8HEX  0 /* supports 16-bit address space */
#define I16HEX 1 /* supports 20-bit address space */
#define I32HEX 2 /* supports 32-bit address space */

/* maximum address for I16HEX */
#define MEBIBYTE (1 << 20) - 1

/* ihex record types */
#define REC_DAT 0 /* data */
#define REC_EOF 1 /* end of file */
#define REC_ESA 2 /* extended segment address */
#define REC_SSA 3 /* start segment address */
#define REC_ELA 4 /* extended linear address */
#define REC_SLA 5 /* start linear address */

static char *copyright = "vasm Intel HEX output module 0.3 (c) 2020 Rida Dzhaafar";

static int ihex_fmt = I8HEX; /* default ihex format */

static uint8_t *buffer;       /* output buffer for data records */
static uint8_t buffer_s = 32; /* maximum buffer size */
static uint8_t buffer_i = 0;  /* current index in buffer */

static uint32_t addr = 0;     /* current output address */
static uint16_t ext_addr = 0; /* last written extended segment/linear address */

static void write_newline(FILE *f) /* gbm 06'21 */
{
  if (!asciiout)
    fw8(f, '\r');
  fw8(f, '\n');
}

static void write_eof_record(FILE *f)
{
  fprintf(f, ":00000001FF");
  write_newline(f);
}

static void write_extended_record(FILE *f)
{
  uint8_t csum;
  uint16_t ext;
  uint8_t type;

  if (ihex_fmt == I16HEX) {
    ext = ext_addr << 4;
    type = REC_ESA;
  }
  else if (ihex_fmt == I32HEX) {
    ext = ext_addr;
    type = REC_ELA;
  }

  csum = type + 2 + (ext >> 8) + ext;
  csum = (~csum) + 1;

  fprintf(f, ":020000%02X%04X%02X", type, ext, csum);
  write_newline(f);
}

static void write_data_record(FILE *f)
{
  uint8_t csum;
  uint8_t i;
  uint16_t ext;
  uint32_t start;

  /* pre-flight checks */
  if (buffer_i == 0)
    return;
  /* addr points to the next inserted byte
     addr-1 is the last one written */
  if (ihex_fmt == I8HEX && addr-1 > UINT16_MAX)
    output_error(11, addr-1);
  if (ihex_fmt == I16HEX && addr-1 > MEBIBYTE)
    output_error(11, addr-1);
  
  start = addr - buffer_i;
  ext = start >> 16;
  start &= 0xFFFF;
  /* set/reset extended address if needed */
  if (ext != ext_addr) {
    ext_addr = ext;
    write_extended_record(f);
  }

  /* write data record */
  fprintf(f, ":%02X%04X00", buffer_i, start);
  csum = start;
  csum += start >> 8;
  csum += buffer_i;
  for (i = 0; i < buffer_i; i++) {
    csum += buffer[i];
    fprintf(f, "%02X", buffer[i]);
  }
  csum = (~csum) + 1;
  fprintf(f, "%02X", csum);
  write_newline(f);

  /* reset the buffer index */
  buffer_i = 0;
}

static void buffer_data(FILE *f, uint8_t b)
{
  buffer[buffer_i] = b;
  buffer_i++;
  addr++;

  if (buffer_i == buffer_s)
    write_data_record(f);
}

static void buffer_byte(FILE *f, void *byte)
{
  uint8_t *p = byte;
  int i;

  if (output_bytes_le)
    for (i = octetsperbyte; i > 0; buffer_data(f, p[--i]));
  else
    for (i = 0; i < octetsperbyte; buffer_data(f, p[i++]));
}

/* align the atom if necessary
   adapted from support.c fwpcalign/fwpattern */
static taddr mypcalign(FILE *f, section *sec, atom *a, taddr pc)
{
  size_t align = balign(pc, a->align);
  int i;
  size_t len;
  uint8_t *fill;

  if (!align)
    return pc;

  if (a->type == SPACE && a->content.sb->space == 0) {
    if (a->content.sb->maxalignbytes != 0 &&
        align > a->content.sb->maxalignbytes)
      return pc;
    fill = a->content.sb->fill;
    len = a->content.sb->size;
  } else {
    fill = sec->pad;
    len = sec->padbytes;
  }

  pc += align;

  while (align % len) {
    for (i = 0; i < octetsperbyte; i++)
      buffer_data(f, 0);
    align--;
  }

  while (align >= len) {
    for (i = 0; i < len; i++) {
      buffer_byte(f, fill+OCTETS(i));
      align--;
    }
  }

  while (align--) {
    for (i = 0; i < octetsperbyte; i++)
      buffer_data(f, 0);
  }
  return pc;
}

static void write_output(FILE *f, section *sec, symbol *sym)
{
  int i, j;
  taddr pc;
  atom *a;
  section *s;

  if (!sec)
    return;

  for (; sym; sym = sym->next)
    if (sym->type == IMPORT)
      output_error(6, sym->name); /* undefined symbol (sym->name) */
  
  chk_sec_overlap(sec); /* fail on overlapping sections */

  buffer = mymalloc(sizeof(uint8_t) * buffer_s);

  for (s = sec; s; s = s->next) {
    pc = s->org;
    addr = pc * octetsperbyte;
    for (a = s->first; a; a = a->next) {
      pc = mypcalign(f, s, a, pc);
      if (a->type == DATA) {
        for (i = 0; i < a->content.db->size; i++,pc++) {
          buffer_byte(f, a->content.db->data+OCTETS(i));
        }
      } else if (a->type == SPACE) {
        for (i = 0; i < a->content.sb->space; i++) {
          for (j = 0; j < a->content.sb->size; j++,pc++) {
            buffer_byte(f, a->content.sb->fill+OCTETS(j));
          }
        }
      }
    }
    /* flush buffer before moving on to next section */
    write_data_record(f);
  }
  
  write_eof_record(f);
  myfree(buffer);
}

static int parse_args(char *arg)
{
  int size;

  if (!strcmp(arg, "-i8hex")) {
    ihex_fmt = I8HEX;
    return 1;
  }
  else if (!strcmp(arg, "-i16hex")) {
    ihex_fmt = I16HEX;
    return 1;
  }
  else if (!strcmp(arg, "-i32hex")) {
    ihex_fmt = I32HEX;
    return 1;
  }
  else if (!strncmp(arg, "-record-size=", 13)) {
    size = atoi(arg + 13);
    /* an IHEX record cannot be longer than FF bytes */
    if (size < 1 || size > 255)
      return 0;
    buffer_s = size;
    return 1;
  }
  else if (!strcmp(arg, "-crlf")) {  /* gbm */
    asciiout = 0;
    return 1;
  }
  return 0;
}

int init_output_ihex(char **cp, void (**wo)(FILE *, section *, symbol *), int (**oa)(char *))
{
  *cp = copyright;
  *wo = write_output;
  *oa = parse_args;
  asciiout = 1;
  output_bitsperbyte = 1;  /* we do support BITSPERBYTE != 8 */
  defsecttype = emptystr;  /* default section is "org 0" */
  return 1;
}

#else
int init_output_ihex(char **cp, void (**wo)(FILE *, section *, symbol *), int (**oa)(char *))
{
  return 0;
}
#endif
