#!/usr/bin/env python3
"""
Converts an i386 test binary into a C header the legacy i386 kernel embeds.

RISC-V programs are ordinary ELF files in its required initrd and do not use
this script. The i386 boot path predates that filesystem.

Usage: gen_user_test_header.py <input-file> <output-var-name> <load-addr-or-blank>

For the raw hand-assembled payload (flat binary, fixed load address):
  gen_user_test_header.py user_test/user_test.bin user_test 0x800000
For a real ELF (no fixed load address -- the ELF loader reads it from
the file itself):
  gen_user_test_header.py user_test/hello.elf hello_elf
"""
import sys

path = sys.argv[1]
varname = sys.argv[2]
load_addr = sys.argv[3] if len(sys.argv) > 3 else None

data = open(path, "rb").read()
guard = f"{varname.upper()}_PAYLOAD_H"

print(f"/* GENERATED FILE from {path} -- see gen_user_test_header.py. Do not hand-edit. */")
print(f"#ifndef {guard}")
print(f"#define {guard}")
print()
if load_addr:
	print(f"#define {varname.upper()}_LOAD_ADDR {load_addr}u")
	print(f"#define {varname.upper()}_ENTRY {load_addr}u")
print(f"#define {varname.upper()}_SIZE {len(data)}u")
print()
print(f"static const unsigned char {varname}_payload[] = {{")
for i in range(0, len(data), 12):
	chunk = data[i:i+12]
	print("\t" + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
print("};")
print()
print("#endif")
