#!/usr/bin/env python3
"""Generates driver.c from corpus.c's own non-static `int test_*(void)`
declarations, so the native reference driver can never drift out of sync
with the corpus by hand-edited omission -- see run.sh."""
import re
import sys

corpus = open(sys.argv[1], encoding="utf-8").read()
names = re.findall(r"^int (test_[a-zA-Z0-9_]+)\(void\)", corpus, re.M)
if not names:
    sys.exit("gen_driver.py: found no test_* functions in " + sys.argv[1])

print('#include <stdio.h>')
for n in names:
    print(f"int {n}(void);")
print("int main(void) {")
for n in names:
    print(f'    printf("{n}=%d\\n", {n}());')
print("    return 0;")
print("}")
