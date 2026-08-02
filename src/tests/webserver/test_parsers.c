/* Host tests for webserver.c's network-facing untrusted-input parsers.
 *
 * The functions under test are extracted VERBATIM from a copy of
 * src/wifi/webserver.c by extract.awk (see run_tests.sh) and compiled
 * into this translation unit, so the real code runs against hostile
 * inputs under ASan/UBSan.  Covered: HTTP request line and header-block
 * parsing, multipart Content-Type boundary/filename extraction, URL
 * percent-decoding (control-byte neutralisation), SD path normalisation
 * and traversal rejection, Digest-auth field parsing and URI binding,
 * WebDAV Destination parsing and RFC1123 date parsing.
 */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "md5.h"          /* real header: md5_hex_t / MD5_HEX_LEN */

#include "ws_defines.inc" /* WS_PATH_MAX etc., extracted */
#include "ws_parsers.inc" /* the parsers, extracted verbatim */

static int checks, fails;
static void ok(int cond, const char *what)
{
   checks++;
   if (!cond) { fails++; printf("  FAIL: %s\n", what); }
   else         printf("  ok: %s\n", what);
}

static int streq(const char *a, const char *b) { return strcmp(a, b) == 0; }

int main(void)
{
   puts("== text helpers ==");
   ok(ws_lc('A') == 'a' && ws_lc('z') == 'z' && ws_lc('0') == '0',
      "ws_lc lowers only A-Z");
   ok(ws_stricmp("Content-Length", "content-LENGTH") == 0, "stricmp equal");
   ok(ws_stricmp("abc", "abd") < 0 && ws_stricmp("abd", "abc") > 0,
      "stricmp orders");
   ok(ws_stricmp("ab", "abc") != 0, "stricmp length mismatch");
   ok(ws_prefix("/files/", "/files/x.txt"), "ws_prefix hit");
   ok(!ws_prefix("/files/", "/file"), "ws_prefix stops at short subject");
   ok(ws_prefix_ci_str("Digest username=..", "digest "), "prefix_ci_str");
   ok(ws_strcasestr("Expect: 100-CONTINUE", "100-continue") != NULL,
      "strcasestr case-blind hit");
   ok(ws_strcasestr("nothing here", "100-continue") == NULL,
      "strcasestr miss");
   ok(ws_strcasestr("abc", "") != NULL, "strcasestr empty needle");
   ok(ws_hexval('0') == 0 && ws_hexval('9') == 9 && ws_hexval('a') == 10
      && ws_hexval('F') == 15 && ws_hexval('g') == -1 && ws_hexval(' ') == -1,
      "ws_hexval table");

   puts("== ws_url_decode ==");
   {
      char d[64];
      ok(ws_url_decode("%41%62c", d, sizeof d) && streq(d, "Abc"),
         "basic percent decode");
      ok(ws_url_decode("a%20b", d, sizeof d) && streq(d, "a b"),
         "space decode");
      ok(ws_url_decode("%00", d, sizeof d) && streq(d, "_"),
         "encoded NUL neutralised to '_'");
      ok(ws_url_decode("%0d%0a", d, sizeof d) && streq(d, "__"),
         "encoded CRLF neutralised (header smuggling)");
      ok(ws_url_decode("%7f%1f", d, sizeof d) && streq(d, "__"),
         "DEL and control neutralised");
      ok(ws_url_decode("a\rb\nc", d, sizeof d) && streq(d, "a_b_c"),
         "raw control bytes neutralised");
      ok(ws_url_decode("%zz%4", d, sizeof d) && streq(d, "%zz%4"),
         "bad hex passes through literally");
      ok(ws_url_decode("%", d, sizeof d) && streq(d, "%"),
         "trailing lone percent");
      ok(ws_url_decode("%ff", d, sizeof d)
         && (unsigned char)d[0] == 0xFFu && d[1] == '\0',
         "high byte preserved");
      ok(ws_url_decode("%2500", d, sizeof d) && streq(d, "%00"),
         "single decode only (no double decode)");
      ok(ws_url_decode("AB", d, 3u) && streq(d, "AB"),
         "exact fit reports success");
      ok(!ws_url_decode("ABC", d, 3u) && streq(d, "AB"),
         "truncation reports failure, output still terminated");
      ok(ws_url_decode("", d, 1u) && streq(d, ""), "empty into size-1");
   }

   puts("== ws_memfind / ws_find_header_end ==");
   {
      static const uint8_t hay[] = { 'a', 0, 'b', 'c', 0, 'b' };
      static const uint8_t nb[]  = { 0, 'b' };
      ok(ws_memfind(hay, sizeof hay, nb, 2u) == 1, "binary needle w/ NUL");
      ok(ws_memfind(hay, sizeof hay, (const uint8_t *)"cz", 2u) == -1,
         "miss");
      ok(ws_memfind(hay, sizeof hay, nb, 0u) == -1, "empty needle refused");
      ok(ws_memfind(hay, 1u, nb, 2u) == -1, "needle longer than hay");
      ok(ws_memfind(hay, sizeof hay, (const uint8_t *)"\0b", 2u) == 1,
         "match not at end confusion");

      ok(ws_find_header_end("GET / H\r\n\r\nBODY", 15u) == 11,
         "CRLFCRLF end offset");
      ok(ws_find_header_end("a\n\nb", 4u) == 3, "bare LFLF accepted");
      ok(ws_find_header_end("\r\n\r\n", 4u) == 4, "block is only the end");
      ok(ws_find_header_end("GET / HTTP/1.1\r\n", 16u) == -1,
         "incomplete block");
      ok(ws_find_header_end("", 0u) == -1, "empty");
      ok(ws_find_header_end("a\r\n\r", 4u) == -1, "split CRLFCRLF not yet");
   }

   puts("== ws_parse_request_line ==");
   {
      char m[12], p[64];
      ok(ws_parse_request_line("GET /x HTTP/1.1\r\n", m, sizeof m, p, sizeof p)
         && streq(m, "GET") && streq(p, "/x"), "simple GET");
      ok(ws_parse_request_line("PROPFIND  /a/b HTTP/1.1\r\n",
                               m, sizeof m, p, sizeof p)
         && streq(m, "PROPFIND") && streq(p, "/a/b"),
         "multiple spaces between tokens");
      ok(!ws_parse_request_line("GET\r\n", m, sizeof m, p, sizeof p),
         "no path -> false");
      ok(!ws_parse_request_line("", m, sizeof m, p, sizeof p), "empty");
      ok(!ws_parse_request_line("GET ", m, sizeof m, p, sizeof p),
         "space but empty path");
      ok(ws_parse_request_line("OPTIONS /y HTTP/1.1", m, 4u, p, sizeof p)
         && streq(m, "OPT") && streq(p, "/y"),
         "method truncated to msz, still parses");
      ok(ws_parse_request_line("GET /very/long/path X", m, sizeof m, p, 6u)
         && streq(p, "/very"),
         "path truncated to psz");
      /* An empty method with a leading space parses; the router's method
         table then rejects it.  Documenting actual behaviour. */
      ok(ws_parse_request_line(" /x H", m, sizeof m, p, sizeof p)
         && streq(m, "") && streq(p, "/x"),
         "leading space -> empty method, path kept");
   }

   puts("== ws_find_header ==");
   {
      static const char H[] =
         "GET /Host:fake HTTP/1.1\r\n"
         "Host: beeb\r\n"
         "X-Content-Length: 9\r\n"
         "Content-Length-Extra: 8\r\n"
         "cOnTeNt-LeNgTh:\t 42\r\n"
         "Empty:\r\n"
         "Tail: end";
      char v[16];
      ok(ws_find_header(H, strlen(H), "Host", v, sizeof v) && streq(v, "beeb"),
         "simple header");
      ok(ws_find_header(H, strlen(H), "Content-Length", v, sizeof v)
         && streq(v, "42"),
         "case-insensitive name, tab/space skipped, decoys ignored");
      ok(ws_find_header(H, strlen(H), "Empty", v, sizeof v) && streq(v, ""),
         "empty value");
      ok(ws_find_header(H, strlen(H), "Tail", v, sizeof v) && streq(v, "end"),
         "value at end of block (no CRLF)");
      ok(!ws_find_header(H, strlen(H), "GET", v, sizeof v),
         "request line is skipped");
      ok(!ws_find_header(H, strlen(H), "Missing", v, sizeof v), "miss");
      ok(!ws_find_header(H, 26u, "Content-Length", v, sizeof v),
         "limit truncates the search");
      ok(ws_find_header(H, strlen(H), "Host", v, 3u) && streq(v, "be"),
         "value truncated to osz");
      ok(!ws_find_header("A: x\r\nHost : y\r\n", 16u, "Host", v, sizeof v),
         "space before colon does not match");
      ok(!ws_find_header("", 0u, "Host", v, sizeof v), "empty block");
   }

   puts("== multipart: boundary / filename ==");
   {
      char b[WS_BOUNDARY_MAX];
      ok(ws_extract_boundary("multipart/form-data; boundary=----WebKit123",
                             b, sizeof b) && streq(b, "----WebKit123"),
         "plain boundary");
      ok(ws_extract_boundary("multipart/form-data; BOUNDARY=\"a b;c\"",
                             b, sizeof b) && streq(b, "a b;c"),
         "quoted boundary keeps spaces/semicolons, case-blind key");
      ok(ws_extract_boundary("multipart/form-data; boundary=xy; charset=x",
                             b, sizeof b) && streq(b, "xy"),
         "unquoted stops at semicolon");
      ok(!ws_extract_boundary("multipart/form-data", b, sizeof b),
         "missing boundary");
      ok(!ws_extract_boundary("multipart/form-data; boundary=", b, sizeof b),
         "empty boundary refused");
      ok(!ws_extract_boundary("multipart/form-data; boundary=\"\"",
                              b, sizeof b),
         "empty quoted boundary refused");
      {
         char tiny[4];
         ok(ws_extract_boundary("x; boundary=abcdef", tiny, sizeof tiny)
            && streq(tiny, "abc"),
            "boundary truncated to buffer");
      }

      {
         char f[32];
         ok(ws_extract_filename(
               "Content-Disposition: form-data; name=\"file\"; "
               "filename=\"a.txt\"", f, sizeof f) && streq(f, "a.txt"),
            "filename extracted, name= decoy skipped");
         ok(!ws_extract_filename("form-data; name=\"x\"", f, sizeof f),
            "no filename");
         ok(!ws_extract_filename("form-data; filename=bare", f, sizeof f),
            "unquoted filename refused");
         ok(ws_extract_filename("form-data; filename=\"\"", f, sizeof f)
            && streq(f, ""),
            "empty quoted filename accepted as empty");
         ok(ws_extract_filename("form-data; filename=\"..\\evil\"",
                                f, sizeof f) && streq(f, "..\\evil"),
            "hostile name survives literally (basename applied later)");
         ok(ws_extract_filename("form-data; filename=\"unterminated",
                                f, sizeof f) && streq(f, "unterminated"),
            "unterminated quote consumes to end without crash");
      }
   }

   puts("== SD paths: safety / normalise / parent ==");
   {
      ok(ws_path_is_safe("/a/b.txt"), "plain path safe");
      ok(ws_path_is_safe(""), "empty safe");
      ok(!ws_path_is_safe("/.."), "trailing dotdot");
      ok(!ws_path_is_safe("../a"), "leading dotdot");
      ok(!ws_path_is_safe("/a/../b"), "middle dotdot");
      ok(!ws_path_is_safe(".."), "bare dotdot");
      ok(ws_path_is_safe("/..."), "three dots is a name");
      ok(ws_path_is_safe("/a..b/..c/d.."), "dotdot inside names ok");
      ok(!ws_path_is_safe("/a\x01" "b"), "control byte rejected");
      /* Only bytes < 0x20 are rejected here: DEL (0x7f) is neutralised
         earlier, at ws_url_decode time.  Documenting the split. */
      ok(ws_path_is_safe("/a\x7f"),
         "DEL not this layer's job (decode neutralises it)");

      char n[WS_PATH_MAX];
      ws_normalize_path("", n, sizeof n);
      ok(streq(n, "/"), "empty -> /");
      ws_normalize_path(NULL, n, sizeof n);
      ok(streq(n, "/"), "NULL -> /");
      ws_normalize_path("a", n, sizeof n);
      ok(streq(n, "/a"), "leading slash forced");
      ws_normalize_path("//a///b//", n, sizeof n);
      ok(streq(n, "/a/b"), "slash collapse + trailing strip");
      ws_normalize_path("/", n, sizeof n);
      ok(streq(n, "/"), "root survives");
      ws_normalize_path("\\a\\b", n, sizeof n);
      ok(streq(n, "/a/b"), "backslashes become slashes");
      ws_normalize_path("/BeebSCSI0\\scsi0.dat", n, sizeof n);
      ok(streq(n, "/BeebSCSI0/scsi0.dat"),
         "the LUN-interlock bypass spelling is canonicalised");
      ws_normalize_path("/a/./b", n, sizeof n);
      ok(streq(n, "/a/b"), "dot segment dropped");
      ws_normalize_path("/./", n, sizeof n);
      ok(streq(n, "/"), "lone dot segment -> root");
      ws_normalize_path("/a/.", n, sizeof n);
      ok(streq(n, "/a"), "trailing dot segment dropped");
      ws_normalize_path("/a/.hidden", n, sizeof n);
      ok(streq(n, "/a/.hidden"), "dotfiles untouched");
      ws_normalize_path("..", n, sizeof n);
      ok(streq(n, "/.."), "dotdot NOT resolved here (is_safe rejects it)");
      {
         char t[4];
         ws_normalize_path("/abcdef", t, sizeof t);
         ok(streq(t, "/ab"), "truncation keeps a valid prefix");
      }

      char pp[WS_PATH_MAX];
      ws_parent_path("/a/b", pp, sizeof pp);
      ok(streq(pp, "/a"), "parent of /a/b");
      ws_parent_path("/x", pp, sizeof pp);
      ok(streq(pp, "/"), "parent of /x");
      ws_parent_path("/", pp, sizeof pp);
      ok(streq(pp, "/"), "parent of root");
      ws_parent_path("noslash", pp, sizeof pp);
      ok(streq(pp, "/"), "no separator -> root");

      ok(streq(ws_basename("/a/b/c.txt"), "c.txt"), "basename");
      ok(streq(ws_basename("c"), "c"), "basename bare");
      ok(streq(ws_basename("/a/"), ""), "basename of dir path");
      ok(streq(ws_basename("a\\b"), "b"), "basename sees backslash");
      ok(ws_is_root("/") && !ws_is_root("") && !ws_is_root("/a"),
         "ws_is_root");
   }

   puts("== dav_url_to_sdpath / dav_destination_sdpath ==");
   {
      char sd[WS_PATH_MAX];
      ok(dav_url_to_sdpath("/dir/file.txt", sd, sizeof sd)
         && streq(sd, "/dir/file.txt"), "plain URL");
      ok(dav_url_to_sdpath("/a%20b", sd, sizeof sd) && streq(sd, "/a b"),
         "escapes decoded");
      ok(!dav_url_to_sdpath("/%2e%2e/etc", sd, sizeof sd),
         "encoded ../ traversal rejected");
      ok(!dav_url_to_sdpath("/a/../b", sd, sizeof sd),
         "literal traversal rejected");
      ok(!dav_url_to_sdpath("/a%5C..%5Cb", sd, sizeof sd),
         "backslash-spelled traversal rejected after canonicalising");
      ok(dav_url_to_sdpath("/BeebSCSI0%5Cscsi0.dat", sd, sizeof sd)
         && streq(sd, "/BeebSCSI0/scsi0.dat"),
         "encoded backslash canonicalised for the LUN interlock");
      ok(dav_url_to_sdpath("/a%00b", sd, sizeof sd) && streq(sd, "/a_b"),
         "encoded NUL cannot cut the path short");
      ok(dav_url_to_sdpath("%2F%2F", sd, sizeof sd) && streq(sd, "/"),
         "encoded slashes collapse to root");
      {
         char lp[WS_PATH_MAX + 64u];
         memset(lp, 'a', sizeof lp - 1u);
         lp[0] = '/';
         lp[sizeof lp - 1u] = '\0';
         ok(!dav_url_to_sdpath(lp, sd, sizeof sd),
            "overlong URL rejected, not silently truncated");
      }

      ok(dav_destination_sdpath("http://pi/dir/a.txt", sd, sizeof sd)
         && streq(sd, "/dir/a.txt"), "Destination http scheme");
      ok(dav_destination_sdpath("HTTPS://pi:8080/x", sd, sizeof sd)
         && streq(sd, "/x"), "Destination https + port, case-blind");
      ok(dav_destination_sdpath("/rel/x", sd, sizeof sd)
         && streq(sd, "/rel/x"), "Destination path-only form");
      ok(!dav_destination_sdpath("http://hostonly", sd, sizeof sd),
         "Destination with no path rejected");
      ok(!dav_destination_sdpath("http://h/%2e%2e/x", sd, sizeof sd),
         "Destination traversal rejected");
      ok(!dav_destination_sdpath("", sd, sizeof sd),
         "empty Destination rejected");
   }

   puts("== digest auth: field / hex compare / uri binding ==");
   {
      static const char A[] =
         "username=\"Pi1MHz\", realm=\"a,b\", cnonce=\"c1\", "
         "nonce=\"n0nce\", uri=\"/x?q=1\", nc=00000001, qop=auth, "
         "response=\"00112233445566778899aabbccddeeff\"";
      char v[64];
      ok(ws_digest_field(A, "username", v, sizeof v) && streq(v, "Pi1MHz"),
         "quoted field");
      ok(ws_digest_field(A, "realm", v, sizeof v) && streq(v, "a,b"),
         "quoted comma stays in value");
      ok(ws_digest_field(A, "nonce", v, sizeof v) && streq(v, "n0nce"),
         "nonce not confused with cnonce");
      ok(ws_digest_field(A, "cnonce", v, sizeof v) && streq(v, "c1"),
         "cnonce found");
      ok(ws_digest_field(A, "nc", v, sizeof v) && streq(v, "00000001"),
         "token value stops at comma");
      ok(ws_digest_field(A, "NC", v, sizeof v) && streq(v, "00000001"),
         "key case-insensitive");
      ok(!ws_digest_field(A, "opaque", v, sizeof v), "missing key");
      ok(!ws_digest_field("", "nonce", v, sizeof v), "empty header");
      ok(ws_digest_field("nonce=\"unterminated", "nonce", v, sizeof v)
         && streq(v, "unterminated"),
         "unterminated quote consumes to end without crash");
      ok(ws_digest_field(A, "username", v, 4u) && streq(v, "Pi1"),
         "value truncated to out_sz");
      ok(!ws_digest_field("=,,=,\"", "nonce", v, sizeof v),
         "garbage pairs survive");

      md5_hex_t e;
      memcpy(e, "0123456789abcdef0123456789abcdef", MD5_HEX_LEN);
      ok(ws_hex_eq_ci(e, "0123456789abcdef0123456789abcdef"), "hex match");
      ok(ws_hex_eq_ci(e, "0123456789ABCDEF0123456789ABCDEF"),
         "hex match case-insensitive");
      ok(!ws_hex_eq_ci(e, "0123456789abcdef0123456789abcdee"),
         "one nibble off");
      ok(!ws_hex_eq_ci(e, "0123456789abcdef0123456789abcde"), "31 chars");
      ok(!ws_hex_eq_ci(e, "0123456789abcdef0123456789abcdeff"), "33 chars");
      ok(!ws_hex_eq_ci(e, ""), "empty received");

      ok(ws_digest_uri_matches("/a", "/a", NULL), "uri exact");
      ok(ws_digest_uri_matches("/a?q=1", "/a", "q=1"), "uri with query");
      ok(!ws_digest_uri_matches("/a?q=2", "/a", "q=1"), "query mismatch");
      ok(!ws_digest_uri_matches("/ab", "/a", NULL),
         "prefix is not a match (namespace-wide replay)");
      ok(!ws_digest_uri_matches("/x", "/", NULL), "root vs child");
      ok(ws_digest_uri_matches("http://h:80/a", "/a", NULL),
         "absolute-form accepted");
      ok(ws_digest_uri_matches("https://h", "/", NULL),
         "absolute-form with no path means /");
      ok(!ws_digest_uri_matches(NULL, "/a", NULL), "NULL field uri");
      ok(ws_digest_uri_matches("/a", "/a", "q=1"),
         "client may omit the query (documented tolerance)");
   }

   puts("== dav_memfind / dav_parse_http_date ==");
   {
      static const char body[] = "<x><Win32LastModifiedTime>date</x>";
      ok(dav_memfind(body, strlen(body), "Win32LastModifiedTime>") != NULL,
         "tag found");
      ok(dav_memfind(body, 5u, "Win32LastModifiedTime>") == NULL,
         "length-limited miss");
      ok(dav_memfind(body, strlen(body), "") == NULL, "empty needle");
      ok(dav_memfind("ab", 2u, "abc") == NULL, "needle longer than hay");

      uint16_t fd, ft;
      ok(dav_parse_http_date("Wed, 22 Jul 2026 09:00:00 GMT", 29u, &fd, &ft)
         && fd == (uint16_t)(((2026u - 1980u) << 9) | (7u << 5) | 22u)
         && ft == (uint16_t)(9u << 11),
         "RFC1123 date packs correctly");
      ok(dav_parse_http_date("22 Jul 2026 23:59:59", 20u, &fd, &ft)
         && ft == (uint16_t)((23u << 11) | (59u << 5) | 29u),
         "weekday optional, seconds halved");
      ok(dav_parse_http_date("Mon, 1 Jan 1980 00:00:00 GMT", 28u, &fd, &ft)
         && fd == (uint16_t)((1u << 5) | 1u) && ft == 0u,
         "FAT epoch");
      ok(!dav_parse_http_date("Wed, 22 Jul 1979 09:00:00 GMT", 29u, &fd, &ft),
         "pre-FAT year rejected");
      ok(!dav_parse_http_date("Wed, 22 Jul 2108 09:00:00 GMT", 29u, &fd, &ft),
         "post-FAT year rejected");
      ok(!dav_parse_http_date("Wed, 0 Jul 2026 09:00:00 GMT", 28u, &fd, &ft),
         "day 0 rejected");
      ok(!dav_parse_http_date("Wed, 32 Jul 2026 09:00:00 GMT", 29u, &fd, &ft),
         "day 32 rejected");
      ok(!dav_parse_http_date("Wed, 22 Jul 2026 24:00:00 GMT", 29u, &fd, &ft),
         "hour 24 rejected");
      ok(!dav_parse_http_date("Wed, 22 Xxx 2026 09:00:00 GMT", 29u, &fd, &ft),
         "bad month rejected");
      ok(!dav_parse_http_date("Wed, 22 Jul 2026 09.00.00 GMT", 29u, &fd, &ft),
         "bad separators rejected");
      ok(!dav_parse_http_date("", 0u, &fd, &ft), "empty rejected");
      ok(!dav_parse_http_date("Wed, 22 Jul 2026 09:00:00 GMT", 8u, &fd, &ft),
         "len parameter clips the input");
      /* longer than the internal 40-byte buffer: must clamp, not overrun */
      {
         static const char long_date[] =
            "Wednesday-the-longest, 22 Jul 2026 09:00:00 GMT trailing junk";
         (void)dav_parse_http_date(long_date, strlen(long_date), &fd, &ft);
         ok(1, "overlong date clamped without overrun (ASan-checked)");
      }
   }

   printf("\n%d checks, %d failures\n", checks, fails);
   return fails ? 1 : 0;
}
