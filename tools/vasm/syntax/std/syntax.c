/* syntax.c  syntax module for vasm */
/* (c) in 2002-2024 by Volker Barthelmann and Frank Wille */

#include "vasm.h"
#include "stabs.h"

/* The syntax module parses the input (read_next_line), handles
   assembly-directives (section, data-storage etc.) and parses
   mnemonics. Assembly instructions are split up in mnemonic name,
   qualifiers and operands. new_inst returns a matching instruction,
   if one exists.
   Routines for creating sections and adding atoms to sections will
   be provided by the main module.
*/

const char *syntax_copyright="vasm std syntax module 5.6a (c) 2002-2024 Volker Barthelmann";
hashtable *dirhash;
int dotdirs = 1;

static char textname[]=".text",textattr[]="acrx";
static char dataname[]=".data",dataattr[]="adrw";
static char sdataname[]=".sdata",sdataattr[]="adrw";
static char sdata2name[]=".sdata2",sdata2attr[]="adr";
static char rodataname[]=".rodata",rodataattr[]="adr";
static char bssname[]=".bss",bssattr[]="aurw";
static char sbssname[]=".sbss",sbssattr[]="aurw";
static char tocdname[]=".tocd",tocdattr[]="adrw";
static char dpagename[]=".dpage",dpageattr[]="adrwz";

#ifdef SYNTAX_STD_COMMENTCHAR_HASH  /* define in cpu.h to allow #-comments */
char commentchar='#';
#else
char commentchar=';';
#endif

static char macroname[] = ".macro";
static char endmname[] = ".endm";
static char reptname[] = ".rept";
static char irpname[] = ".irp";
static char irpcname[] = ".irpc";
static char endrname[] = ".endr";
static struct namelen dmacro_dirlist[] = {
  { 6,&macroname[0] }, { 0,0 }
};
static struct namelen dendm_dirlist[] = {
  { 5,&endmname[0] }, { 0,0 }
};
static struct namelen drept_dirlist[] = {
  { 5,&reptname[0] }, { 4,&irpname[0] }, { 5,&irpcname[0] }, { 0,0 }
};
static struct namelen dendr_dirlist[] = {
  { 5,&endrname[0] }, { 0,0 }
};
static struct namelen endm_dirlist[] = {
  { 4,&endmname[1] }, { 0,0 }
};
static struct namelen macro_dirlist[] = {
  { 5,&macroname[1] }, { 0,0 }
};
static struct namelen rept_dirlist[] = {
  { 4,&reptname[1] }, { 3,&irpname[1] }, { 4,&irpcname[1] },{ 0,0 }
};
static struct namelen endr_dirlist[] = {
  { 4,&endrname[1] }, { 0,0 }
};

static int gas_compat;
static int parse_end;
static int alloccommon;
static int align_data;
static int noesc;
static unsigned local_labno[10]; /* ser.no. of GNU-as local labels 0 .. 9 */
static taddr sdlimit=-1;         /* max size of common data in .sbss section */
static int dir_else,dir_elif,dir_endif;  /* directive indices */


char *skip(char *s)
{
  while (isspace((unsigned char )*s))
    s++;
  return s;
}

/* check for end of line, issue error, if not */
void eol(char *s)
{
  s = skip(s);
  if (!ISEOL(s))
    syntax_error(6);
}

#ifdef VASM_CPU_M68K
char *chkidend(char *start,char *end)
{
  if ((end-start)>2 && *(end-2)=='.') {
    char c = tolower((unsigned char)*(end-1));

    if (c=='b' || c=='w' || c=='l')
      return end - 2;  /* .b/.w/.l extension is not part of M68k-identifier */
  }
  return end;
}
#endif

char *skip_operand(char *s)
{
#if defined(VASM_CPU_Z80)
  unsigned char lastuc = 0;
#endif
  int par_cnt=0;
  char c;

  while(1){
    c = *s;
    if(START_PARENTH(c)) par_cnt++;
    else if(END_PARENTH(c)){
      if(par_cnt>0)
        par_cnt--;
      else
        syntax_error(3);
#if defined(VASM_CPU_Z80)
    /* For the Z80 ignore ' behind a letter, as it may be a register */
    }else if((c=='\''&&(lastuc<'A'||lastuc>'Z'))||c=='\"')
#else
    }else if(c=='\''||c=='\"')
#endif
      s=skip_string(s,c,NULL)-1;
    else if(ISEOL(s)||(c==','&&par_cnt==0))
      break;
#if defined(VASM_CPU_Z80)
    lastuc = toupper((unsigned char)c);
#endif
    s++;
  }
  if(par_cnt!=0)
    syntax_error(4);
  return s;
}

static char *skip_macroparam(char *s)
{
  int par_cnt=0;
  char c;

  while(1){
    c = *s;
    if(START_PARENTH(c)) par_cnt++;
    if(END_PARENTH(c)){
      if(par_cnt>0)
        par_cnt--;
      else
        return s;
    }
    if(ISEOL(s)||((c==','||isspace((unsigned char)c))&&par_cnt==0))
      break;
    s++;
  }
  return s;
}

static taddr comma_constexpr(char **s)
{
  *s = skip(*s);
  if (**s == ',') {
    *s = skip(*s + 1);
    return parse_constexpr(s);
  }
  general_error(6,',');  /* comma expected */
  return 0;
}

