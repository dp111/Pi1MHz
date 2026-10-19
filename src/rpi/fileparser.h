#ifndef FILEPARSER_H
#define FILEPARSER_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

enum parserkeyvaluetype {
    NUMSTRING,
    STRING,
    INTEGER
};

typedef struct parserkey {
    const char * key;
    int min;
    int max;
    enum parserkeyvaluetype type;
} parserkey;


typedef struct  {
    size_t length;
    union {
        int * integer;
        char * string;
    } v;
} parserkeyvalue;

int parse_findindex( const char * searchkey, const parserkey array[]);
int parse_readfile( const char * filename , const char * outfile, const parserkey keyv[], parserkeyvalue values[]);
/* A rewrite (outfile set) that also adds a line at the end for each key with
   append[k] set, a value, and no line in the file - where parse_readfile
   would drop that value.  append[] is indexed as keyv[]. */
int parse_rewritefile_adding( const char * filename , const char * outfile, const parserkey keyv[],
                              parserkeyvalue values[], const bool append[]);
/* Frees every non-NULL v.string in the array - which is every value the
   parser allocated on a read.  Do NOT call it on an array whose values the
   caller filled in for a rewrite: those pointers are the caller's, and
   freeing a string literal or a stack buffer is what it sounds like. */
void parse_releasekeyvalues( parserkeyvalue values[], int numberofkeys );
#endif