// Tests for persian_itn. Build and run: shim/test/itn_test.sh
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../persian_itn.h"

static int fails;

static void check(const char *in, const char *want) {
  char *got = persian_itn(in);
  if (strcmp(got, want) != 0) {
    printf("FAIL\n  in:   %s\n  want: %s\n  got:  %s\n", in, want, got);
    fails++;
  }
  free(got);
}

int main(void) {
  check("بیست و پنج", "۲۵");
  check("ساعت سه و نیم", "ساعت ۳ و نیم");
  check("دو هزار و چهارصد و پنجاه و شش تومان", "۲۴۵۶ تومان");
  check("سال هزار و چهارصد و سه", "سال ۱۴۰۳");
  check("دو میلیون و سیصد هزار", "۲۳۰۰۰۰۰");
  check("یک روز خوب", "یک روز خوب");
  check("یک میلیارد", "۱۰۰۰۰۰۰۰۰۰");
  check("دو سه نفر", "۲ ۳ نفر");
  check("صد و بیست", "۱۲۰");
  check("پنج و بیست", "۵ و ۲۰");
  check("سیصد و دوازده", "۳۱۲");
  check("هزار", "۱۰۰۰");
  check("من و تو", "من و تو");
  check("ده و", "۱۰ و");
  check("جلسه ی شماره ی هفت", "جلسه ی شماره ی ۷");
  check("", "");
  check("hello world", "hello world");
  if (!fails) printf("all passed\n");
  return fails != 0;
}