static void handle_section(char *s)
{
  char *name,*attr,*p;
  strbuf *namebuf;
  uint32_t mem=0;
  section *sec;

  if(!(namebuf=parse_name(0,&s))){
    syntax_error(20);  /* section name */
    return;
  }
  name=namebuf->str;
  s=skip(s);
  if(*s==','){
    strbuf *attrbuf;
    s=skip(s+1);
    if (attrbuf=get_raw_string(&s,'\"')){
      attr=attrbuf->str;
      s=skip(s);
      if(*s==','){
        p=s=skip(s+1);
        if(*s=='@'||*s=='%'){
          /* ELF section type "progbits" or "nobits" */
          strbuf *typebuf;
          s++;
          if(typebuf=parse_identifier(0,&s)){
            if(!strcmp(typebuf->str,"nobits")){
              if(!strchr(attr,'u')){
                attr=strbuf_alloc(attrbuf,attrbuf->len+2);
                strcat(attr,"u");  /* append 'u' for "nobits" */
                attrbuf->len++;
              }
            }else{
              if(strcmp(typebuf->str,"progbits"))
                syntax_error(14);  /* invalid section type ignored */
            }
          }
        }else{
          /* optional memory flags (vasm-specific) */
          mem=parse_constexpr(&s);
          if(s==p) syntax_error(9);  /* memory flags */
        }
      }
    }else{
      syntax_error(7);  /* section flags */
      attr="";
    }
  }else{
    attr="";
    if(!strcmp(name,textname)) attr=textattr;
    if(!strcmp(name,dataname)) attr=dataattr;
    if(!strcmp(name,sdataname)) attr=sdataattr;
    if(!strcmp(name,sdata2name)) attr=sdata2attr;
    if(!strcmp(name,rodataname)) attr=rodataattr;
    if(!strcmp(name,bssname)) attr=bssattr;
    if(!strcmp(name,sbssname)) attr=sbssattr;
    if(!strcmp(name,tocdname)) attr=tocdattr;
    if(!strcmp(name,dpagename)) attr=dpageattr;
  }

  sec=new_section(name,attr,1);
  sec->memattr=mem;
  set_section(sec);
  eol(s);
}

static void handle_pushsection(char *s)
{
  push_section();
  handle_section(s);
}

static void handle_popsection(char *s)
{
  pop_section();
  eol(s);
}

static void handle_org(char *s)
{
  if (gas_compat && current_section==NULL)
    default_section();  /* .org is always section-offset in gas */

  if (*s == current_pc_char) {    /*  "* = * + <expr>" reserves bytes */
    s = skip(s+1);
    if (*s == '+') {
      add_atom(0,new_space_atom(parse_expr_tmplab(&s),1,0));
    }
    else {
      syntax_error(18);  /* syntax error */
      return;
    }
  }
  else if (current_section!=NULL && !(current_section->flags & ABSOLUTE)) {
    /* .org inside a section is treated as an offset */
    expr *offs = parse_expr_tmplab(&s);
    expr *fill;

    s = skip(s);
    if (*s == ',') {
      /* ...and may be followed by an optional fill-value */
      s = skip(s+1);
      fill = parse_expr_tmplab(&s);
    }
    else fill = NULL;
    add_atom(0,new_roffs_atom(offs,fill));
  }
  else
    set_section(new_org(parse_constexpr(&s)));
  eol(s);
}

static void handle_file(char *s)
{
  char *name;
  if(*s!='\"'){
    general_error(6,'\"');  /* quote expected */
    return;
  }
  name=++s;
  while(*s&&*s!='\"')
    s++;
  if(*s!='\"')
    general_error(6,'\"');  /* quote expected */
  name=cnvstr(name,s-name);
  setfilename(name);
  eol(++s);
}

static int oplen(char *e,char *s)
{
  while(s!=e&&isspace((unsigned char)e[-1]))
    e--;
  return e-s;
}

static void handle_data(char *s,int size,int noalign)
{
  for (;;){
    char *opstart=s;
    operand *op;
    dblock *db=NULL;

    if(OPSZ_BITS(size)==size&&*s=='\"'){
      if(db=parse_string(&opstart,*s,size)){
        add_atom(0,new_data_atom(db,1));
        s=opstart;
      }
    }
    if(!db){
      op=new_operand();
      s=skip_operand(s);
      if(parse_operand(opstart,s-opstart,op,DATA_OPERAND(size))) {
        atom *a;

        a=new_datadef_atom(OPSZ_BITS(size),op);
        if(!align_data||noalign)
          a->align=1;
        add_atom(0,a);
      }else
        syntax_error(8);  /* invalid data operand */
    }

    s=skip(s);
    if(*s==',') {
      s=skip(s+1);
    }else if(ISEOL(s)){
      break;
    }else{
      general_error(6,',');  /* comma expected */
      return;
    }
  }

  eol(s);
}

static void do_equ(char *s,int equiv)
{
  strbuf *labname;

  if(!(labname=parse_identifier(0,&s))){
    syntax_error(10);  /* identifier expected */
    return;
  }
  s=skip(s);
  if(*s!=',')
    general_error(6,',');  /* comma expected */
  else
    s=skip(s+1);
  if(equiv) check_symbol(labname->str);
  new_abs(labname->str,parse_expr_tmplab(&s));
  eol(s);
}

