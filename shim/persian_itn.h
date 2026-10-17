#pragma once

// Rewrites spoken Persian numbers ("بیست و پنج") as Persian digits ("۲۵").
// Returns a malloc'ed string; the caller frees it. Words that are not part of a
// number pass through unchanged. Runs of spaces collapse to one.
char *persian_itn(const char *text);
