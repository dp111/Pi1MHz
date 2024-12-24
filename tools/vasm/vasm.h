/* vasm.h  main header file for vasm */
/* (c) in 2002-2024 by Volker Barthelmann */

#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <limits.h>

typedef struct atom atom;
typedef struct dblock dblock;
typedef struct sblock sblock;
typedef struct symbol symbol;
typedef struct section section;
typedef struct expr expr;
typedef struct macro macro;
typedef struct source source;
typedef struct listing listing;
typedef struct regsym regsym;
typedef struct rlist rlist;

typedef struct strbuf {
  size_t size;
  size_t len;
  char *str;
} strbuf;
#define STRBUFINC 0x100

#define MAXPADSIZE 8  /* max. pattern size to pad alignments */

#include "cpu.h"
#include "symbol.h"
#include "reloc.h"
#include "syntax.h"
#include "symtab.h"
#include "expr.h"
#include "atom.h"
#include "parse.h"
#include "source.h"
#include "cond.h"
#include "listing.h"
#include "supp.h"

#if defined(BIGENDIAN)&&!defined(LITTLEENDIAN)
#define LITTLEENDIAN (!BIGENDIAN)
#endif

#if !defined(BIGENDIAN)&&defined(LITTLEENDIAN)
#define BIGENDIAN (!LITTLEENDIAN)
#endif

#if !defined(BITSPERBYTE)
#define BITSPERBYTE 8
#endif

#ifndef MNEMONIC_VALID
#define MNEMONIC_VALID(i) 1
#endif

#ifndef OPERAND_OPTIONAL
#define OPERAND_OPTIONAL(p,t) 0
#endif

#ifndef IGNORE_FIRST_EXTRA_OP
#define IGNORE_FIRST_EXTRA_OP 0
#endif

#ifndef START_PARENTH
#define START_PARENTH(x) ((x)=='(')
#endif

#ifndef END_PARENTH
#define END_PARENTH(x) ((x)==')')
#endif

#ifndef CHKIDEND
#define CHKIDEND(s,e) (e)
#endif

#define MAXPATHLEN 1024

/* operations on bit-vectors */
typedef unsigned int bvtype;
#define BVBITS (sizeof(bvtype)*CHAR_BIT)
#define BVSIZE(x) ((((x)+BVBITS-1)/BVBITS)*sizeof(bvtype))
#define BSET(array,bit) (array)[(bit)/BVBITS]|=1<<((bit)%BVBITS)
#define BCLR(array,bit) (array)[(bit)/BVBITS]&=~(1<<((bit)%BVBITS))
#define BTST(array,bit) ((array)[(bit)/BVBITS]&(1<<((bit)%BVBITS)))

/* section flags */
#define HAS_SYMBOLS      (1<<0)
#define RESOLVE_WARN     (1<<1)
#define UNALLOCATED      (1<<2)
#define LABELS_ARE_LOCAL (1<<3)
#define ABSOLUTE         (1<<4)
#define PREVABS          (1<<5) /* saved ABSOLUTE-flag during RORG-block */
#define IN_RORG          (1<<6)       
#define NEAR_ADDRESSING  (1<<7)
#define FAR_ADDRESSING   (1<<8)
#define SECRSRVD       (1L<<24) /* bits 24-31 are reserved for output modules */

/* section description */
struct section {
  struct section *next;
  bvtype *deps;
  char *name;
  char *attr;
  atom *first;
  atom *last;
  taddr align;
  uint8_t pad[MAXPADSIZE];
  int padbytes;
  uint32_t flags;
  taddr memattr;  /* type of memory, used by some object formats */
  taddr org;
  taddr pc;
  unsigned long idx; /* usable by output module */
};

/* mnemonic description */
typedef struct mnemonic {
  const char *name;
#if MAX_OPERANDS!=0
  int operand_type[MAX_OPERANDS];
#endif
  mnemonic_extension ext;
} mnemonic;

/* operand size flags (ORed with size in bits) */
#define OPSZ_BITS(x)	((x) & 0xff)
#define OPSZ_FLOAT      0x100  /* operand stored as floating point */
#define OPSZ_SWAP	0x200  /* operand stored with swapped bytes */


extern char *inname,*outname,*output_format,*defsectname,*defsecttype;
extern taddr defsectorg,inst_alignment;
extern int chklabels,nocase,no_symbols,pic_check,unnamed_sections;
extern unsigned space_init;
extern int asciiout,secname_attr,warn_unalloc_ini_dat;
extern hashtable *mnemohash;
extern char *filename,*debug_filename;
extern source *cur_src;
extern section *current_section,container_section;
extern int num_secs,final_pass,exec_out,nostdout;
extern struct stabdef *first_nlist,*last_nlist;
extern char emptystr[];
extern char vasmsym_name[];

extern int octetsperbyte,output_bitsperbyte,output_bytes_le,input_bytes_le;
extern unsigned long long taddrmask;
extern taddr taddrmin,taddrmax;
#define ULLTADDR(x) (((unsigned long long)x)&taddrmask)
#define ULLTVAL(x) ((unsigned long long)((utaddr)(x)))

