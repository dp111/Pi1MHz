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
#include <inttypes.h>
#include "../beebscsi/filesystem.h"


#define DEFAULT_BLOCK_SIZE 512
#define DEFAULT_SECTORS_PER_TRACK 32

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#define FIND_INDEX(key, array) ({ \
    int index = -1; \
    for (int i = 0; i < ARRAY_SIZE(array); i++) { \
        if (strcasecmp(array[i].key, key) == 0) { \
            index = i; \
            break; \
        } \
    } \
    index; \
})

enum keyvaluetype {
    NUMSTRING,
    STRING,
    INTEGER
};

struct keyvalue {
    char * key;
    int min;
    int max;
    int line;
    enum keyvaluetype type;
    char * defaultvalue;
    char * value;
};

struct keyvalue scsiattributes[] = {
    { "Title"           , 0 , 39 , 0  , STRING },
    { "Description"     , 0 ,255 , 0  , STRING },
    { "Inquiry"         , 0 , 0  ,101 , NUMSTRING ,{
        0x00,												// Peripherial Device Type
        0x00,												// RMB / Device-Type Qualifier
        0x00,												// ISO Version | ECMA Version | ANSI Version
        0x00,												// Reserved
        0x1E,												// Additional Length
        0x00,												// Vendor Unique
        0x00,												// Vendor Unique
        0x00,												// Vendor Unique
        'B','E','E','B','S','C','S','I',			// Vendor  Identification ASCII  8 bytes
        ' ','G','E','N','E','R','I','C',			// Product Identification ASCII 16 bytes
        ' ','H','D',' ',' ',' ',' ',' ',
        '1','.','0','0'								// Product Revision Level ASCII  4 bytes
         }},
    { "ModeParamHeader" , 0 ,  4 , 0  , NUMSTRING ,{
        // Mode Parameter Header (6)
        0x10,												// Mode Data length of(MPHeader + LBA BLOCK + Mode Page)-1 : not including this byte
        0x00,												// Medium type (0x00 = fixed hard drive)
        0x00,												// b7 Write Protect bit | b6 Rsvd | b5 DPOFUA | b4-b0	Reserved
        0x08												// the following LBA Block Descriptor Length
    } },
    { "LBADescriptor"   , 0 ,  8 , 0  , NUMSTRING ,{
        0x00, 0x00, 0x00, 0x00,						// Number of blocks MSB - LSB
        0x00,												// reserved
        (DEFAULT_BLOCK_SIZE & 0xFF0000) >> 16,			// Logical block length (sector size)
        (DEFAULT_BLOCK_SIZE & 0x00FF00) >> 8,
        (DEFAULT_BLOCK_SIZE & 0x0000FF)
    }},
    { "ModePage1"       , 0 ,  5 , 0 , NUMSTRING,{
        0x01,						// Page Code	| b7 PS| b6 SPF| b5-b0 Page Code
        0x03,						// Following data Length
        0x20,						// | 7 | 6 | TB | 4 | 3 | PER | DTE | DCR |
        0x04,						// Error recovery Retries (4)
        0x05,						// Error Correction Bit Span (5)
    } },
    { "ModePage3"       , 0 , 21 , 0 , NUMSTRING,{
        0x03,						// Page Code	| b7 PS| b6 SPF| b5-b0 Page Code
        0x13,						// Page Length (19)
        0x01, 0x32,				// Tracks per Zone (MSB, LSB)
        0x01, 0x32,				// Alternate Sectors per Zone (MSB, LSB)
        0x00, 0x06,				// Alternate Tracks per Zone (MSB, LSB)
        0x00, 0x06,				// Alternate Tracks per Volume (MSB, LSB)
        (DEFAULT_SECTORS_PER_TRACK & 0xFF00) >> 8,		// Sectors per Track (MSB, LSB)
        (DEFAULT_SECTORS_PER_TRACK & 0xFF),
        (DEFAULT_BLOCK_SIZE & 0x00FF00) >> 8, 				// Data Bytes per Physical Sector (MSB, LSB)
        (DEFAULT_BLOCK_SIZE & 0x0000FF),
        0x00, 0x01,				// Interleave (MSB, LSB)
        0x00, 0x00,				// Track Skew Factor (MSB, LSB)
        0x00, 0x00,				// Cylinder Skew Factor (MSB, LSB)
        0x80,						// | b7 SSEC | b6 HSEC | b5 RMB | b4 SURF | b3-b0 Drive Type
    } },
    { "ModePage4"       , 0 ,  6 , 0 , NUMSTRING,{
        0x04,						// Page Code	| b7 PS| b6 SPF| b5-b0 Page Code
        0x04,						// Page Length (4)
        0x00, 0x01, 0x32,		// Number of Cylinders (MSB-LSB)
        0x04						// Number of Heads
    } },
    { "ModePage32"      , 0 , 10 , 0 , NUMSTRING,{
        0x20,						// Page Code	| b7 PS| b6 SPF| b5-b0 Page Code
        0x08,						// Page Length (8)
        '0','0','0','0',
        '0','0','0','0'		// Serial Number ASCII (8 bytes)
    } },
    { "ModePage33"      , 0 ,  9 , 0 , NUMSTRING,{
        0x21,						// Page Code	| b7 PS| b6 SPF| b5-b0 Page Code
        0x07,						// Page Length (7)
        0x57, 0x0A, 0x0D,		// Manufacture Date and Build Level (6 bytes)
        0x02, 0x86, 0x00,
        02
    } },
    { "ModePage35"      , 0 ,  3 , 0 , NUMSTRING,{
        0x23,						// Page Code	| b7 PS| b6 SPF| b5-b0 Page Code
        0x01,						// Page Length (1)
        0x00						// System Flags
    } },
    { "ModePage36"      , 0 ,  4 , 0 , NUMSTRING,{
        0x24,						// Page Code	| b7 PS| b6 SPF| b5-b0 Page Code
        0x02,						// Page Length (2)
        0x00, 0x00				//
    } },
    { "ModePage37"      , 0 ,  6 , 0 , NUMSTRING,{
        0x25,						// Page Code	| b7 PS| b6 SPF| b5-b0 Page Code
        0x04,						// Page Length (4)
        '(','C',')','A'		// "(C)A",<cr>,0 (4-bytes)
    } },
    { "ModePage38"      , 0 ,  6 , 0 , NUMSTRING,{
        0x26,						// Page Code
        0x04,						// Page Length (6)
        'c','o','r','n'		// ASCII (4-bytes)
    } },
    { "LDUserCode"      , 0 , 4 , 0 , NUMSTRING },
    { "LDVideoXoffset"  , -768 , 768 , 0 , INTEGER },
    { ""} // end of list
};

