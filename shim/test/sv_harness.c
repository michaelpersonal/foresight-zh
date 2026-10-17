// Feeds a raw 48 kHz float32 capture through the engine in Foresight-sized chunks.
// FORESIGHT_ZH_LANG=persian selects the Persian model instead of SenseVoice.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../sensevoice_engine.h"

void logf_(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr); }

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s capture.f32\n", argv[0]); return 2; }
  const char *lang = getenv("FORESIGHT_ZH_LANG");
  int fa = lang && (strcmp(lang, "persian") == 0 || strcmp(lang, "fa") == 0);
  if (!sv_available(getenv("HOME"), fa ? SV_LANG_PERSIAN : SV_LANG_SENSEVOICE)) return 1;
  double t0 = now();
  sv_start_loading();
  FILE *f = fopen(argv[1], "rb");
  static float buf[7680];
  SvResult r[16];
  size_t n;
  double worst = 0;
  while ((n = fread(buf, 4, 7680, f)) > 0) {
    double t = now();
    int k = sv_push(0, buf, (int)n, r, 16);
    if (t0 > 0) { fprintf(stderr, "first push returned after %.2fs (includes model load)\n", now() - t0); t0 = 0; }
    else if (now() - t > worst) worst = now() - t;
    for (int i = 0; i < k; i++) { printf("[%.2f-%.2f] %s\n", r[i].start_us / 1e6, r[i].end_us / 1e6, r[i].text); free(r[i].text); }
  }
  int k = sv_finish(0, r, 16);
  for (int i = 0; i < k; i++) { printf("[%.2f-%.2f] %s (flush)\n", r[i].start_us / 1e6, r[i].end_us / 1e6, r[i].text); free(r[i].text); }
  fprintf(stderr, "slowest push: %.3fs\n", worst);
  return 0;
}
