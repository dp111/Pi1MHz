/*
 * Host test for outbyte() in src/rpi/armc-cstubs.c, which printf() and puts()
 * reach through _write().  Its bytes go to the mini-UART unchanged, and a raw
 * terminal shows a bare "\n" as a line feed with no carriage return, so each
 * line starts under the end of the last.  outbyte() is extracted VERBATIM by
 * run_tests.sh; RPI_AuxMiniUartWrite() here records what would be sent.
 */
#include <stdio.h>
#include <string.h>

static char g_wire[256];
static size_t g_len;
static unsigned g_checks;
static unsigned g_failures;

static void RPI_AuxMiniUartWrite(char c)
{
   if (g_len < sizeof g_wire - 1)
      g_wire[g_len++] = c;
}

#include "outbyte.inc"

static void send(const char *text)
{
   for (; *text; text++)
      outbyte(*text);
}

static void expect(const char *name, const char *sent, const char *want)
{
   g_len = 0;
   memset(g_wire, 0, sizeof g_wire);
   /* Start every case from a known state: a character that is neither CR nor LF. */
   outbyte('x');
   g_len = 0;
   send(sent);
   g_checks++;
   if (strcmp(g_wire, want) != 0) {
      g_failures++;
      printf("  FAIL: %s\n", name);
   } else {
      printf("  ok: %s\n", name);
   }
}

int main(void)
{
   expect("a bare LF gets a CR in front", "a\nb\n", "a\r\nb\r\n");
   expect("CR LF is left alone", "a\r\nb\r\n", "a\r\nb\r\n");
   expect("text with no newline is untouched", "abc", "abc");
   expect("two LFs in a row each get one", "\n\n", "\r\n\r\n");
   expect("a lone CR is left alone", "a\rb", "a\rb");
   expect("LF CR (as in a message that ends \\n\\r) gains one CR in front only", "a\n\rb", "a\r\n\rb");

   /* The state carries across messages: "...\r" then "\n..." is still CR LF. */
   g_len = 0;
   memset(g_wire, 0, sizeof g_wire);
   outbyte('x');
   g_len = 0;
   send("first\r");
   send("\nsecond\n");
   g_checks++;
   if (strcmp(g_wire, "first\r\nsecond\r\n") != 0) {
      g_failures++;
      printf("  FAIL: CR in one call and LF in the next make one CR LF\n");
   } else {
      printf("  ok: CR in one call and LF in the next make one CR LF\n");
   }

   printf("%u checks, %u failures\n", g_checks, g_failures);
   return g_failures ? 1 : 0;
}
