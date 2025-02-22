#ifndef FILEPARSER_H
#define FILEPARSER_H
#include <stddef.h>
#include <stdint.h>

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
void parse_relasekeyvalues( parserkeyvalue values[], int numberofkeys );
#endif