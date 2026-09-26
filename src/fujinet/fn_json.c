/* fn_json.c - the network device's JSON translation.

   fujinet-nio src/lib/json_content_translator.cpp, with the same cJSON (the
   submodule is pinned to nio's commit): parse the body, look the selector up
   with cJSONUtils_GetPointer (RFC 6901, keys matched without regard to
   case, as cJSON does), and flatten what it names to text:
      string        its characters
      number        an integer if it is one within +-2^53, else %.10g
      true/false    TRUE / FALSE
      null          NULL
      object        "key\nvalue" per member, members joined by "\n"
      array         one element per line
   A body that does not parse, or a selector naming nothing, gives no text -
   still a success, as in nio. */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "fn_json.h"
#include "cJSON/cJSON.h"
#include "cJSON/cJSON_Utils.h"

typedef struct {
   char *p;
   uint32_t len, cap;
   bool oom;
} text_t;

static void put(text_t *t, const char *s, size_t n)
{
   if (t->oom || n == 0)
      return;
   if (t->len + n + 1u > t->cap) {
      uint32_t cap = t->cap ? t->cap : 256u;
      while (t->len + n + 1u > cap)
         cap *= 2u;
      char *p = realloc(t->p, cap);
      if (!p) {
         t->oom = true;
         return;
      }
      t->p = p;
      t->cap = cap;
   }
   memcpy(t->p + t->len, s, n);
   t->len += (uint32_t)n;
   t->p[t->len] = '\0';
}

static void puts_(text_t *t, const char *s) { put(t, s, strlen(s)); }

static bool is_approx_integer(double v)
{
   double i = 0.0;
   double frac = modf(v, &i);
   /* frac == 0.0, spelt so -Wfloat-equal accepts it (NaN is false either way) */
   return frac >= 0.0 && frac <= 0.0 && v >= -9007199254740992.0 && v <= 9007199254740992.0;
}

/* The release build's printf has no float or long long support (newlib
   nano, by choice - see src/CMakeLists.txt), so both formats are done here. */

static void fmt_int64(char *out, int64_t v)
{
   char tmp[24];
   int n = 0;
   uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
   do {
      tmp[n++] = (char)('0' + u % 10u);
      u /= 10u;
   } while (u);
   if (v < 0)
      *out++ = '-';
   while (n)
      *out++ = tmp[--n];
   *out = '\0';
}

/* The exact decimal digits of a finite, non-zero |v|: v = m * 2^e, so the
   value is the integer m * 2^e (e >= 0) or m * 5^-e scaled by 10^e (e < 0).
   Built in base-1e9 limbs, little end first.  Returns the digit count and
   sets *decpt, the number of digits before the decimal point. */
#define LIMB 1000000000u
static int exact_digits(double v, char *s, int *decpt)
{
   uint64_t bits, m;
   memcpy(&bits, &v, sizeof bits);
   int be = (int)((bits >> 52) & 0x7FFu);
   m = bits & (((uint64_t)1 << 52) - 1u);
   if (be)
      m |= (uint64_t)1 << 52;
   else
      be = 1;                            /* subnormal */
   int e = be - 1075;

   uint32_t L[100];                      /* 5^1074 * 2^53 < 10^767 = 86 limbs */
   int n = 0;
   do {
      L[n++] = (uint32_t)(m % LIMB);
      m /= LIMB;
   } while (m);
   int k = e < 0 ? -e : 0;               /* value = L * 10^-k */
   int rest = e < 0 ? -e : e;
   while (rest > 0) {
      int step;
      uint32_t mul;
      if (e > 0) {
         step = rest < 30 ? rest : 30;
         mul = 1u << step;
      } else {
         step = rest < 13 ? rest : 13;
         mul = 1;
         for (int i = 0; i < step; i++)
            mul *= 5u;
      }
      uint64_t carry = 0;
      for (int i = 0; i < n; i++) {
         uint64_t t = (uint64_t)L[i] * mul + carry;
         L[i] = (uint32_t)(t % LIMB);
         carry = t / LIMB;
      }
      while (carry) {
         L[n++] = (uint32_t)(carry % LIMB);
         carry /= LIMB;
      }
      rest -= step;
   }
   int len = 0;
   for (int i = n - 1; i >= 0; i--) {    /* top limb unpadded, the rest 9 digits */
      char d[9];
      uint32_t x = L[i];
      for (int j = 8; j >= 0; j--) {
         d[j] = (char)('0' + x % 10u);
         x /= 10u;
      }
      int from = 0;
      if (i == n - 1)
         while (from < 8 && d[from] == '0')
            from++;
      memcpy(s + len, d + from, (size_t)(9 - from));
      len += 9 - from;
   }
   *decpt = len - k;
   return len;
}

