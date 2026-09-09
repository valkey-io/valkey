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
    generate-redefine-syms.py <input .o file>

Notes:
  * Must be run in the same directory as 'wrappers.h'
  * Input object file must be a Mach-O object file
  * Output is written to stdout in a format compatible with llvm-objcopy
"""
import sys
import shlex
import subprocess
import os

from wrapper_util import find_wrapper_functions_in_header


def wrap_symbols(methods, object_file):
    """
    For each function in `methods`, determine if it is defined in `object_file`.
    Print symbol redefinition lines for llvm-objcopy:
      * defined symbols -> ___real_<name>
      * undefined symbols -> ___wrap_<name>
    """
    safe_arg = shlex.quote(object_file)

    # List all defined global symbols (text section) in the object file
    cmd = "nm --defined-only --extern-only {} | grep ' T ' | awk '{{print $3}}'".format(safe_arg)
    try:
        output = subprocess.check_output(cmd, shell=True, text=True)
    except subprocess.CalledProcessError as e:
        print(f"Error running nm on '{object_file}': {e}", file=sys.stderr)
        sys.exit(1)

    defined_syms = set(output.split())  # faster lookup

    for wrapped_method in methods:
        # On macOS LLVM, symbols may have a leading underscore
        if wrapped_method.name in defined_syms or '_' + wrapped_method.name in defined_syms:
            print(f"_{wrapped_method.name} ___real_{wrapped_method.name}")
        else:
            print(f"_{wrapped_method.name} ___wrap_{wrapped_method.name}")

def main():
    # Check for single argument
    if len(sys.argv) != 2:
        print("Usage: generate-redefine-syms.py <input .o file>", file=sys.stderr)
        sys.exit(1)

    file_path = sys.argv[1]

    # Check file exists and is an object file
    if not os.path.isfile(file_path):
        print(f"Error: File '{file_path}' does not exist.", file=sys.stderr)
        sys.exit(1)

    # Parse the source file containing the wrappers
    wrapped_methods = find_wrapper_functions_in_header('wrappers.h')

    wrap_symbols(wrapped_methods, file_path)

if __name__ == "__main__":
    main()
