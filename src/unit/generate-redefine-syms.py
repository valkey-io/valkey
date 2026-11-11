#!/usr/bin/env python3

"""
Generate a symbol redefinition file for llvm-objcopy's --redefine-syms option.

On Darwin (macOS), the linker does not support the --wrap directive.
We emulate this behavior by renaming symbols using llvm-objcopy:
  * Symbols defined in the object file are renamed to __real_<symbol>
  * Symbols referenced elsewhere are renamed to __wrap_<symbol>

The script uses 'wrappers.h' to determine which functions to wrap and
analyzes the supplied .o file to determine which symbols are defined in it.

Usage:
    generate-redefine-syms.py [--nm <nm program>] <input .o file>

Notes:
  * Must be run in the same directory as 'wrappers.h'
  * Input object file must be a Mach-O object file
  * Output is written to stdout in a format compatible with llvm-objcopy
  * The logic is shared with wrap-objects.py (CMake build), see wrapper_util.py
"""
import argparse
import subprocess
import sys
import os

from wrapper_util import defined_text_symbols, find_wrapper_functions_in_header, redefine_syms_lines


def main():
    parser = argparse.ArgumentParser(description="Generate a symbol redefinition file for llvm-objcopy")
    parser.add_argument("--nm", default="nm", help="nm program to run (default: nm)")
    parser.add_argument("object_file", help="input .o file")
    args = parser.parse_args()

    # Check file exists and is an object file
    if not os.path.isfile(args.object_file):
        print(f"Error: File '{args.object_file}' does not exist.", file=sys.stderr)
        sys.exit(1)

    # Parse the source file containing the wrappers
    wrapped_methods = find_wrapper_functions_in_header('wrappers.h')

    try:
        defined_syms = defined_text_symbols(args.nm, args.object_file)
    except (OSError, subprocess.CalledProcessError) as e:
        print(f"Error running {args.nm} on '{args.object_file}': {e}", file=sys.stderr)
        sys.exit(1)

    sys.stdout.writelines(redefine_syms_lines(wrapped_methods, defined_syms))

if __name__ == "__main__":
    main()
