#!/usr/bin/env python3

"""
Emulate the linker '--wrap' option on macOS for the gtest unit tests.

The macOS linker does not support '--wrap', so we rename symbols in the
object files with llvm-objcopy instead:
  * In the object file that defines a wrapped function, the symbol is
    renamed to '___real_<name>'.
  * In every other object file, the reference is renamed to '___wrap_<name>'.

The result is the same as '-Wl,--wrap=<name>' on GNU ld: all calls go to
'__wrap_<name>' (defined in generated_wrappers.cpp), which can dispatch to
the mock or to '__real_<name>'.

The wrapped object files are written to '--output-dir' and collected into
the static library given by '--archive'.

This is the CMake counterpart of the 'wrap-object' rule in Makefile.
"""

import argparse
import hashlib
import os
import subprocess
import sys

import wrapper_util
from wrapper_util import find_wrapper_functions_in_header

# Magic numbers of LLVM bitcode files (raw bitcode and bitcode wrapper).
BITCODE_MAGIC = (b"BC\xc0\xde", b"\xde\xc0\x17\x0b")


def is_bitcode(object_file):
    """Return True if 'object_file' is LLVM bitcode instead of a Mach-O object."""
    with open(object_file, "rb") as f:
        return f.read(4) in BITCODE_MAGIC


def defined_text_symbols(nm, object_file):
    """Return the set of global text symbols defined by 'object_file'."""
    output = subprocess.run(
        [nm, "--defined-only", "--extern-only", object_file],
        check=True,
        capture_output=True,
        text=True,
    ).stdout

    symbols = set()
    for line in output.splitlines():
        fields = line.split()
        # Format: <address> T <symbol>
        if len(fields) == 3 and fields[1] == "T":
            symbols.add(fields[2])
    return symbols


def write_redefine_syms(methods, defined_symbols, syms_file):
    """Write the symbol renaming rules for llvm-objcopy '--redefine-syms'."""
    with open(syms_file, "w") as f:
        for method in methods:
            # On macOS, C symbols carry a leading underscore.
            if method.name in defined_symbols or "_" + method.name in defined_symbols:
                f.write("_{0} ___real_{0}\n".format(method.name))
            else:
                f.write("_{0} ___wrap_{0}\n".format(method.name))


def is_up_to_date(target, dependencies):
    """Return True if 'target' exists and is newer than all 'dependencies'."""
    if not os.path.exists(target):
        return False
    target_mtime = os.path.getmtime(target)
    return all(os.path.getmtime(dep) <= target_mtime for dep in dependencies)


def output_name(object_file):
    """
    Build a unique output file name for 'object_file'.

    Two object files can share a base name, so a short hash of the path is
    added. The name must not depend on the position of the file in the input
    list, otherwise the up to date check below could compare an output file
    with the wrong input file.
    """
    digest = hashlib.sha1(os.path.abspath(object_file).encode()).hexdigest()[:8]
    return "{}-{}.o".format(os.path.basename(object_file), digest)


def wrap_object(args, methods, object_file, output_file):
    """Rename the wrapped symbols of a single object file."""
    source = object_file
    temp_object = output_file + ".tmp.o"

    if is_bitcode(object_file):
        if not args.llc:
            print("Error: '{}' is LLVM bitcode but llc was not found".format(object_file), file=sys.stderr)
            sys.exit(1)
        subprocess.run([args.llc, object_file, "-filetype=obj", "-o", temp_object], check=True)
        source = temp_object

    syms_file = output_file + "-wrap-syms"
    partial_output = output_file + ".partial.o"
    write_redefine_syms(methods, defined_text_symbols(args.nm, source), syms_file)
    subprocess.run(
        [args.objcopy, "--redefine-syms=" + syms_file, source, partial_output],
        check=True,
    )

    # Put the result in place only after llvm-objcopy succeeded. A killed or
    # failed run must not leave a partial object file behind, because the mtime
    # check above would treat it as up to date.
    os.replace(partial_output, output_file)

    os.remove(syms_file)
    if os.path.exists(temp_object):
        os.remove(temp_object)


def main():
    parser = argparse.ArgumentParser(description="Wrap symbols in object files on macOS")
    parser.add_argument("--nm", required=True, help="path to llvm-nm")
    parser.add_argument("--objcopy", required=True, help="path to llvm-objcopy")
    parser.add_argument("--ar", required=True, help="path to ar")
    parser.add_argument("--llc", default="", help="path to llc (only needed for LTO builds)")
    parser.add_argument("--wrappers", required=True, help="path to wrappers.h")
    parser.add_argument("--output-dir", required=True, help="directory for the wrapped object files")
    parser.add_argument("--archive", required=True, help="static library to create from the wrapped objects")
    parser.add_argument("objects", nargs="+", help="object files to wrap")
    args = parser.parse_args()

    methods = find_wrapper_functions_in_header(args.wrappers)
    os.makedirs(args.output_dir, exist_ok=True)

    # A change in wrappers.h or in the wrapping logic invalidates every object.
    tools = [
        args.wrappers,
        os.path.abspath(__file__),
        os.path.abspath(wrapper_util.__file__),
    ]

    wrapped_objects = []
    for object_file in args.objects:
        output_file = os.path.join(args.output_dir, output_name(object_file))
        if not is_up_to_date(output_file, [object_file] + tools):
            wrap_object(args, methods, object_file, output_file)
        wrapped_objects.append(output_file)

    if os.path.exists(args.archive):
        os.remove(args.archive)
    subprocess.run([args.ar, "rcs", args.archive] + wrapped_objects, check=True)


if __name__ == "__main__":
    main()