static uint32_t parse_skip( const char * buffer , uint32_t * ptr , uint32_t filesize )
{
    while ( *ptr < filesize)
    {
        if ((buffer[*ptr] == ' ') || (buffer[*ptr] == '\t') || (buffer[*ptr] == '='))
        {
            *ptr++;
            continue;
        }
        if (buffer[*ptr] == '#')
        {
            while ( ((buffer[*ptr] != '\n')||( buffer[*ptr] != '\r' )) && *ptr < filesize)
                *ptr++;
            return 0;
        }
        if ((buffer[*ptr] == '\n') || ( buffer[*ptr] != '\r' ))
        {
            *ptr++;
            return 0;
        }
        return ptr;
    }

}

int parse_readfile( const char * filename , struct keyvalue keyv[] )
{
// open file for reading
    int ptr = 0;
    char * buffer = 0 ;
    uint32_t filesize = filesystemReadFile( filename , buffer , 0 );
    if (filesize == 0)
        return 0;

    while (ptr <filesize)
    {
        // skip a line if it starts with a #
        if (buffer[ptr] == '#')
        {
            while ( ((buffer[ptr] != '\n')||( buffer[ptr] != '\r' )) && ptr < filesize)
                ptr++;
            continue;
        }

        // find the key
        int keyindex = FIND_INDEX( buffer + ptr , keyv );
        if (keyindex == -1)
        {
            // key not found
            LOG_DEBUG("Key not found %s\n", buffer + ptr );
            while ( ((buffer[ptr] != '\n')||( buffer[ptr] != '\r' )) && ptr < filesize)
                ptr++;
            continue;

        }
        else
        {
            // key found
            ptr += strlen(keyv[keyindex].key);
            // skip white space
            // skip "="
            // if '#' then skip line
            // if new line then skip
            if ( parse_skip( buffer , &ptr , filesize ) == 0)
                continue;

            // read value into keyv[keyindex].value
            if (keyv[keyindex].type == NUMSTRING)
            {
                // read a number
                // skip white space
                // skip ","
                // if new line then return
                // read number
            }
            else if (keyv[keyindex].type == STRING)
            {
                // read a string
                // skip white space
                // skip ","
                // if new line then return
                // read string
            }
            else if (keyv[keyindex].type == INTEGER)
            {
                // read a number
                // skip white space
                // skip ","
                // if new line then return
                // read number
            }
        }
    }


    free(buffer);
    return 1;
}

int parse_writefile ( const char * filename , struct keyvalue keyv[]  )
{

}

int parse_relasekeyvalues( struct keyvalue *keys )
{
    for (int i = 0; keys[i].key != ""; i++)
    {
        if (keys[i].value != NULL)
        {
            free(keys[i].value);
        }
    }
}