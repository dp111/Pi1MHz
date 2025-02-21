/*

Simple file parser

A struct of values are passed in the file is then opened and parsed for the values

The file may be written out with updated values

comments start with a "#"
keys must start at the beginning of the line and with

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>
#include "../beebscsi/filesystem.h"
#include "rpi.h"

enum keyvaluetype {
    NUMSTRING,
    STRING,
    INTEGER
};

struct keyvalue {
    const char * key;
    int min;
    int max;
    enum keyvaluetype type;
};


struct values {
    size_t length;
    union {
        int * integer;
        char * string;
    } v;
};

const struct keyvalue scsiattributes[] = {
    { "Title"           , 0 , 39 , STRING},
    { "Description"     , 0 ,255 , STRING },
    { "Inquiry"         , 0 ,101 , NUMSTRING },
    { "ModeParamHeader" , 0 ,  4 , NUMSTRING },
    { "LBADescriptor"   , 0 ,  8 , NUMSTRING },
    { "ModePage1"       , 0 ,  5 , NUMSTRING},
    { "ModePage3"       , 0 , 21 , NUMSTRING },
    { "ModePage4"       , 0 ,  6 , NUMSTRING},
    { "ModePage32"      , 0 , 10 , NUMSTRING},
    { "ModePage33"      , 0 ,  9 , NUMSTRING },
    { "ModePage35"      , 0 ,  3 , NUMSTRING},
    { "ModePage36"      , 0 ,  4 , NUMSTRING},
    { "ModePage37"      , 0 ,  6 , NUMSTRING},
    { "ModePage38"      , 0 ,  6 , NUMSTRING},
    { "LDUserCode"      , 0 ,  4 , STRING },
    { "LDVideoXoffset"  , -768 , 768 , INTEGER },
    { "" , 0 ,0, 0} // end of list
};

inline int parse_findindex( uint8_t * searchkey, const struct keyvalue array[])
{
    int i = 0;
    while (array[i].key) {
        if (strcasecmp(array[i].key, (char *) searchkey) == 0) {
            return i;
        }
    }
    return -1;
}


// nonnumber = 0 for hex digits, 1 for 0x, -1 for non decimal digits
//
//
static size_t parse_strlen( const uint8_t * buf , size_t ptr, size_t max,  int *nonnumber)
{
    size_t len = 0;
    size_t oldptr = ptr;
    *nonnumber = 0;
    while ((buf[ptr] > ' ') && (buf[ptr] != '#')  && (ptr < max))
        {
            if ( ((buf[ptr] >= '0') && (buf[ptr] <= '9')) ||
                    ((buf[ptr] >= 'A') && (buf[ptr] <= 'F')) ||
                    ((buf[ptr] >= 'a') && (buf[ptr] <= 'f'))
            )
                {}
                else
                {
                    if (buf[ptr] == 'x' || buf[ptr] == 'X')
                        if ((*nonnumber == 0) && ( (ptr-oldptr)==1) )
                            *nonnumber = 1;
                        else
                            *nonnumber = -1;
                    else
                        *nonnumber = -1;
                }
            len++;
            ptr++;
        }
    return len;
}


//
// Parse a file into the key value structure
//
// if outfile filename is set then the keyvalues will be written back to the file.

int parse_readfile( const char * filename , const char * outfile, struct keyvalue keyv[], struct values values[] )
{
// open file for reading
    size_t ptr = 0, outptr = 0;
    uint8_t * buffer = 0 ;
    size_t filesize = filesystemReadFile( filename , &buffer , 0 );
    if (filesize == 0)
        return 0;
    LOG_DEBUG("Parsing %s File size %d\n\r",filename, filesize);
    char * outbuf = malloc( filesize*4); // create out buffer *4 input size should be enough

    while (ptr <filesize)
    {
        // skip white space and blank lines
        while  ((buffer[ptr] <= ' ') && (ptr < filesize))
        // skip a line if it starts with a #
        if (buffer[ptr] == '#')
        {
            while ( ((buffer[ptr] != '\n')||( buffer[ptr] != '\r' )) && ptr < filesize)
                outbuf[outptr++] = buffer[ptr++];
            continue;
        }

        // find the key
        int keyindex = parse_findindex( (buffer + ptr) , keyv );
        if (keyindex == -1)
        {
            // key not found skip line
            LOG_DEBUG("Key not found %s\n", buffer + ptr );
            while ( ((buffer[ptr] != '\n')||( buffer[ptr] != '\r' )) && ptr < filesize)
                outbuf[outptr++] = buffer[ptr++];
            continue;

        }
        else
        {
            // key found
            size_t keylen = strlen(keyv[keyindex].key);
            LOG_DEBUG("Key found %s ", keyv[keyindex].key );
            while (keylen)
                {
                    outbuf[outptr++] = buffer[ptr++];
                    keylen-=1;
                }
            bool flag = false;
            while ( ptr < filesize)
            {
                if ((buffer[ptr] == ' ') || (buffer[ptr] == '\t') || (buffer[ptr] == '='))
                {
                    outbuf[outptr++] = buffer[ptr++];
                    continue;
                }
                if (buffer[ptr] == '#')
                {
                    while ( ((buffer[ptr] != '\n')||( buffer[ptr] != '\r' )) && ptr < filesize)
                        outbuf[outptr++] = buffer[ptr++];
                    break;

                }
                if ((buffer[ptr] == '\n') || ( buffer[ptr] != '\r' ))
                {
                    outbuf[outptr++] = buffer[ptr++];
                    break;
                }
                flag = true;
                break;
            }
            if (flag)
                {
                    if (outbuf)
                    {
                      int nonnumber;
                      size_t len = parse_strlen( buffer , ptr, filesize, &nonnumber);
                      // check if value exists
                      if (values[keyindex].length)
                      {
                        // if so write it out
                        if (keyv[keyindex].type == NUMSTRING)
                        {
                            // write a string number
                            for( size_t i = 0; i < values[keyindex].length; i++)
                                {
                                    char nibble = values[keyindex].v.string[i] >> 4;
                                    if (nibble < 10)
                                        outbuf[outptr++] = nibble + '0';
                                    else
                                        outbuf[outptr++] = nibble + 'A' - 10;
                                    nibble = values[keyindex].v.string[i] & 0x0F;
                                    if (nibble < 10)
                                        outbuf[outptr++] = nibble + '0';
                                    else
                                        outbuf[outptr++] = nibble + 'A' - 10;
                                }
                            ptr += len;
                        }
                        else if (keyv[keyindex].type == STRING)
                        {
                            // write a string
                            for( size_t i = 0; i < values[keyindex].length; i++)
                                outbuf[outptr++] = values[keyindex].v.string[i];
                            ptr += len;
                        }
                        else if (keyv[keyindex].type == INTEGER)
                        {
                            // write a number
                            size_t outlen = (size_t) sprintf( outbuf + outptr , "%d" , *values[keyindex].v.integer);
                            outptr += outlen;
                            ptr += len;
                        }
                       }

                        // copy the rest of the line to the output buffer
                        while ( ((buffer[ptr] != '\n')||( buffer[ptr] != '\r' )) && ptr < filesize)
                            outbuf[outptr++] = buffer[ptr++];


                    } else {
                        int nonnumber;
                        size_t len = parse_strlen( buffer , ptr, filesize, &nonnumber);
                        if (keyv[keyindex].type == NUMSTRING)
                        {
                            if (nonnumber<0)
                            {
                                // error
                                LOG_DEBUG("Error in number format\n\r");
                            }
                            else
                            {
                                if (nonnumber==1)
                                    {
                                     // strip off 0x
                                     ptr+=2;
                                     len-=2;
                                    }
                                // read a string number
                                LOG_DEBUG("Number %s\n\r" , &buffer[ptr]);
                                values[keyindex].v.string = malloc( len/2);
                                for (size_t i = 0; i < len/2; i++)
                                {
                                    if ((buffer[ptr] >= '0') && (buffer[ptr] <= '9'))
                                        values[keyindex].v.string[i] = (char)(buffer[ptr] - '0')<<4;
                                    else if ((buffer[ptr] >= 'A' && buffer[ptr] <= 'F'))
                                        values[keyindex].v.string[i] = (char)(buffer[ptr] - 'A' + 10)<<4;
                                    else if ((buffer[ptr] >= 'a' && buffer[ptr] <= 'f'))
                                        values[keyindex].v.string[i] = (char)(buffer[ptr] - 'a' + 10)<<4;
                                    ptr++;
                                    if ((buffer[ptr] >= '0') && (buffer[ptr] <= '9'))
                                        values[keyindex].v.string[i] += (char)(buffer[ptr] - '0');
                                    else if ((buffer[ptr] >= 'A') && (buffer[ptr] <= 'F'))
                                        values[keyindex].v.string[i] += (char)(buffer[ptr] - 'A' + 10);
                                    else if ((buffer[ptr] >= 'a') && (buffer[ptr] <= 'f'))
                                        values[keyindex].v.string[i] += (char)(buffer[ptr] - 'a' + 10);
                                    ptr++;
                                }

                                values[keyindex].length = len/2;
                            }
                        }
                        else if (keyv[keyindex].type == STRING)
                        {
                            // read a string
                            values[keyindex].v.string = malloc( len + 1);
                            memcpy( values[keyindex].v.string , buffer + ptr , len);
                            values[keyindex].v.string[len] = 0;
                            values[keyindex].length = len;
                            LOG_DEBUG("string %s\n\r" , values[keyindex].v.string);
                            ptr += len;
                        }
                        else if (keyv[keyindex].type == INTEGER)
                        {
                            // read a number
                            values[keyindex].v.integer = malloc( 4);
                            *values[keyindex].v.integer = (int) strtol( (char *)buffer + ptr, 0 , 0);
                            LOG_DEBUG("number %d\n\r" , *values[keyindex].value);
                            values[keyindex].length = 1;
                            ptr += len;
                        }

                        // now seach for the end of the line
                        while ( ((buffer[ptr] != '\n')||( buffer[ptr] != '\r' )) && (ptr < filesize))
                            outbuf[outptr++] = buffer[ptr++];
                }
            } else
            {
                LOG_DEBUG("Key data not found \n\r" );
            }
        }
    }

    free(buffer);

    if (outfile)
    {
    // write out file

    }
    free(outbuf);

    return 1;
}

void parse_relasekeyvalues( struct values values[], int numberofkeys )
{
    for (int i = 0; i < numberofkeys ; i++)
    {
        if (values[i].v.string != NULL)
        {
            free(values[i].v.string);
            values[i].length = 0;
        }
    }
}