static void handle_set(char *s)
{
  do_equ(s,0);
}

static void handle_equiv(char *s)
{
  do_equ(s,1);
}

static void do_binding(char *s,int bind)
{
  symbol *sym;
  strbuf *name;

  while(1){
    if(!(name=parse_identifier(0,&s))){
      syntax_error(10);  /* identifier expected */
      return;
    }
    sym=new_import(name->str);
    if(((sym->flags&(EXPORT|WEAK|LOCAL))!=0 &&
        (sym->flags&(EXPORT|WEAK|LOCAL))!=bind)
       || ((sym->flags&COMMON) && bind==LOCAL))
      general_error(62,sym->name,get_bind_name(sym)); /* binding already set */
    else
      sym->flags|=bind;
    s=skip(s);
    if(*s!=',')
      break;
    s=skip(s+1);
  }
  eol(s);
}

static void handle_global(char *s)
{
  do_binding(s,EXPORT);
}

static void handle_weak(char *s)
{
  do_binding(s,WEAK);
}

static void handle_local(char *s)
{
  do_binding(s,LOCAL);
}

static void do_align(taddr align,size_t width,expr *fill,taddr max)
{
  atom *a = new_space_atom(number_expr(0),width,fill);

  a->align = align;
  a->content.sb->maxalignbytes = max;
  add_atom(0,a);
}

static void alignment(char *s,int mode,size_t patwidth)
{
  int align,max=0;
  expr *fill=0;

  align = parse_constexpr(&s);
  s = skip(s);
  if (*s == ',') {
    s = skip(s+1);
    if (*s != ',')
      fill = parse_expr_tmplab(&s);
    s = skip(s);
    if (*s == ',') {
      s = skip(s+1);
      max = parse_constexpr(&s);
    }
  }
  if (!mode)
    mode = CPU_DEF_ALIGN;
  if (mode==2 && align>63)
    syntax_error(23);  /* alignment too big */
  do_align(mode==1?align:(1<<align),patwidth,fill,max);
  eol(s);
}

static void handle_even(char *s)
{
  do_align(2,1,NULL,0);
  eol(s);
}

static void handle_align(char *s)
{
  alignment(s,0,1);
}

static void handle_balign(char *s)
{
  alignment(s,1,1);
}

static void handle_balignw(char *s)
{
  alignment(s,1,2);
}

static void handle_balignl(char *s)
{
  alignment(s,1,4);
}

static void handle_p2align(char *s)
{
  alignment(s,2,1);
}

static void handle_p2alignw(char *s)
{
  alignment(s,2,2);
}

static void handle_p2alignl(char *s)
{
  alignment(s,2,4);
}

static void handle_space(char *s)
{
  expr *space = parse_expr_tmplab(&s);
  expr *fill = 0;

  s = skip(s);
  if (*s == ',') {
    s = skip(s+1);
    fill = parse_expr_tmplab(&s);
  }
  add_atom(0,new_space_atom(space,1,fill));
  eol(s);
}

static void handle_size(char *s)
{
  strbuf *name;
  symbol *sym;

  if(!(name=parse_identifier(0,&s))){
    syntax_error(10);  /* identifier expected */
    return;
  }
  sym=new_import(name->str);
  s=skip(s);
  if(*s==',')
    s=skip(s+1);
  else
    general_error(6,',');  /* comma expected */
  sym->size=parse_expr_tmplab(&s);
  eol(s);
}

static void handle_type(char *s)
{
  strbuf *name;
  symbol *sym;

  if(!(name=parse_identifier(0,&s))){
    syntax_error(10);  /* identifier expected */
    return;
  }
  sym=new_import(name->str);
  s=skip(s);
  if(*s==','){
    s=skip(s+1);
    if(!strncmp(s,"@object",7)){
      sym->flags|=TYPE_OBJECT;
      s=skip(s+7);
    }else if(!strncmp(s,"@function",9)){
      sym->flags|=TYPE_FUNCTION;
      s=skip(s+9);
    }else{
      uint32_t t=parse_constexpr(&s)&TYPE_MASK;
      if (t<=TYPE_LAST)
        sym->flags|=t;
    }
  }else
    general_error(6,',');  /* comma expected */
  eol(s);
}

static void new_bss(char *s,int global)
{
  strbuf *name;
  symbol *sym;
  atom *a;
  taddr size;
  section *bss;

  if(!(name=parse_identifier(0,&s))){
    syntax_error(10);  /* identifier expected */
    return;
  }
  size=comma_constexpr(&s);
  if(size<=sdlimit){
    if(!(bss=find_section(sbssname,sbssattr)))
      bss=new_section(sbssname,sbssattr,1);
  }
  else{
    if(!(bss=find_section(bssname,bssattr)))
      bss=new_section(bssname,bssattr,1);
  }
  sym=new_labsym(bss,name->str);
  sym->flags|=TYPE_OBJECT;
  if(global) sym->flags|=EXPORT;
  sym->size=number_expr(size);
  s=skip(s);
  if(*s==','){
    s=skip(s+1);
    sym->align=parse_constexpr(&s);
  }
  else
    sym->align=DATA_ALIGN(size*BITSPERBYTE);
  a=new_label_atom(sym);
  if(sym->align)
    a->align=sym->align;
  add_atom(bss,a);
  a=new_space_atom(number_expr(size),1,0);
  if(sym->align)
    a->align=sym->align;
  add_atom(bss,a);
  eol(s);
}