/* provided by main assembler module */
extern int debug;

void leave(void);
void set_taddr(void);
void set_section(section *);
section *new_section(const char *,const char *,int);
section *new_org(taddr);
section *find_section(const char *,const char *);
void switch_section(const char *,const char *);
void switch_offset_section(const char *,taddr);
void add_align(section *,taddr,expr *,int,unsigned char *);
section *default_section(void);
void push_section(void);
section *pop_section(void);
#if NOT_NEEDED
section *restore_section(void);
section *restore_org(void);
#endif
int end_rorg(void);
void try_end_rorg(void);
void start_rorg(taddr);
void print_section(FILE *,section *);

#define setfilename(x) filename=(x)
#define getfilename() filename
#define setdebugname(x) debug_filename=(x)
#define getdebugname() debug_filename

/* provided by error.c */
extern int errors,warnings;
extern int max_errors;
extern int no_warn;

void general_error(int,...);
void syntax_error(int,...);
void cpu_error(int,...);
void output_error(int,...);
void output_atom_error(int,atom *,...);
void modify_gen_err(int,...);
void modify_syntax_err(int,...);
void modify_cpu_err(int,...);
void disable_message(int);
void disable_warning(int);

#define ierror(x) general_error(4,(x),__LINE__,__FILE__)

/* provided by cpu.c */
extern int bytespertaddr;
extern const int mnemonic_cnt;
extern mnemonic mnemonics[];
extern const char *cpu_copyright;
extern const char *cpuname;

/* convert target-bytes into the host's 8-bit bytes */
#if BITSPERBYTE == 8
#define OCTETS(n) (n)
#else
#define OCTETS(n) ((n)*octetsperbyte)
#endif

int init_cpu(void);
int cpu_args(char *);
char *parse_cpu_special(char *);
operand *new_operand(void);
int parse_operand(char *,int,operand *,int);
size_t instruction_size(instruction *,section *,taddr);
dblock *eval_instruction(instruction *,section *,taddr);
dblock *eval_data(operand *,size_t,section *,taddr);
#if HAVE_INSTRUCTION_EXTENSION
void init_instruction_ext(instruction_ext *);
#endif
#if MAX_QUALIFIERS!=0
char *parse_instruction(char *,int *,char **,int *,int *);
int set_default_qualifiers(char **,int *);
#endif
#if HAVE_CPU_OPTS
void cpu_opts_init(section *);
void cpu_opts(void *);
void print_cpu_opts(FILE *,void *);
#endif

/* provided by syntax.c */
extern const char *syntax_copyright;
extern char commentchar;
extern int dotdirs;
extern hashtable *dirhash;

int init_syntax(void);
int syntax_args(char *);
int syntax_defsect(void);
void parse(void);
char *parse_macro_arg(struct macro *,char *,struct namelen *,struct namelen *);
int expand_macro(source *,char **,char *,int);
char *skip(char *);
void eol(char *);
char *const_prefix(char *,int *);
char *const_suffix(char *,char *);
strbuf *get_local_label(int,char **);

/* provided by output_xxx.c */
#ifdef OUTTOS
extern int tos_hisoft_dri;
#endif
#ifdef OUTHUNK
extern int hunk_xdefonly;
extern int hunk_devpac;
#endif

int init_output_test(char **,void (**)(FILE *,section *,symbol *),int (**)(char *));
int init_output_elf(char **,void (**)(FILE *,section *,symbol *),int (**)(char *));
int init_output_bin(char **,void (**)(FILE *,section *,symbol *),int (**)(char *));
int init_output_srec(char **,void (**)(FILE *,section *,symbol *),int (**)(char *));
int init_output_vobj(char **,void (**)(FILE *,section *,symbol *),int (**)(char *));
int init_output_hunk(char **,void (**)(FILE *,section *,symbol *),int (**)(char *));
int init_output_aout(char **,void (**)(FILE *,section *,symbol *),int (**)(char *));
int init_output_tos(char **,void (**)(FILE *,section *,symbol *),int (**)(char *));
int init_output_gst(char **,void (**)(FILE *,section *,symbol *),int (**)(char *));
int init_output_dri(char **,void (**)(FILE *,section *,symbol *),int (**)(char *));
int init_output_xfile(char **,void (**)(FILE *,section *,symbol *),int (**)(char *));
int init_output_cdef(char **,void (**)(FILE *,section *,symbol *),int (**)(char *));
int init_output_ihex(char **,void (**)(FILE *,section *,symbol *),int (**)(char *));
int init_output_o65(char **,void (**)(FILE *,section *,symbol *),int (**)(char *));
int init_output_woz(char **,void (**)(FILE *,section *,symbol *),int (**)(char *));
int init_output_pap(char **,void (**)(FILE *,section *,symbol *),int (**)(char *));
int init_output_hans(char **,void (**)(FILE *,section *,symbol *),int (**)(char *));
