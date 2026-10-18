#pragma once

// Cleans Persian transcript text: Arabic yeh/kaf and digits -> Persian forms, and
// zero-width non-joiners for «می‌», «‌ها» and «ه‌ای». Returns a malloc'ed string.
char *persian_fix(const char *text);

// Adds sentence-final «.» or «؟» and «،» before «اما/ولی/چون/یعنی». One call per
// phrase. complete = 0 when the phrase was cut by the length limit rather than a
// pause: no final «.» then. Returns a malloc'ed string.
char *persian_punctuate(const char *text, int complete);