static void handle_comm(char *s)
{
  strbuf *name;
  char *start=s;
  symbol *sym;

  if (alloccommon){
    new_bss(s,1);
    return;
  }
  if(!(name=parse_identifier(0,&s))){
    syntax_error(10);  /* identifier expected */
    return;
  }
  if ((sym=find_symbol(name->str))&&(sym->flags&LOCAL)) {
    new_bss(start,0);  /* symbol is local, make it .lcomm instead */
    return;
  }

  sym=new_import(name->str);
  s=skip(s);
  if(*s==',')
    s=skip(s+1);
  else
    general_error(6,',');  /* comma expected */
  if (!(sym->size=parse_expr(&s)))
    return;
  simplify_expr(sym->size);
  if(sym->size->type!=NUM){
    general_error(30);
    return;
  }
  sym->flags|=COMMON|TYPE_OBJECT;
  s=skip(s);
  if(*s==','){
    s=skip(s+1);
    sym->align=parse_constexpr(&s);
  }
  else
    sym->align=DATA_ALIGN((int)sym->size->c.val*BITSPERBYTE);
  eol(s);
} 

static void handle_lcomm(char *s)
{
  new_bss(s,0);
} 

static expr *read_stabexp(char **s)
{
  *s = skip(*s);
  if (**s == ',') {
    *s = skip(*s+1);
    return parse_expr(s);
  }
  general_error(6,',');  /* comma expected */
  return NULL;
}

static void handle_stabs(char *s)
{
  char *name;
  size_t len;
  int t,o,d;

  if (*s == '\"') {
    skip_string(s,*s,&len);
    name = mymalloc(len+1);
    s = read_string(name,s,*s,8);
    name[len] = '\0';
  }
  else {
    general_error(6,'\"');  /* quote expected */
    return;
  }
  t = comma_constexpr(&s);
  o = comma_constexpr(&s);
  d = comma_constexpr(&s);
  add_atom(0,new_nlist_atom(name,t,o,d,read_stabexp(&s)));
  eol(s);
}

static void handle_stabn(char *s)
{
  int t,o,d;

  t = parse_constexpr(&s);
  o = comma_constexpr(&s);
  d = comma_constexpr(&s);
  add_atom(0,new_nlist_atom(NULL,t,o,d,read_stabexp(&s)));
  eol(s);
}

static void handle_stabd(char *s)
{
  int t,o,d;

  t = parse_constexpr(&s);
  o = comma_constexpr(&s);
  d = comma_constexpr(&s);
  add_atom(0,new_nlist_atom(NULL,t,o,d,NULL));
  eol(s);
}

static void handle_incdir(char *s)
{
  strbuf *name;

  if (name = parse_name(0,&s))
    new_include_path(name->str);
  eol(s);
}

static void handle_include(char *s)
{
  strbuf *name;

  if (name = parse_name(0,&s)) {
    eol(s);
    include_source(name->str);
  }
}

static void handle_incbin(char *s)
{
  strbuf *name;

  if (name = parse_name(0,&s)) {
    eol(s);
    include_binary_file(name->str,0,0);
  }
}

static void handle_rept(char *s)
{
  utaddr cnt = parse_constexpr(&s);

  eol(s);
  new_repeat(cnt,NULL,NULL,
             dotdirs?drept_dirlist:rept_dirlist,
             dotdirs?dendr_dirlist:endr_dirlist);
}

static void do_irp(int type,char *s)
{
  strbuf *name;

  if(!(name=parse_identifier(0,&s))){
    syntax_error(10);  /* identifier expected */
    return;
  }
  s=skip(s);
  if (*s==',')
    s=skip(s+1);
  new_repeat(type,name->str,mystrdup(s),
             dotdirs?drept_dirlist:rept_dirlist,
             dotdirs?dendr_dirlist:endr_dirlist);
}

static void handle_irp(char *s)
{
  do_irp(REPT_IRP,s);
}

static void handle_irpc(char *s)
{
  do_irp(REPT_IRPC,s);
}

static void handle_endr(char *s)
{
  syntax_error(12,endrname,reptname);  /* unexpected endr without rept */
}

static void handle_macro(char *s)
{
  strbuf *name;

  if(name = parse_identifier(0,&s)){
    s=skip(s);
    if(ISEOL(s))
      s=NULL;
    new_macro(name->str,dotdirs?dmacro_dirlist:macro_dirlist,
              dotdirs?dendm_dirlist:endm_dirlist,s);
  }else
    syntax_error(10);  /* identifier expected */
}

static void handle_endm(char *s)
{
  syntax_error(12,endmname,".macro");  /* unexpected endm without macro */
}

static void ifdef(char *s,int b)
{
  char *name;
  symbol *sym;
  int result;

  if (!(name = parse_symbol(&s))) {
    syntax_error(10);  /* identifier expected */
    return;
  }
  if (sym = find_symbol(name))
    result = sym->type != IMPORT;
  else
    result = 0;
  cond_if(result == b);
  eol(s);
}

static void handle_ifd(char *s)
{
  ifdef(s,1);
}

static void handle_ifnd(char *s)
{
  ifdef(s,0);
}

