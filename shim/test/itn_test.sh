#!/bin/sh
# Runs the Persian text tests (number ITN and orthography cleanup).
cd "$(dirname "$0")" && T=$(mktemp -d) &&
cc -Wall -O2 -o "$T/itn" itn_test.c ../persian_itn.c && "$T/itn" &&
cc -Wall -O2 -o "$T/text" text_test.c ../persian_text.c && "$T/text"
