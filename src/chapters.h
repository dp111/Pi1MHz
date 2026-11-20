/*
 * chapters.h - a LaserDisc side's chapter table, from the chapters.txt that
 * sits beside its video.pvf (docs/dev/fcode-chapters.md).
 *
 * Pure parsing and lookup, no file or player state: videoplayer.c reads the
 * file and owns the table.
 *
 * chapters.txt, one chapter per line, ascending:
 *
 *     CC FFFFF  # anything
 *
 * CC is the chapter number as the disc codes it (0..79, the VP415's ?C
 * range); FFFFF is the chapter's first picture in the player's own unit -
 * video.pvf record index + 1, which is the CAV picture number and what
 * FxxxxxR takes.  A chapter ends one picture before the next one starts; the
 * last runs to the end of the disc.  Blank lines and '#' comments are
 * ignored.  Anything else rejects the whole file: a table with a line
 * missing would send a goto to the wrong place, which is worse than none.
 */
#ifndef CHAPTERS_H
#define CHAPTERS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CHAPTERS_MAX        80u        /* chapters 00..79 */
#define CHAPTERS_PICTURE_MAX 999999u   /* a CLV side runs past 5 digits' worth of CAV */

typedef struct {
   uint8_t  count;
   uint8_t  number[CHAPTERS_MAX];      /* ascending */
   uint32_t first[CHAPTERS_MAX];       /* ascending, >= 1 */
} chapter_table_t;

/* Parse text[0..len).  True with the table filled (count may be 0 for a file
   of only comments); false with count 0 and *bad_line (if not NULL) set to
   the 1-based line that is wrong. */
bool chapters_parse(const char *text, size_t len, chapter_table_t *t, unsigned *bad_line);

/* The index of chapter 'number', or -1. */
int chapters_find(const chapter_table_t *t, uint32_t number);

/* The index of the chapter holding 'picture', or -1 (before the first). */
int chapters_at(const chapter_table_t *t, uint32_t picture);

#endif