static void handle_ifb(char *s)
{
  s = skip(s);
  cond_if(ISEOL(s));
}

static void handle_ifnb(char *s)
{
  s = skip(s);
  cond_if(!ISEOL(s));
}

static void ifc(char *s,int b)
{
  strbuf *str1,*str2;

  str1 = parse_name(0,&s);
  if (*s == ',') {
    s = skip(s+1);
    str2 = parse_name(1,&s);
    cond_if((!strcmp(str1?str1->str:emptystr,str2?str2->str:emptystr)) == b);
  }
  else
    syntax_error(5);  /* missing operand */
}

static void handle_ifc(char *s)
{
  ifc(s,1);
}

static void handle_ifnc(char *s)
{
  ifc(s,0);
}

static int eval_ifexp(char **s,int c)
{
  expr *condexp = parse_expr_tmplab(s);
  taddr val;
  int b = 0;

  if (eval_expr(condexp,&val,NULL,0)) {
    switch (c) {
      case 0: b = val == 0; break;
      case 1: b = val != 0; break;
      case 2: b = val > 0; break;
      case 3: b = val >= 0; break;
      case 4: b = val < 0; break;
      case 5: b = val <= 0; break;
      default: ierror(0); break;
    }
  }
  else
    general_error(30);  /* expression must be constant */
  free_expr(condexp);
  return b;
}

static void ifexp(char *s,int c)
{
  cond_if(eval_ifexp(&s,c));
  eol(s);
}

static void handle_ifeq(char *s)
{
  ifexp(s,0);
}

static void handle_ifne(char *s)
{
  ifexp(s,1);
}

static void handle_ifgt(char *s)
{
  ifexp(s,2);
}

static void handle_ifge(char *s)
{
  ifexp(s,3);
}

static void handle_iflt(char *s)
{
  ifexp(s,4);
}

static void handle_ifle(char *s)
{
  ifexp(s,5);
}

static void handle_else(char *s)
{
  eol(s);
  cond_skipelse();
}

static void handle_elseif(char *s)
{
  cond_skipelse();
}

static void handle_endif(char *s)
{
  eol(s);
  cond_endif();
}

static void handle_bsss(char *s)
{
  s=skip(s);
  if(!ISEOL(s)){
    new_bss(s,0);
  }else{
    handle_section(bssname);
    eol(s);
  }
}

static void handle_8bit(char *s){ handle_data(s,8,0); }
static void handle_16bit(char *s){ handle_data(s,16,0); }
static void handle_32bit(char *s){ handle_data(s,32,0); }
static void handle_64bit(char *s){ handle_data(s,64,0); }
static void handle_taddr(char *s){ handle_data(s,bytespertaddr*BITSPERBYTE,0); }
static void handle_16bit_noalign(char *s){ handle_data(s,16,1); }
static void handle_32bit_noalign(char *s){ handle_data(s,32,1); }
static void handle_64bit_noalign(char *s){ handle_data(s,64,1); }
#if FLOAT_PARSER
static void handle_single(char *s){ handle_data(s,OPSZ_FLOAT|32,0); }
static void handle_double(char *s){ handle_data(s,OPSZ_FLOAT|64,0); }
#endif
#if VASM_CPU_OIL
static void handle_string(char *s){ handle_data(s,8,0); }
#else
static void handle_string(char *s)
{
  handle_data(s,8,0);
  add_atom(0,new_space_atom(number_expr(1),1,0));  /* terminating zero */
}
#endif
static void handle_texts(char *s){ handle_section(textname);eol(s);}
static void handle_datas(char *s){ handle_section(dataname);eol(s);}
static void handle_sdatas(char *s){ handle_section(sdataname);eol(s);}
static void handle_sdata2s(char *s){ handle_section(sdata2name);eol(s);}
static void handle_rodatas(char *s){ handle_section(rodataname);eol(s);}
static void handle_sbsss(char *s){ handle_section(sbssname);eol(s);}
static void handle_tocds(char *s){ handle_section(tocdname);eol(s);}
static void handle_dpages(char *s){ handle_section(dpagename);eol(s);}

static void handle_abort(char *s)
{
  syntax_error(11);
  parse_end = 1;
}

static void handle_err(char *s)
{
  add_atom(0,new_assert_atom(NULL,NULL,mystrdup(s)));
}

static void handle_fail(char *s)
{
  expr *condexp = parse_expr_tmplab(&s);
  taddr val;

  if (eval_expr(condexp,&val,NULL,0)) {
    if (val >= 500)
      syntax_error(21,(long long)val);
    else
      syntax_error(22,(long long)val);
  }
  else
    general_error(30);  /* expression must be constant */
  eol(s);
}

static void handle_vdebug(char *s)
{
  add_atom(0,new_atom(VASMDEBUG,0));
}

static void handle_title(char *s)
{
  char *t;
  s=skip(s);
  if(*s!='\"')
    general_error(6,'\"');  /* quote expected */
  else
    s++;
  t=s;
  while(*s&&*s!='\"')
    s++;
  set_list_title(t,s-t);
  if(*s!='\"')
    general_error(6,'\"');  /* quote expected */
  else
    s++;
  eol(s);
}

static void handle_ident(char *s)
{
  strbuf *name;

  if(name=parse_name(0,&s))
    setfilename(mystrdup(name->str));
  eol(s);
}

