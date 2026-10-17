#!/bin/sh
# Runs the Persian ITN tests.
cd "$(dirname "$0")" && cc -Wall -O2 -o /tmp/itn_test itn_test.c ../persian_itn.c && /tmp/itn_test
