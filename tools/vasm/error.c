/* error.c - error output and modification routines */
/* (c) in 2002-2024 by Volker Barthelmann and Frank Wille */

#include <stdarg.h>
#include "vasm.h"
#include "error.h"

static struct err_out general_err_out[]={
#include "general_errors.h"
};
static const int general_errors=sizeof(general_err_out)/sizeof(general_err_out[0]);

static struct err_out syntax_err_out[]={
#include "syntax_errors.h"
};
static const int syntax_errors=sizeof(syntax_err_out)/sizeof(syntax_err_out[0]);

static struct err_out cpu_err_out[]={
#include "cpu_errors.h"
};
static const int cpu_errors=sizeof(cpu_err_out)/sizeof(cpu_err_out[0]);

static struct err_out output_err_out[]={
#include "output_errors.h"
};
static const int output_errors=sizeof(output_err_out)/sizeof(output_err_out[0]);

int errors,warnings;  /* count */

/* options */
int max_errors=5;
int no_warn;


static void print_source_line(FILE *f,source *src,int l)
{
  static char *buf = NULL;
  static size_t bufsz = 0;
  char c,*e,*p,*q;

  /* allocate a sufficiently dimensioned line buffer */
  if (src->bufsize > bufsz) {
    bufsz = src->bufsize;
    buf = myrealloc(buf,bufsz);
  }

  p = src->text;
  q = buf;
  e = buf + bufsz - 1;

  do {
    c = *p++;
    if (c=='\n' || c=='\r') {
      if (*p == ((c=='\n') ? '\r' : '\n'))
        p++;
      if (--l == 0) {
        /* terminate error line in buffer and print it */
        *q = '\0';
        fprintf(f,"%s\n",buf);
        return;
      }
      q = buf;  /* next line, start to fill buffer from the beginning */
    }
    else if (q < e)
      *q++ = c;
  }
  while (*p);
  ierror(0);  /* line doesn't exist */
}


static void print_source_file(FILE *f, source *src)
{
  if (src->srcfile) {
    if (src->srcfile->incpath != NULL)
      fprintf(f,"\"%s%s%s\"",
              src->srcfile->incpath->compdir_based
              ? compile_dir : emptystr,
              src->srcfile->incpath->path,
              src->srcfile->name);
    else
      fprintf(f,"\"%s\"",src->srcfile->name);
  }
  else
    fprintf(f,"\"%s\"",src->name);
}


static void error(int n,va_list vl,struct err_out *errlist,int offset)
{
  static source *last_err_source = NULL;
  static int last_err_no;
  static int last_err_line;
  FILE *f;
  int flags=errlist[n].flags;

  if ((flags&DISABLED) || ((flags&WARNING) && no_warn))
    return;

  if ((flags&MESSAGE) && !(flags&(WARNING|ERROR|FATAL))) {
    if (nostdout)
      return;
    f = stdout;  /* print messages to stdout */
  }
  else {
    f = stderr;  /* otherwise stderr */

    if (last_err_source) {
      /* avoid printing the same error again and again, which might happen
         when a line is evaluated in multiple passes */
      if (cur_src!=NULL && cur_src==last_err_source &&
         cur_src->line==last_err_line &&
         n+offset==last_err_no)
        return;
    }
  }

  if (cur_src) {
    last_err_source = cur_src;
    last_err_line = cur_src->line;
    last_err_no = n + offset;
  }
  fprintf(f,"\n");

  if (cur_listing && (flags & ERROR))
    cur_listing->error = n + offset;

  if (flags & FATAL)
    fprintf(f,"fatal ");
  if (flags & ERROR) {
    ++errors;
    fprintf(f,"error");
  }
  else if (flags & WARNING) {
    ++warnings;
    fprintf(f,"warning");
  }
  else if (flags & MESSAGE)
    fprintf(f,"message");
  fprintf(f," %d",n+offset);
  if (!(flags & NOLINE) && cur_src!=NULL) {
    fprintf(f," in line %d of ",cur_src->line);
    print_source_file(f,cur_src);
    if (cur_src->defsrc) {
      fprintf(f," (line %d of ",cur_src->defline+cur_src->line);
      print_source_file(f,cur_src->defsrc);
      fputc(')',f);
    }
  }
  fprintf(f,": ");
  vfprintf(f,errlist[n].text,vl);
  fprintf(f,"\n");

  if (!(flags & NOLINE) && cur_src!=NULL) {
    if (cur_src->parent != NULL) {
      source *parent,*child;
      int recurs;

      child = cur_src;
      while (parent = child->parent) {
        if (child->srcfile)
          fprintf(f,"\tincluded");  /* included from */
        else if (child->macro)
          fprintf(f,"\tcalled");    /* macro called from */
        else {
          child = parent;           /* skip parent for repetitions */
          continue;
        }
        fprintf(f," from line %d of ",child->parent_line);
        print_source_file(f,parent);
        if (parent->defsrc) {
          fprintf(f," (line %d of ",parent->defline+child->parent_line);
          print_source_file(f,parent->defsrc);
          fputc(')',f);
        }

        recurs = 1;
        while (parent->parent!=NULL &&
               child->parent_line==parent->parent_line &&
               !strcmp(parent->name,parent->parent->name)) {
          recurs++;
          parent = parent->parent;
        }

        if (recurs > 1)
          fprintf(f," %d times",recurs);

        if (child==cur_src && child->macro!=NULL) {
          fprintf(f,": ");
          print_source_line(f,parent,child->parent_line);
        }
        else
          fprintf(f,"\n");
        child = parent;
      }
    }
    fputc('>',f);
    print_source_line(f,cur_src,cur_src->line);
  }

  if (flags & FATAL) {
    fprintf(f,"aborting...\n");
    leave();
  }
  if ((flags & ERROR) && max_errors!=0 && errors>=max_errors) {
    fprintf(f,"***maximum number of errors reached!***\n");
    leave();
  }
}


