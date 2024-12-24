/* atom.h - atomic objects from source */
/* (c) in 2010-2024 by Volker Barthelmann and Frank Wille */

#ifndef ATOM_H
#define ATOM_H

/* types of atoms */
enum {
  VASMDEBUG,LABEL,DATA,INSTRUCTION,SPACE,DATADEF,LINE,OPTS,
  PRINTTEXT,PRINTEXPR,ROFFS,RORG,RORGEND,ASSERT,NLIST
};

/* a machine instruction */
typedef struct instruction {
  int code;
#if MAX_QUALIFIERS!=0
  char *qualifiers[MAX_QUALIFIERS];
#endif
#if MAX_OPERANDS!=0
  operand *op[MAX_OPERANDS];
#endif
#if HAVE_INSTRUCTION_EXTENSION
  instruction_ext ext;
#endif
} instruction;  

typedef struct defblock {
  size_t bitsize;
  operand *op;
} defblock;

struct dblock {
  size_t size;
  uint8_t *data;
  rlist *relocs;
};

struct sblock {
  size_t space;
  expr *space_exp;  /* copied to space, when evaluated as constant */
  size_t size;
  uint8_t fill[MAXPADSIZE];
  expr *fill_exp;   /* copied to fill, when evaluated - may be NULL */
  rlist *relocs;
  taddr maxalignbytes;
  uint32_t flags;
};
/* Space is completely uninitialized - may be used as hint by output modules */
#define SPC_UNINITIALIZED 1
/* Space should be stored as a zeroed extension to a text/data section */
#define SPC_DATABSS 2

typedef struct reloffs {
  expr *offset;
  expr *fillval;
} reloffs;

typedef struct printexpr {
  expr *print_exp;
  short type;  /* hex, signed, unsigned */
  short size;  /* precision in bits */
} printexpr;
enum {
  PEXP_HEX,PEXP_SDEC,PEXP_UDEC,PEXP_BIN,PEXP_ASC
};

typedef struct assertion {
  expr *assert_exp;
  const char *expstr;
  const char *msgstr;
} assertion;

typedef struct aoutnlist {
  const char *name;
  int type;
  int other;
  int desc;
  expr *value;
} aoutnlist;

/* an atomic element of data */
struct atom {
  struct atom *next;
  int type;
  taddr align;
  size_t lastsize;
  unsigned changes;
  source *src;
  int line;
  listing *list;
  union {
    instruction *inst;
    dblock *db;
    symbol *label;
    sblock *sb;
    defblock *defb;
    void *opts;
    int srcline;
    const char *ptext;
    printexpr *pexpr;
    reloffs *roffs;
    taddr *rorg;
    assertion *assert;
    aoutnlist *nlist;
  } content;
};

#define MAXSIZECHANGES 5  /* warning, when atom changed size so many times */

enum {
  PO_CORRUPT=-1,PO_NOMATCH=0,PO_MATCH,PO_SKIP,PO_COMB_OPT,PO_COMB_REQ,PO_NEXT
};
instruction *new_inst(const char *,int,int,char **,int *);
instruction *copy_inst(instruction *);
dblock *new_dblock(void);
sblock *new_sblock(expr *,size_t,expr *);

atom *new_atom(int,taddr);
void add_atom(section *,atom *);
void add_or_save_atom(atom *);
size_t atom_size(atom *,section *,taddr);
void print_atom(FILE *,atom *);
void atom_printexpr(printexpr *,section *,taddr);
atom *clone_atom(atom *);

/* this group is currently used by dwarf.c only */
atom *add_data_atom(section *,size_t,taddr,taddr);
void add_leb128_atom(section *,utaddr);
void add_sleb128_atom(section *,taddr);
atom *add_char_atom(section *,const void *,size_t);
#define add_string_atom(s,p) add_char_atom(s,p,strlen(p)+1)

atom *new_inst_atom(instruction *);
atom *new_data_atom(dblock *,taddr);
atom *new_label_atom(symbol *);
atom *new_space_atom(expr *,size_t,expr *);
atom *new_datadef_atom(size_t,operand *);
atom *new_srcline_atom(int);
atom *new_opts_atom(void *);
atom *new_text_atom(const char *);
atom *new_expr_atom(expr *,int,int);
atom *new_roffs_atom(expr *,expr *);
atom *new_rorg_atom(taddr);
atom *new_rorgend_atom(void);
atom *new_assert_atom(expr *,const char *,const char *);
atom *new_nlist_atom(const char *,int,int,int,expr *);

#endif
