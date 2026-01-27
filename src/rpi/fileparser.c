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
#include <ctype.h>
#include "../BeebSCSI/filesystem.h"
#include "rpi.h"
#include "fileparser.h"

/* Compare S1 and S2, ignoring case, returning less than, equal to or
   greater than zero if S1 is lexicographically less than,
   equal to or greater than S2.  */
static bool localstrcasecmp(const char *s1, const char *s2 )
{
    while(1)
    {
        //printf("compare : %c %c\n\r",*s1,*s2);
        if ((*s1 == '\0') && ((*s2 == '\0') || (*s2 == '\n') || (*s2 == '\r') || (*s2 == ' ') || (*s2 == '\t') || (*s2 == '#') || (*s2 == '=')))
            return true;
        if ( tolower(*s1++) != tolower(*s2++) )
            return false;
    }
}

int parse_findindex( const char * searchkey, const parserkey array[])
{
    int i = 0;
    while (array[i].key) {
        if (localstrcasecmp(array[i].key, searchkey)) {
            return i;
        }
        i++;
    }
    LOG_DEBUG("Key not found %s\n\r", searchkey);
    return -1;
}


// nonnumber = 0 for hex digits, 1 for 0x, -1 for non decimal digits
// returns the length of the string
//
static size_t parse_numstrlen( const uint8_t * buf , size_t ptr, size_t max,  int *nonnumber)
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

// length of string till end of line or #
//
//
static size_t parse_strlen( const uint8_t * buf , size_t ptr, size_t max)
{
    size_t len = 0;
    while ((buf[ptr] >= ' ') && (buf[ptr] != '#')  && (ptr < max))
        {
            len++;
            ptr++;
        }
    return len;
}


//
// Parse a file into the key value structure
//
// if outfile filename is set then the keyvalues will be written back to the file.

