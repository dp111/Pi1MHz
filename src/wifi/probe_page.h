/* probe_page.h - the body of the web server's /probe page: the RAM probe for a person.
   See docs/dev/ramprobe.md. */
#ifndef PROBE_PAGE_H
#define PROBE_PAGE_H

/* The page's HTML and script, in pieces (each under the 4095 characters C promises a string
   literal), ending with NULL.  It goes between page_open and page_close. */
extern const char *const probe_page_body[];

/* Instead of probe_page_body, when the probe isn't running. */
extern const char probe_page_off[];

#endif
