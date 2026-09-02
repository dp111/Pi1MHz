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

/* Largest config file parse_readfile() will accept. Config files are only a
 * few KB; this ceiling keeps the 4x output buffer (filesize*4) well clear of
 * a size_t overflow and a sane allocation size. */
#define PARSE_MAX_FILE_SIZE (256u * 1024u)

/* Compare S1 and S2, ignoring case, returning less than, equal to or
   greater than zero if S1 is lexicographically less than,
   equal to or greater than S2.  */
static bool localstrcasecmp(const char *s1, const char *s2 )
{
    while(1)
    {
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

/* Length of a hex value, and what kind it is:
 *   *nonnumber =  0  plain hex digits
 *                 1  an 0x prefix, which the caller strips
 *                -1  something that is not a hex number at all
 */
static size_t parse_numstrlen( const uint8_t * buf , size_t ptr, size_t max,  int *nonnumber)
{
    size_t len = 0;
    size_t oldptr = ptr;
    *nonnumber = 0;
    while ( (ptr < max) && (buf[ptr] > ' ') && (buf[ptr] != '#') )
    {
        bool ishex = ((buf[ptr] >= '0') && (buf[ptr] <= '9'))
                  || ((buf[ptr] >= 'A') && (buf[ptr] <= 'F'))
                  || ((buf[ptr] >= 'a') && (buf[ptr] <= 'f'));
        if (!ishex)
        {
            /* An x is only the 0x prefix, and only as the second character. */
            if (((buf[ptr] == 'x') || (buf[ptr] == 'X'))
                && (*nonnumber == 0) && ((ptr - oldptr) == 1))
                *nonnumber = 1;
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
    while ( (ptr < max) &&(buf[ptr] >= ' ') && (buf[ptr] != '#') )
        {
            len++;
            ptr++;
        }
    return len;
}

/* One pass over a file: where we are reading, and the buffer being built if
 * this is a rewrite.  Threading this through the helpers is what keeps the
 * main loop flat enough to read. */
typedef struct {
    uint8_t * in;        /* file contents - written to once, see the odd-digit pad */
    size_t    insize;
    size_t    ptr;       /* read cursor */
    char    * out;       /* rewrite buffer - always allocated */
    size_t    outcap;
    size_t    outptr;
} parse_state;

/* Bounded append to the output buffer. The buffer is sized at 4x the input
 * file; routing every write through this helper means a config file whose
 * rewritten form is unexpectedly large drops the excess instead of
 * overrunning the heap allocation. */
static void emit(parse_state * st, int c)
{
    if (st->outptr < st->outcap)
        st->out[st->outptr++] = (char)c;
}

static void emit_nibble(parse_state * st, int nibble)
{
    emit(st, nibble < 10 ? nibble + '0' : nibble + 'A' - 10);
}

static bool at_eol(const parse_state * st)
{
    return (st->in[st->ptr] == '\n') || (st->in[st->ptr] == '\r');
}

/* Copy through to the end of the line, leaving the terminator in place. */
static void copy_to_eol(parse_state * st)
{
    while ((st->ptr < st->insize) && !at_eol(st))
        emit(st, st->in[st->ptr++]);
}

/* Step over the "=" and any spacing between a key and its value, copying it
 * through unchanged.  False when the line carries no value: a bare key, or
 * one followed only by a comment. */
static bool skip_to_value(parse_state * st)
{
    while (st->ptr < st->insize)
    {
        uint8_t c = st->in[st->ptr];
        if ((c == ' ') || (c == '\t') || (c == '='))
        {
            emit(st, st->in[st->ptr++]);
            continue;
        }
        if (c == '#')
        {
            copy_to_eol(st);
            return false;
        }
        if ((c == '\n') || (c == '\r'))
        {
            emit(st, st->in[st->ptr++]);
            return false;
        }
        return true;
    }
    return false;
}

/* Rewrite path: put the caller's value where the file's one was. */
static void write_value(parse_state * st, const parserkey * key,
                        const parserkeyvalue * value)
{
    size_t len = 0;

    switch (key->type)
    {
        case NUMSTRING:
        {
            int nonnumber;
            len = parse_numstrlen(st->in, st->ptr, st->insize, &nonnumber);
            for (size_t i = 0; i < value->length; i++)
            {
                emit_nibble(st, (value->v.string[i] >> 4) & 0x0F);
                emit_nibble(st, value->v.string[i] & 0x0F);
            }
            break;
        }
        case STRING:
            len = parse_strlen(st->in, st->ptr, st->insize);
            for (size_t i = 0; i < value->length; i++)
                emit(st, value->v.string[i]);
            break;
        case INTEGER:
        {
            size_t remaining = st->outcap - st->outptr;
            size_t outlen;
            len = parse_strlen(st->in, st->ptr, st->insize);
            outlen = (size_t) snprintf(st->out + st->outptr, remaining, "%d",
                                       *value->v.integer);
            if (outlen > 0 && outlen < remaining)
                st->outptr += outlen;
            break;
        }
    }
    st->ptr += len;                     /* skip the value the file had */
}

/* Read path: take the file's value into the caller's slot. */
static void store_numstring(parse_state * st, const parserkey * key,
                            parserkeyvalue * value)
{
    int nonnumber;
    size_t len = parse_numstrlen(st->in, st->ptr, st->insize, &nonnumber);
    size_t maxbytes = (size_t)key->max;
    size_t nbytes;

    if (nonnumber < 0)
    {
        LOG_DEBUG("Error in number format\n\r");
        st->ptr += len;
        return;
    }
    if (nonnumber == 1)
    {
        st->ptr += 2;                   /* strip off 0x */
        len -= 2;
    }
    if (len % 2)
    {
        /* Odd digit count: borrow the character before the value - the "="
           or a space, already copied out - and pad the number from the
           left, so A1B reads as 0x0A1B rather than being rejected. */
        LOG_DEBUG("Error odd number of digits in hex string\n\r");
        len++;
        st->ptr--;
        st->in[st->ptr] = '0';
    }
    nbytes = len / 2;
    if (nbytes > maxbytes)
    {
        LOG_DEBUG("Value too long for key - truncated\n\r");
        nbytes = maxbytes;
    }
    /* The allocation is always max bytes and zero padded, so consumers that
       access fixed offsets up to max-1 can never overrun a value that a
       hand-edited file made short. */
    value->v.string = malloc(maxbytes);
    if (value->v.string == NULL)
    {
        LOG_DEBUG("Error allocating memory for string\n\r");
        st->ptr += len;
        return;
    }
    memset(value->v.string, 0, maxbytes);
    for (size_t i = 0; i < nbytes; i++)
    {
        char digit = 0;
        for (int half = 0; half < 2; half++)
        {
            uint8_t c = st->in[st->ptr++];
            digit = (char)(digit << 4);
            if ((c >= '0') && (c <= '9'))       digit = (char)(digit | (c - '0'));
            else if ((c >= 'A') && (c <= 'F'))  digit = (char)(digit | (c - 'A' + 10));
            else if ((c >= 'a') && (c <= 'f'))  digit = (char)(digit | (c - 'a' + 10));
        }
        value->v.string[i] = digit;
        LOG_DEBUG("%02x ", value->v.string[i]);
    }
    value->length = nbytes;
    st->ptr += len - (nbytes * 2);      /* skip any truncated digits */
    LOG_DEBUG("\r\n");
}

static void store_string(parse_state * st, const parserkey * key,
                         parserkeyvalue * value)
{
    size_t len = parse_strlen(st->in, st->ptr, st->insize);
    size_t valuelen = len;
    size_t copylen;

    /* Blanks between the value and a trailing comment are layout, not data:
       "ssid=Home   # mine" must not join a network called "Home   ".  Only
       the stored value is trimmed - ptr still advances over the whole run,
       so a rewrite keeps the spacing the file had. */
    while (valuelen != 0
           && ((st->in[st->ptr + valuelen - 1u] == ' ')
            || (st->in[st->ptr + valuelen - 1u] == '\t')))
        valuelen--;

    copylen = valuelen;
    if (copylen > (size_t)key->max)
    {
        LOG_DEBUG("Value too long for key - truncated\n\r");
        copylen = (size_t)key->max;
    }
    value->v.string = malloc(copylen + 1);
    if (value->v.string == NULL)
    {
        LOG_DEBUG("Error allocating memory for string\n\r");
        st->ptr += len;
        return;
    }
    memcpy(value->v.string, st->in + st->ptr, copylen);
    value->v.string[copylen] = 0;
    value->length = copylen;
    LOG_DEBUG("string %s\n\r", value->v.string);
    st->ptr += len;
}

static void store_integer(parse_state * st, const parserkey * key,
                          parserkeyvalue * value)
{
    size_t len = parse_strlen(st->in, st->ptr, st->insize);
    int parsed;

    value->v.integer = malloc(sizeof(int));
    if (value->v.integer == NULL)
    {
        LOG_DEBUG("Error allocating memory for integer\n\r");
        st->ptr += len;
        return;
    }
    parsed = (int) strtol((char *)st->in + st->ptr, 0, 0);
    if (parsed < key->min) parsed = key->min;      /* clamp to the key's range */
    if (parsed > key->max) parsed = key->max;
    *value->v.integer = parsed;
    LOG_DEBUG("number %d\n\r", *value->v.integer);
    value->length = 1;
    st->ptr += len;
}

static void store_value(parse_state * st, const parserkey * key,
                        parserkeyvalue * value)
{
    switch (key->type)
    {
        case NUMSTRING: store_numstring(st, key, value); break;
        case STRING:    store_string(st, key, value);    break;
        case INTEGER:   store_integer(st, key, value);   break;
    }
}

//
// Parse a file into the key value structure
//
// if outfile filename is set then the keyvalues will be written back to the
// file, with any value the caller has already filled in replacing the file's.
// Note that a rewrite does not read values back out: with an outfile the
// values array is an input, not an output.

int parse_readfile( const char * filename , const char * outfile, const parserkey keyv[], parserkeyvalue values[])
{
    parse_state st = {0};
    uint8_t * buffer = 0;
    size_t filesize = filesystemReadFile( filename , &buffer , 0 );

    if (filesize == 0)
    {
        /* A zero-length read still allocated: filesystemReadFile() mallocs
           fileSize+1 before it reads, so an empty .cfg leaked a byte on
           every open of the LUN it belongs to. */
        free(buffer);
        return 0;
    }
    // Reject absurdly large files: this bounds filesize*4 below well clear of
    // a size_t overflow, which would otherwise yield a tiny output buffer.
    if (filesize > PARSE_MAX_FILE_SIZE)
    {
        LOG_DEBUG("Config file %s too large (%zu bytes) - not parsed\n\r", filename, filesize);
        free(buffer);
        return 0;
    }
    LOG_DEBUG("Parsing %s File size %zu\n\r",filename, filesize);

    st.in = buffer;
    st.insize = filesize;
    st.outcap = filesize * 4;           // out buffer is 4x input size
    st.out = malloc(st.outcap);
    if (st.out == NULL)
    {
        free(buffer);
        return 0;
    }

    while (st.ptr < st.insize)
    {
        int keyindex;
        size_t keylen;

        // blank lines and leading white space pass straight through
        while ((st.ptr < st.insize) && (st.in[st.ptr] <= ' '))
            emit(&st, st.in[st.ptr++]);
        if (st.ptr >= st.insize)
            break;

        if (st.in[st.ptr] == '#')       // a comment line
        {
            copy_to_eol(&st);
            continue;
        }

        keyindex = parse_findindex((const char *)(st.in + st.ptr), keyv);
        if (keyindex == -1)             // not a key we know - leave the line alone
        {
            LOG_DEBUG("Key not found\n\r");
            copy_to_eol(&st);
            continue;
        }

        LOG_DEBUG("Key found %s \r\n", keyv[keyindex].key);
        for (keylen = strlen(keyv[keyindex].key); keylen != 0; keylen--)
            emit(&st, st.in[st.ptr++]);

        if (!skip_to_value(&st))
        {
            LOG_DEBUG("Key data not found \n\r");
            continue;
        }

        if (outfile)
        {
            if (values[keyindex].length)
                write_value(&st, &keyv[keyindex], &values[keyindex]);
        }
        else
        {
            store_value(&st, &keyv[keyindex], &values[keyindex]);
        }
        copy_to_eol(&st);
    }

    free(buffer);

    if (outfile)
    {
        /* Verified write-and-swap: a half-written scsi0.cfg is a LUN that no
           longer describes itself, and this is often called with outfile ==
           filename, i.e. rewriting the only copy in place. */
        if (!filesystemWriteFileSafe(outfile, ( uint8_t * ) st.out, st.outptr))
        {
            LOG_DEBUG("Error writing file %s\n\r", outfile);
            free(st.out);
            return 0;
        }
    }
    free(st.out);

    return 1;
}

void parse_releasekeyvalues( parserkeyvalue values[], int numberofkeys )
{
    for (int i = 0; i < numberofkeys ; i++)
    {
        if (values[i].v.string != NULL)
        {
            free(values[i].v.string);
            values[i].v.string = NULL;
            values[i].length = 0;
            
        }
    }
}