void general_error(int n,...)
{
  va_list vl;
  va_start(vl,n);
  error(n,vl,general_err_out,FIRST_GENERAL_ERROR);
  va_end(vl);
}


void syntax_error(int n,...)
{
  va_list vl;
  va_start(vl,n);
  error(n,vl,syntax_err_out,FIRST_SYNTAX_ERROR);
  va_end(vl);
}


void cpu_error(int n,...)
{
  va_list vl;
  va_start(vl,n);
  error(n,vl,cpu_err_out,FIRST_CPU_ERROR);
  va_end(vl);
}


void output_error(int n,...)
{
  va_list vl;
  va_start(vl,n);
  error(n,vl,output_err_out,FIRST_OUTPUT_ERROR);
  va_end(vl);
}


void output_atom_error(int n,atom *a,...)
{
  source *old = cur_src;
  va_list vl;

  va_start(vl,a);

  /* temporarily set the source text and line from the given atom */
  if (a != NULL) {
    if ((cur_src = a->src) != NULL)
      cur_src->line = a->line;
  }
  else
    cur_src = NULL;  /* no line information, when atom is missing */

  error(n,vl,output_err_out,FIRST_OUTPUT_ERROR);
  cur_src = old;

  va_end(vl);
}


static void modify_errors(struct err_out *err,int flags,va_list vl)
{
  int n;

  while (n = va_arg(vl,int)) {
    err[n].flags = flags;
  }
}


void modify_gen_err(int flags,...)
{
  va_list vl;
  va_start(vl,flags);
  modify_errors(general_err_out,flags,vl);
  va_end(vl);
}


void modify_syntax_err(int flags,...)
{
  va_list vl;
  va_start(vl,flags);
  modify_errors(syntax_err_out,flags,vl);
  va_end(vl);
}


void modify_cpu_err(int flags,...)
{
  va_list vl;
  va_start(vl,flags);
  modify_errors(cpu_err_out,flags,vl);
  va_end(vl);
}


static void disable(int type,struct err_out *err,int errnum,int first,int max)
{
  int n = errnum-first;

  if (n>=0 && n<max) {
    if (err[n].flags & type) {
      err[n].flags |= DISABLED;
      return;
    }
  }
  general_error(33,errnum,type==WARNING?"warning":emptystr);
}


static void disable_type(int type,int n)
{
  if (n >= FIRST_OUTPUT_ERROR)
    disable(type,output_err_out,n,FIRST_OUTPUT_ERROR,output_errors);
  else if (n >= FIRST_CPU_ERROR)
    disable(type,cpu_err_out,n,FIRST_CPU_ERROR,cpu_errors);
  else if (n >= FIRST_SYNTAX_ERROR)
    disable(type,syntax_err_out,n,FIRST_SYNTAX_ERROR,syntax_errors);
  else if (n >= FIRST_GENERAL_ERROR)
    disable(type,general_err_out,n,FIRST_GENERAL_ERROR,general_errors);
}


void disable_message(int n)
{
  disable_type(MESSAGE,n);
}


void disable_warning(int n)
{
  disable_type(WARNING,n);
}