static void handle_list(char *s)
{
  set_listing(1);
}

static void handle_nolist(char *s)
{
  del_last_listing();  /* hide directive in listing */
  set_listing(0);
}

static void handle_swbeg(char *s)
{
  /* gas emits no code here? Ignore...? */
}

struct {
  const char *name;
  void (*func)(char *);
} directives[]={ /* NOTE: Conditional if-directives first, followed by endif */
  "ifdef",handle_ifd,
  "ifndef",handle_ifnd,
  "ifb",handle_ifb,
  "ifnb",handle_ifnb,
  "ifc",handle_ifc,
  "ifnc",handle_ifnc,
  "if",handle_ifne,
  "ifeq",handle_ifeq,
  "ifne",handle_ifne,
  "ifgt",handle_ifgt,
  "ifge",handle_ifge,
  "iflt",handle_iflt,
  "ifle",handle_ifle,
  "endif",handle_endif,
  "else",handle_else,
  "elseif",handle_elseif,
  "org",handle_org,
  "section",handle_section,
  "pushsection",handle_pushsection,
  "popsection",handle_popsection,
  "string",handle_string,
  "byte",handle_8bit,
  "ascii",handle_8bit,
  "asciz",handle_string,
  "short",handle_16bit,
  "half",handle_16bit,
  "word",handle_16bit,
  "int",handle_taddr,
  "long",handle_32bit,
  "quad",handle_64bit,
  "2byte",handle_16bit_noalign,
  "uahalf",handle_16bit_noalign,
  "4byte",handle_32bit_noalign,
  "uaword",handle_16bit_noalign,
  "ualong",handle_32bit_noalign,
  "8byte",handle_64bit_noalign,
  "uaquad",handle_64bit_noalign,
#if FLOAT_PARSER
  "float",handle_single,
  "single",handle_single,
  "double",handle_double,
#endif
  "text",handle_texts,
  "data",handle_datas,
  "bss",handle_bsss,
  "rodata",handle_rodatas,
  "sdata",handle_sdatas,
  "sdata2",handle_sdata2s,
  "sbss",handle_sbsss,
  "tocd",handle_tocds,
  "dpage",handle_dpages,
  "equ",handle_set,
  "set",handle_set,
  "equiv",handle_equiv,
  "global",handle_global,
  "globl",handle_global,
  "extern",handle_global,
  "weak",handle_weak,
  "local",handle_local,
  "even",handle_even,
  "align",handle_align,
  "balign",handle_balign,
  "balignw",handle_balignw,
  "balignl",handle_balignl,
  "p2align",handle_p2align,
  "p2alignw",handle_p2alignw,
  "p2alignl",handle_p2alignl,
  "space",handle_space,
  "skip",handle_space,
  "zero",handle_space,
  "comm",handle_comm,
  "lcomm",handle_lcomm,
  "size",handle_size,
  "type",handle_type,
  "file",handle_file,
  "stabs",handle_stabs,
  "stabn",handle_stabn,
  "stabd",handle_stabd,
  "incdir",handle_incdir,
  "include",handle_include,
  "incbin",handle_incbin,
  "rept",handle_rept,
  "irp",handle_irp,
  "irpc",handle_irpc,
  "endr",handle_endr,
  "macro",handle_macro,
  "endm",handle_endm,
  "abort",handle_abort,
  "err",handle_err,
  "fail",handle_fail,
  "title",handle_title,
  "ident",handle_ident,
  "list",handle_list,
  "nolist",handle_nolist,
  "swbeg",handle_swbeg,
  "vdebug",handle_vdebug,
};

int dir_cnt=sizeof(directives)/sizeof(directives[0]);

/* checks for a valid directive, and return index when found, -1 otherwise */
static int check_directive(char **line)
{
  char *s,*name;
  hashdata data;

  s = skip(*line);
  if (!ISIDSTART(*s))
    return -1;
  name = s++;
  while (ISIDCHAR(*s))
    s++;
  if (*name == '.')
    name++;
  else if (dotdirs)
    return -1;
  if (!find_namelen_nc(dirhash,name,s-name,&data))
    return -1;
  *line = s;
  return data.idx;
}

/* Handles assembly directives; returns non-zero if the line
   was a directive. */
static int handle_directive(char *line)
{
  int idx = check_directive(&line);

  if (idx >= 0) {
    directives[idx].func(skip(line));
    return 1;
  }
  return 0;
}

static char *parse_stdlabel(char **start)
{
  char *s,*name;

  s = skip(*start);
  if (isdigit((unsigned char)s[0]) && s[1]==':') {
    /* redefinable local label 0 .. 9 */
    strbuf *buf;
    char serno[16];

    buf = make_local_label(0,s,1,serno,
                           sprintf(serno,"%u",++local_labno[*s-'0']));
    name = buf->str;
    *start = s+2;
  }
  else
    name = parse_labeldef(start,1);
  return name;
}

