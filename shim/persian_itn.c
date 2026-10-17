// Persian inverse text normalization for the Shenava ASR output.
//
// Shenava writes numbers as words ("بیست و پنج", "دو هزار و چهارصد"). This turns
// well-formed runs of number words into Persian digits ("۲۵", "۲۴۰۰") and leaves
// everything else as it is. A run follows Persian number grammar: components
// joined by "و" in falling order, optionally scaled by هزار / میلیون / میلیارد.
// Adjacent components without "و" ("دو سه", "a couple") end the run, so they are
// not summed. A lone "یک" stays a word, since it is usually the article "a".

#include "persian_itn.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *w;
  long long v;
} Word;

// Additive components. The order of a value is the size of the next smaller
// component that may follow it after "و" (e.g. after 300 a tens or units word).
static const Word kAdd[] = {
    {"صفر", 0},      {"یک", 1},       {"یه", 1},        {"دو", 2},        {"سه", 3},
    {"چهار", 4},     {"پنج", 5},      {"شش", 6},        {"شیش", 6},       {"هفت", 7},
    {"هشت", 8},      {"نه", 9},       {"ده", 10},       {"یازده", 11},    {"دوازده", 12},
    {"سیزده", 13},   {"چهارده", 14},  {"پانزده", 15},   {"پونزده", 15},   {"شانزده", 16},
    {"شونزده", 16},  {"هفده", 17},    {"هیفده", 17},    {"هجده", 18},     {"هیجده", 18},
    {"نوزده", 19},   {"بیست", 20},    {"سی", 30},       {"چهل", 40},      {"پنجاه", 50},
    {"شصت", 60},     {"هفتاد", 70},   {"هشتاد", 80},    {"نود", 90},      {"صد", 100},
    {"یکصد", 100},   {"دویست", 200},  {"سیصد", 300},    {"چهارصد", 400},  {"پانصد", 500},
    {"پونصد", 500},  {"ششصد", 600},   {"شیشصد", 600},   {"هفتصد", 700},   {"هشتصد", 800},
    {"نهصد", 900},
};
static const Word kScale[] = {{"هزار", 1000LL}, {"میلیون", 1000000LL}, {"میلیارد", 1000000000LL}};

static int lookup(const Word *t, int n, const char *s, size_t len, long long *v) {
  for (int i = 0; i < n; i++) {
    if (strlen(t[i].w) == len && memcmp(t[i].w, s, len) == 0) {
      *v = t[i].v;
      return 1;
    }
  }
  return 0;
}

// 0 = not a number word, 1 = additive, 2 = scale.
static int classify(const char *s, size_t len, long long *v) {
  if (lookup(kAdd, sizeof kAdd / sizeof *kAdd, s, len, v)) return 1;
  if (lookup(kScale, sizeof kScale / sizeof *kScale, s, len, v)) return 2;
  return 0;
}

// Largest component allowed after this one: hundreds -> 99, tens -> 9, else none.
static long long room_after(long long v) {
  if (v >= 100) return 99;
  if (v >= 20 && v % 10 == 0) return 9;
  return -1;
}

typedef struct {
  const char *s;
  size_t len;
} Tok;

static int is_and(Tok t) { return t.len == 2 && memcmp(t.s, "و", 2) == 0; }

static size_t put_digits(char *o, long long v) {
  char tmp[32];
  int n = 0;
  do {
    tmp[n++] = (char)('0' + v % 10);
    v /= 10;
  } while (v);
  size_t w = 0;
  while (n--) {
    // U+06F0 .. U+06F9 (EXTENDED ARABIC-INDIC DIGIT), UTF-8 DB B0..DB B9
    o[w++] = (char)0xDB;
    o[w++] = (char)(0xB0 + (tmp[n] - '0'));
  }
  return w;
}

// Parses the longest well-formed number starting at tokens[i]. Returns the
// number of tokens used (0 if none) and the value.
static int parse_number(const Tok *t, int n, int i, long long *value) {
  long long total = 0, cur = 0, last = -1, last_scale = 0;
  int used = 0, j = i, have_cur = 0, expect_add = 1;
  while (j < n) {
    long long v;
    int k = classify(t[j].s, t[j].len, &v);
    if (k == 1) {
      if (!expect_add) break;  // two components without "و"
      if (have_cur && (room_after(last) < 0 || v > room_after(last))) break;
      cur += v;
      last = v;
      have_cur = 1;
      expect_add = 0;
      used = j - i + 1;
      j++;
    } else if (k == 2) {
      if (last_scale && v >= last_scale) break;
      if (!have_cur && !expect_add) break;
      if (!have_cur && used) break;  // "و هزار"
      total += (have_cur ? cur : 1) * v;
      cur = 0;
      have_cur = 0;
      last = -1;
      last_scale = v;
      expect_add = 0;
      used = j - i + 1;
      j++;
    } else if (is_and(t[j]) && !expect_add && j + 1 < n) {
      long long nv;
      int nk = classify(t[j + 1].s, t[j + 1].len, &nv);
      if (nk == 0) break;
      expect_add = 1;
      if (nk == 1 && have_cur && (room_after(last) < 0 || nv > room_after(last))) break;
      if (nk == 1 && !have_cur && last_scale && nv >= last_scale) break;
      if (nk == 2 && !have_cur) break;
      j++;  // consume "و"; the next word is consumed by the loop
    } else {
      break;
    }
  }
  *value = total + cur;
  return used;
}

char *persian_itn(const char *text) {
  if (!text) return NULL;
  size_t L = strlen(text);
  // Tokenize on ASCII spaces; keep the separators by position.
  int cap = 16, n = 0;
  Tok *t = malloc(sizeof *t * cap);
  for (size_t p = 0; p < L;) {
    while (p < L && text[p] == ' ') p++;
    if (p >= L) break;
    size_t q = p;
    while (q < L && text[q] != ' ') q++;
    if (n == cap) t = realloc(t, sizeof *t * (cap *= 2));
    t[n].s = text + p;
    t[n].len = q - p;
    n++;
    p = q;
  }
  // Digits never take more bytes than the words they replace, but leave room.
  char *out = malloc(L * 2 + 64);
  size_t o = 0;
  for (int i = 0; i < n;) {
    long long v = 0;
    int used = parse_number(t, n, i, &v);
    if (o) out[o++] = ' ';
    if (used > 0 && !(used == 1 && v == 1)) {
      o += put_digits(out + o, v);
      i += used;
    } else {
      memcpy(out + o, t[i].s, t[i].len);
      o += t[i].len;
      i++;
    }
  }
  out[o] = 0;
  free(t);
  return out;
}
