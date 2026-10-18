// Tests for persian_fix. Build and run: shim/test/itn_test.sh
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../persian_text.h"

#define Z "\xE2\x80\x8C"
static int fails;

static void check(const char *in, const char *want) {
  char *got = persian_fix(in);
  if (strcmp(got, want) != 0) {
    printf("FAIL\n  in:   %s\n  want: %s\n  got:  %s\n", in, want, got);
    fails++;
  }
  free(got);
}

int main(void) {
  check("می شه", "می" Z "شه");
  check("نمی تونیم", "نمی" Z "تونیم");
  check("طرح های پیشین", "طرح" Z "های پیشین");
  check("فیک ها", "فیک" Z "ها");
  check("خانه ای بزرگ", "خانه" Z "ای بزرگ");
  check("ای خدا", "ای خدا");
  check("می", "می");
  check("میز کار", "میز کار");
  check("هادی ها", "هادی" Z "ها");
  check("علي كتاب", "علی کتاب");
  check("سال ١٤٠٣", "سال ۱۴۰۳");
  check("ما به نتیجه رسیدیم که می تونیم", "ما به نتیجه رسیدیم که می" Z "تونیم");
  check("API ها", "API ها");
  check("", "");
  // punctuation
#define P(in, want) do { char *g = persian_punctuate(in, 1); if (strcmp(g, want)) { printf("FAIL punct\n  in:   %s\n  want: %s\n  got:  %s\n", in, want, g); fails++; } free(g); } while (0)
  P("تا جایی که می" Z "شه در موردشون صحبت کرد", "تا جایی که می" Z "شه در موردشون صحبت کرد.");
  P("در جلسه دیزاین امروز ما بررسی می" Z "کنیم که", "در جلسه دیزاین امروز ما بررسی می" Z "کنیم که");
  P("چرا این طرح رو انتخاب کردیم", "چرا این طرح رو انتخاب کردیم؟");
  P("قراره فردا تحویل بدیم نه", "قراره فردا تحویل بدیم نه؟");
  P("طرح خوبیه ولی رنگش باید عوض بشه", "طرح خوبیه، ولی رنگش باید عوض بشه.");
  P("خوبه ولی", "خوبه ولی");
  P("سلام", "سلام.");
  P("واقعا؟", "واقعا؟");
  P("", "");
  { char *g = persian_punctuate("یک اثر هنری به عنوان یک شیء فیزیکی", 0);
    if (strcmp(g, "یک اثر هنری به عنوان یک شیء فیزیکی")) { printf("FAIL cut phrase got %s\n", g); fails++; } free(g); }
  if (!fails) printf("text: all passed\n");
  return fails != 0;
}