void parse(void)
{
  char *s,*line,*ext[MAX_QUALIFIERS?MAX_QUALIFIERS:1],*op[MAX_OPERANDS];
  char *labname,*start;
  int inst_len,ext_len[MAX_QUALIFIERS?MAX_QUALIFIERS:1],op_len[MAX_OPERANDS];
  int ext_cnt,op_cnt;
  instruction *ip;

  while (line=read_next_line()){
    if (parse_end)
      continue;

    /* # is always allowed as a comment at the beginning of a line */
    s = skip(line);
    if (*s == '#')
      continue;

    if (!cond_state()) {
      /* skip source until ELSE or ENDIF */
      int idx;

      s = line;
      (void)parse_stdlabel(&s);  /* skip label field */
      idx = check_directive(&s);
      if (idx>=0) {
        if (idx<dir_endif)
          cond_skipif();  /* if... */
        else if (idx==dir_else)
          cond_else();
        else if (idx==dir_endif)
          cond_endif();
        else if (idx==dir_elif) {
          s = skip(s);
          cond_elseif(eval_ifexp(&s,1));
        }
      }
      continue;
    }

    s=skip(line);

    if(handle_directive(s))
      continue;

    /* skip spaces */
    s=skip(s);
    if(ISEOL(s))
      continue;

    if(labname=parse_stdlabel(&s)){
      /* we have found a valid global or local label */
      add_atom(0,new_label_atom(new_labsym(0,labname)));
      s=skip(s);
    }

    if(ISEOL(s))
      continue;

    s=skip(parse_cpu_special(s));
    if(ISEOL(s))
      continue;

    if(handle_directive(s))
      continue;

    /* read mnemonic name */
    start=s;
    ext_cnt=0;
    if(!ISIDSTART(*s)){
      syntax_error(10);
      continue;
    }
#if MAX_QUALIFIERS==0
    while(*s&&!isspace((unsigned char)*s))
      s++;
    inst_len=s-start;
#else
    s=parse_instruction(s,&inst_len,ext,ext_len,&ext_cnt);
#endif
    s=skip(s);

    if(execute_macro(start,inst_len,ext,ext_len,ext_cnt,s))
      continue;

    /* read operands, terminated by comma (unless in parentheses)  */
    op_cnt=0;
    while(!ISEOL(s)&&op_cnt<MAX_OPERANDS){
      op[op_cnt]=s;
      s=skip_operand(s);
      op_len[op_cnt]=oplen(s,op[op_cnt]);
#if !ALLOW_EMPTY_OPS
      if(op_len[op_cnt]<=0)
        syntax_error(5);
      else
#endif
        op_cnt++;
      s=skip(s);
      if(*s!=','){
        break;
      }else{
        s=skip(s+1);
      }
    }      
    s=skip(s);
    if(!ISEOL(s)) syntax_error(6);
    ip=new_inst(start,inst_len,op_cnt,op,op_len);
#if MAX_QUALIFIERS>0
    if(ip){
      int i;
      for(i=0;i<ext_cnt;i++)
        ip->qualifiers[i]=cnvstr(ext[i],ext_len[i]);
      for(;i<MAX_QUALIFIERS;i++)
        ip->qualifiers[i]=0;
    }
#endif
    if(ip){
#if MAX_OPERANDS>0
      if (ip->op[0]==NULL&&op_cnt!=0)
        syntax_error(6);  /* mnemonic without operands has tokens in op.field */
#endif
      add_atom(0,new_inst_atom(ip));
    }
  }

  cond_check();
}

/* get defaults and qualifiers for a macro argument name specifier */
char *macro_arg_opts(macro *m,int argno,char *name,char *s)
{
  int req = 0;
  char *end;
  char *new = NULL;

  if (*s==':' && (end=skip_identifier(s+1))!=NULL) {
    if (end-s==4 && !strnicmp(s+1,"req",3)) {
      /* required argument: argname:req */
      req = 1;
      new = s = skip(s+4);
    }
    else if (end-s==7 && !strnicmp(s+1,"vararg",6)) {
      /* define vararg position: argname:vararg */
      m->vararg = argno;
      new = s = skip(s+7);
    }
  }
  if (*s == '=') {
    /* define a default value for this argument */
    s = skip(s+1);
    if (end = skip_macroparam(s)) {
      if (req)
        syntax_error(13,name);  /* pointless default value for req. parameter */
      addmacarg(&m->defaults,s,end);
      return end;
    }
  }
  if (new) {
    if (req)
      addmacarg(&m->defaults,NULL,NULL);  /* mark as required - no default! */
    else
      addmacarg(&m->defaults,new,new);  /* empty string as default */
  }
  return new;  /* NULL means: set an empty string as default value */
}

/* parse next macro argument */
char *parse_macro_arg(struct macro *m,char *s,
                      struct namelen *param,struct namelen *arg)
{
  char *idend = skip_identifier(s);

  arg->len = 0;
  if (idend!=NULL && idend-s>0) {
    char *end = skip(idend);

    if (*end++ == '=') {
      /* argument selected by keyword */
      arg->name = s;
      arg->len = idend - s;
      s = skip(end);
    }
  }

  if (*s == '\'') {
    /* evaluate character constant */
    unsigned long cc = parse_constexpr(&s);
    char buf[64],*ccstr;
    int len;

    if ((len = sprintf(buf,"%lu",cc)) < 0)
      ierror(0);
    ccstr = mystrdup(buf);
    param->name = ccstr;
    param->len = len;
  }
  else if (*s == '\"') {
    /* pass everything enclosed in quotes, but without the quotes */
    param->name = ++s;
    while (*s!='\0' && *s!='\"') s++;
    param->len = s - param->name;
    if (*s == '\"')
      s++;
    else
      general_error(6,'\"');  /* " expected */
  }
  else {
    param->name = s;
    s = skip_macroparam(s);
    param->len = s - param->name;
  }
  return s;
}