/* printf's "%.10g" for a finite, non-zero v: the ten significant digits
   correctly rounded (a tie to even, as printf), then %g's layout - trailing
   zeros dropped, exponent form when the exponent is below -4 or not below
   10.  cJSON never parses an infinity or a NaN into a number. */
static void fmt_g10(char *out, double v)
{
   static char s[800];
   int decpt;
   bool neg = v < 0.0;
   int len = exact_digits(neg ? -v : v, s, &decpt);
   char dig[11];
   int nd = len < 10 ? len : 10;
   memcpy(dig, s, (size_t)nd);
   if (len > 10) {
      bool beyond = false;               /* anything non-zero past the '5'? */
      for (int i = 11; i < len && !beyond; i++)
         beyond = s[i] != '0';
      if (s[10] > '5' || (s[10] == '5' && (beyond || (dig[9] - '0') % 2))) {
         int i = 9;
         while (i >= 0 && dig[i] == '9')
            dig[i--] = '0';
         if (i >= 0)
            dig[i]++;
         else {                          /* 9999999999 -> 1000000000 */
            dig[0] = '1';
            decpt++;
         }
      }
   }
   while (nd > 1 && dig[nd - 1] == '0')
      nd--;
   if (neg)
      *out++ = '-';
   int x = decpt - 1;                   /* the decimal exponent */
   if (x < -4 || x >= 10) {
      *out++ = dig[0];
      if (nd > 1) {
         *out++ = '.';
         memcpy(out, dig + 1, (size_t)nd - 1u);
         out += nd - 1;
      }
      *out++ = 'e';
      *out++ = x < 0 ? '-' : '+';
      int ax = x < 0 ? -x : x;
      if (ax >= 100)
         *out++ = (char)('0' + ax / 100);
      *out++ = (char)('0' + ax / 10 % 10);
      *out++ = (char)('0' + ax % 10);
   } else if (x < 0) {
      *out++ = '0';
      *out++ = '.';
      for (int i = x + 1; i < 0; i++)
         *out++ = '0';
      memcpy(out, dig, (size_t)nd);
      out += nd;
   } else {
      for (int i = 0; i <= x; i++)
         *out++ = i < nd ? dig[i] : '0';
      if (nd > x + 1) {
         *out++ = '.';
         memcpy(out, dig + x + 1, (size_t)(nd - x - 1));
         out += nd - x - 1;
      }
   }
   *out = '\0';
}

static void item(text_t *t, const cJSON *it)
{
   char num[40];
   if (cJSON_IsString(it)) {
      const char *s = cJSON_GetStringValue(it);
      if (s)
         puts_(t, s);
   } else if (cJSON_IsBool(it))
      puts_(t, cJSON_IsTrue(it) ? "TRUE" : "FALSE");
   else if (cJSON_IsNull(it))
      puts_(t, "NULL");
   else if (cJSON_IsNumber(it)) {
      double v = cJSON_GetNumberValue(it);
      if (is_approx_integer(v))
         fmt_int64(num, (int64_t)v);
      else
         fmt_g10(num, v);
      puts_(t, num);
   } else if (cJSON_IsObject(it)) {
      for (const cJSON *c = it->child; c; c = c->next) {
         puts_(t, c->string ? c->string : "");
         puts_(t, "\n");
         item(t, c);
         if (c->next)
            puts_(t, "\n");
      }
   } else if (cJSON_IsArray(it)) {
      for (const cJSON *c = it->child; c; c = c->next) {
         if (c != it->child)
            puts_(t, "\n");
         item(t, c);
      }
   }
}

bool fn_json_translate(const char *body, const char *selector, char **out, uint32_t *len)
{
   text_t t = { 0 };
   *out = NULL;
   *len = 0;
   cJSON *json = cJSON_Parse(body);
   if (json) {
      cJSON *it = cJSONUtils_GetPointer(json, selector);
      if (it)
         item(&t, it);
      cJSON_Delete(json);
   }
   if (t.oom) {
      free(t.p);
      return false;
   }
   *out = t.p;
   *len = t.len;
   return true;
}