int parse_readfile( const char * filename , const char * outfile, const parserkey keyv[], parserkeyvalue values[])
{
// open file for reading
    size_t ptr = 0, outptr = 0;
    uint8_t * buffer = 0 ;
    size_t filesize = filesystemReadFile( filename , &buffer , 0 );
    if (filesize == 0)
        return 0;
    LOG_DEBUG("Parsing %s File size %u\n\r",filename, filesize);
    char * outbuf = malloc( filesize*4); // create out buffer *4 input size should be enough

    if (outbuf == NULL)
        return 0;

    while (ptr <filesize)
    {
        // skip white space and blank lines
        while  ((ptr < filesize) && (buffer[ptr] <= ' ') )
            outbuf[outptr++] = buffer[ptr++];

        if (ptr >= filesize)
            break;

        // skip a line if it starts with a #
        if (buffer[ptr] == '#')
        {
            while ( (ptr < filesize) && ((buffer[ptr] != '\n') && ( buffer[ptr] != '\r' )) )
                outbuf[outptr++] = buffer[ptr++];
            continue;
        }

        // find the key
        int keyindex = parse_findindex(( const char *) (buffer + ptr) , keyv );
        if (keyindex == -1)
        {
            // key not found skip line
            LOG_DEBUG("Key not found\n\r" );
            while ( (ptr < filesize) && ((buffer[ptr] != '\n') && ( buffer[ptr] != '\r' )) )
                outbuf[outptr++] = buffer[ptr++];
            continue;
        }
        else
        {
            // key found
            size_t keylen = strlen(keyv[keyindex].key);
            LOG_DEBUG("Key found %s \r\n", keyv[keyindex].key );
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
                    while ( (ptr < filesize) && ((buffer[ptr] != '\n') && ( buffer[ptr] != '\r' )) )
                        outbuf[outptr++] = buffer[ptr++];
                    break;
                }
                if ((buffer[ptr] == '\n') || ( buffer[ptr] == '\r' ))
                {
                    outbuf[outptr++] = buffer[ptr++];
                    break;
                }
                flag = true;
                break;
            }
            if (flag)
                {
                    if (outfile)
                    {
                      int nonnumber;
                      // check if value exists
                      if (values[keyindex].length)
                      {

                        // if so write it out
                        if (keyv[keyindex].type == NUMSTRING)
                        {
                            size_t len = parse_numstrlen( buffer , ptr, filesize, &nonnumber);
                            // write a string number
                            for( size_t i = 0; i < values[keyindex].length; i++)
                                {
                                    char nibble = (values[keyindex].v.string[i] >> 4) & 0x0F;
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
                            size_t len = parse_strlen( buffer , ptr, filesize);
                            // write a string
                            for( size_t i = 0; i < values[keyindex].length; i++)
                                outbuf[outptr++] = values[keyindex].v.string[i];
                            ptr += len;
                        }
                        else if (keyv[keyindex].type == INTEGER)
                        {
                            size_t len = parse_strlen( buffer , ptr, filesize);
                            // write a number
                            size_t remaining = (filesize * 4) - outptr;
                            size_t outlen = (size_t) snprintf( outbuf + outptr , remaining , "%d" , *values[keyindex].v.integer);
                            if (outlen > 0 && outlen < remaining) outptr += outlen;
                            ptr += len;
                        }
                       }

                        // copy the rest of the line to the output buffer
                        while ( (ptr < filesize) && ((buffer[ptr] != '\n') && ( buffer[ptr] != '\r' )))
                            outbuf[outptr++] = buffer[ptr++];


                    } else {
                        int nonnumber;

                        if (keyv[keyindex].type == NUMSTRING)
                        {
                            size_t len = parse_numstrlen( buffer , ptr, filesize, &nonnumber);
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
                                if (len%2)
                                    {
                                        // odd number of digits error
                                        LOG_DEBUG("Error odd number of digits in hex string\n\r");
                                        len++;
                                        ptr--;
                                        buffer[ptr]='0'; // pad with a 0
                                    }
                                // read a string number
                              //  LOG_DEBUG("Number %s\n\r" , &buffer[ptr]);
                                values[keyindex].v.string = malloc( len/2);
                                if (values[keyindex].v.string == NULL)
                                    {
                                    LOG_DEBUG("Error allocating memory for string number\n\r");
                                    ptr += len;
                                } else
                                {
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
                                        LOG_DEBUG("%02x ", values[keyindex].v.string[i]);
                                    }
                                    values[keyindex].length = len/2;
                                    LOG_DEBUG("\r\n" );
                                }
                            }
                        }
                        else if (keyv[keyindex].type == STRING)
                        {
                            size_t len = parse_strlen( buffer , ptr, filesize);
                            // read a string
                            values[keyindex].v.string = malloc( len + 1);
                            if (values[keyindex].v.string == NULL)
                                {
                                LOG_DEBUG("Error allocating memory for string\n\r");
                                ptr += len;
                            } else {
                                memcpy( values[keyindex].v.string , buffer + ptr , len);
                                values[keyindex].v.string[len] = 0;
                                values[keyindex].length = len;
                                LOG_DEBUG("string %s\n\r" , values[keyindex].v.string);
                                ptr += len;
                            }
                        }
                        else if (keyv[keyindex].type == INTEGER)
                        {
                            size_t len = parse_strlen( buffer , ptr, filesize);
                            // read a number
                            values[keyindex].v.integer = malloc( 4);
                            if (values[keyindex].v.integer == NULL)
                                {
                                LOG_DEBUG("Error allocating memory for integer\n\r");
                                ptr += len;
                            } else
                            {
                                *values[keyindex].v.integer = (int) strtol( (char *)buffer + ptr, 0 , 0);
                                LOG_DEBUG("number %d\n\r" , *values[keyindex].v.integer);
                                values[keyindex].length = 1;
                                ptr += len;
                            }
                        }

                        // now search for the end of the line
                        while ( (ptr < filesize) && ((buffer[ptr] != '\n') && ( buffer[ptr] != '\r' )))
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
        if (filesystemWriteFile(outfile, ( uint8_t * ) outbuf, outptr) != outptr)
            {
                LOG_DEBUG("Error writing file %s\n\r", outfile);
                free(outbuf);
                return 0;
            }
    }
    free(outbuf);

    return 1;
}

void parse_relasekeyvalues( parserkeyvalue values[], int numberofkeys )
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