/* expands arguments and special escape codes into macro context */
int expand_macro(source *src,char **line,char *d,int dlen)
{
  int n,nc=0;
  char *end,*s=*line;

  if (*s++ == '\\') {
    /* possible macro expansion detected */
    if (*s == '@') {
      /* \@: insert a unique id */
      s++;
      nc = sprintf(d,"%lu",src->id);
      if (nc >= dlen)
        nc = -1;
    }
    else if (*s=='(' && *(s+1)==')') {
      /* \() is just skipped, useful to terminate named macro parameters */
      nc = 0;
      s += 2;
    }
    else if ((end = skip_identifier(s)) != NULL) {
      if ((n = find_macarg_name(src,s,end-s)) >= 0) {
        /* \argname: insert named macro parameter n */
        nc = copy_macro_param(src,n,d,dlen);
        s = end;
      }
    }
    if (nc >= 0)
      *line = s;  /* update line pointer when expansion took place */
  }           
  return nc;  /* number of chars written to line buffer, -1: out of space */
}

void my_exec_macro(source *src)
{
#if 0 /* make it possible to use default values, when params are missing */
  if (src->macro->num_argnames>=0 && src->num_params<src->macro->num_argnames)
    general_error(24);  /* missing macro parameters (named) */
#endif
}

char *const_prefix(char *s,int *base)
{
  if(!isdigit((unsigned char)*s)){
    *base=0;
    return s;
  }
  if(*s=='0'){
    if(s[1]=='x'||s[1]=='X'){
      *base=16;
      return s+2;
    }
    if(s[1]=='b'||s[1]=='B'){
      *base=2;
      return s+2;
    }
    if(isdigit((unsigned char)s[1])){
      *base=8;
      return s;
    }
#if FLOAT_PARSER
    if(s[1]=='d'||s[1]=='D'||s[1]=='f'||s[1]=='F'||s[1]=='r'||s[1]=='R')
      s+=2;  /* floating point is handled automatically, so skip prefix */
#endif
  }
  *base=10;
  return s;
}

char *const_suffix(char *start,char *end)
{
  return end;
}

strbuf *get_local_label(int n,char **start)
/* Local label have digits only. Either they end with '$' or start with
   '.' (not in gas-compatibility mode). Additionally multiple single-
  digit local labels from '0' to '9' are allowed and referenced with
  'Nb' for the previous and 'Nf' for the next definition. */
{
  char *s = *start;
  strbuf *name = NULL;

  if (!gas_compat && *s=='.') {
    s++;
    while (isdigit((unsigned char)*s))
      s++;
    if (s > (*start+1)) {
      name = make_local_label(n,NULL,0,*start,s-*start);
      *start = skip(s);
    }
  }
  else if (isdigit((unsigned char)*s)) {
    s++;
    if ((*s=='b' || *s=='f') && !isdigit((unsigned char)*(s+1))) {
      unsigned serno = local_labno[*(s-1)-'0'];
      char sernostr[16];

      if (*s-- == 'f')
        serno++;
      name = make_local_label(n,s,1,sernostr,sprintf(sernostr,"%u",serno));
      *start = skip(s+2);
    }
    else {
      while (isdigit((unsigned char)*s))
        s++;
      if (*s=='$' && isdigit((unsigned char)*(s-1))) {
        s++;
        name = make_local_label(n,NULL,0,*start,s-*start);
        *start = skip(s);
      }
    }
  }
  return name;
}

int init_syntax(void)
{
  size_t i;
  hashdata data;
  dirhash=new_hashtable(0x1000);
  for(i=0;i<dir_cnt;i++){
    data.idx=i;
    add_hashentry(dirhash,directives[i].name,data);
    if(!strcmp(directives[i].name,"else"))
      dir_else=i;
    if(!strcmp(directives[i].name,"elseif"))
      dir_elif=i;
    if(!strcmp(directives[i].name,"endif"))
      dir_endif=i;
  }
  if(!(dir_else&&dir_elif&&dir_endif))
    ierror(0);
  if(debug && dirhash->collisions)
    fprintf(stderr,"*** %d directive collisions!!\n",dirhash->collisions);

  cond_init();
#if defined(VASM_CPU_X86)
  current_pc_char = '.';
#endif  
  esc_sequences = !noesc;
  nocase_macros = 1;
  return 1;
}

int syntax_defsect(void)
{
  return 0;  /* defaults to .text */
}

int syntax_args(char *p)
{
  int i;

  if (!strcmp(p,"-ac")) {
    alloccommon = 1;
    return 1;
  }
  else if (!strcmp(p,"-align")) {
    align_data = 1;
    return 1;
  }
  else if (!strcmp(p,"-gas")) {
    gas_compat = 1;
    return 1;
  }
  else if (!strcmp(p,"-nodotneeded")) {
    dotdirs = 0;
    return 1;
  }
  else if (!strcmp(p,"-noesc")) {
    noesc = 1;
    return 1;
  }
  else if (!strncmp(p,"-sdlimit=",9)) {
    i = atoi(p+9);
    sdlimit = (i<=0) ? -1 : i;
    return 1;
  }

  return 0;
}
