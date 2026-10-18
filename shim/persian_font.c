// Vazirmatn for Persian text in Foresight.
//
// Foresight's own fonts have no Arabic-script glyphs, so Flutter's text engine
// (Skia) asks CoreText for a fallback font for each Persian run, through
// CTFontCreateForString. We interpose that call: when the run starts with an
// Arabic-script character and the Vazirmatn fonts are installed, we return
// Vazirmatn at the same size and the closest weight. Everything else goes to
// CoreText unchanged.
//
// Fonts: ~/Library/Application Support/farsight/fonts/Vazirmatn-*.ttf
// (setup-persian.sh), registered for this process only. FARSIGHT_FONT=system
// turns this off.

#include "persian_font.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreText/CoreText.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void logf_(const char *fmt, ...);

static int g_enabled;
static int g_logged;

static const char *kWeights[] = {"Thin", "ExtraLight", "Light", "Regular", "Medium",
                                 "SemiBold", "Bold", "ExtraBold", "Black"};
// CoreText weight trait (-1..1) at which each Vazirmatn weight is the closest match.
static const double kWeightTrait[] = {-0.8, -0.6, -0.4, 0.0, 0.23, 0.3, 0.4, 0.56, 0.62};

void persian_font_init(const char *home) {
  const char *pref = getenv("FARSIGHT_FONT");
  if (pref && strcmp(pref, "system") == 0) {
    logf_("font: system (FARSIGHT_FONT=system)");
    return;
  }
  int registered = 0;
  for (size_t i = 0; i < sizeof kWeights / sizeof *kWeights; i++) {
    char path[1200];
    snprintf(path, sizeof path, "%s/Library/Application Support/farsight/fonts/Vazirmatn-%s.ttf", home,
             kWeights[i]);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    if (!url) continue;
    CFErrorRef err = NULL;
    if (CTFontManagerRegisterFontsForURL(url, kCTFontManagerScopeProcess, &err)) registered++;
    else if (err) {
      // Already registered (e.g. installed system-wide) is fine too.
      if (CFErrorGetCode(err) == kCTFontManagerErrorAlreadyRegistered) registered++;
      CFRelease(err);
    }
    CFRelease(url);
  }
  g_enabled = registered > 0;
  logf_("font: %s (%d Vazirmatn weights registered)", g_enabled ? "Vazirmatn" : "system", registered);
}

static int is_arabic_script(UniChar c) {
  return (c >= 0x0600 && c <= 0x06FF) || (c >= 0x0750 && c <= 0x077F) || (c >= 0x08A0 && c <= 0x08FF) ||
         (c >= 0xFB50 && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF);
}

static CTFontRef vazirmatn_for(CTFontRef current, CFStringRef string, CFRange range) {
  if (!g_enabled || !string || range.length <= 0) return NULL;
  // The first non-neutral character decides (spaces, digits and ZWNJ are shared).
  UniChar c = 0;
  CFIndex end = range.location + (range.length < 16 ? range.length : 16);
  for (CFIndex i = range.location; i < end; i++) {
    UniChar u = CFStringGetCharacterAtIndex(string, i);
    if (u == ' ' || u == 0x200C || u == 0x200D || (u >= '0' && u <= '9')) continue;
    c = u;
    break;
  }
  if (!is_arabic_script(c)) return NULL;

  CGFloat size = current ? CTFontGetSize(current) : 14.0;
  double weight = 0;
  if (current) {
    CFDictionaryRef traits = CTFontCopyTraits(current);
    if (traits) {
      CFNumberRef w = CFDictionaryGetValue(traits, kCTFontWeightTrait);
      if (w) CFNumberGetValue(w, kCFNumberDoubleType, &weight);
      CFRelease(traits);
    }
  }
  size_t best = 3;
  double best_d = 10;
  for (size_t i = 0; i < sizeof kWeightTrait / sizeof *kWeightTrait; i++) {
    double d = kWeightTrait[i] - weight;
    if (d < 0) d = -d;
    if (d < best_d) best_d = d, best = i;
  }
  char name[64];
  snprintf(name, sizeof name, "Vazirmatn-%s", kWeights[best]);
  CFStringRef cfname = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
  CTFontRef font = CTFontCreateWithName(cfname, size, NULL);
  // CoreText substitutes another font for an unknown name; only accept Vazirmatn.
  int ok = 0;
  if (font) {
    CFStringRef ps = CTFontCopyPostScriptName(font);
    ok = ps && CFStringCompare(ps, cfname, 0) == kCFCompareEqualTo;
    if (ps) CFRelease(ps);
  }
  CFRelease(cfname);
  if (!ok) {
    if (font) CFRelease(font);
    return NULL;
  }
  if (!g_logged) {
    g_logged = 1;
    logf_("font: Vazirmatn in use (first run U+%04X, %s, %.0fpt)", c, name, size);
  }
  return font;
}

static CTFontRef my_CTFontCreateForString(CTFontRef current, CFStringRef string, CFRange range) {
  CTFontRef f = vazirmatn_for(current, string, range);
  return f ? f : CTFontCreateForString(current, string, range);
}

static CTFontRef my_CTFontCreateForStringWithLanguage(CTFontRef current, CFStringRef string, CFRange range,
                                                      CFStringRef language) {
  CTFontRef f = vazirmatn_for(current, string, range);
  return f ? f : CTFontCreateForStringWithLanguage(current, string, range, language);
}

__attribute__((used)) static struct {
  const void *replacement;
  const void *replacee;
} font_interposers[] __attribute__((section("__DATA,__interpose"))) = {
    {(const void *)my_CTFontCreateForString, (const void *)CTFontCreateForString},
    {(const void *)my_CTFontCreateForStringWithLanguage, (const void *)CTFontCreateForStringWithLanguage},
};
