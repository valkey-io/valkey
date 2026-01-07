#!/usr/bin/env python
"""
Utility functions for parsing '__wrap_' C function signatures from header files (e.g. wrappers.h).

Extracts return types, parameters, and function pointers, producing Method and Arg namedtuples.
This structured data is used by generate-wrappers.py to create MockValkey and RealValkey classes
for gtest-based tests.
"""
import re
from collections import namedtuple

Method = namedtuple("Method", "ret_type name full_args arg_string args")
Arg = namedtuple("Arg", "type name")

def split_args(arg_string):
    """
    Split a C-style argument string by commas, but ignore commas inside parentheses.
    Example:
        "int x, void (*cb)(int, int), char* buf"
    becomes:
        ["int x", "void (*cb)(int, int)", "char* buf"]
    """
    args = []
    current = []
    depth = 0
    for char in arg_string:
        if char == '(':
            depth += 1
            current.append(char)
        elif char == ')':
            depth -= 1
            current.append(char)
        elif char == ',' and depth == 0:
            # Split only at top-level commas
            args.append(''.join(current).strip())
            current = []
        else:
            current.append(char)
    if current:
        args.append(''.join(current).strip())
    # Single "void" means no args
    if len(args) == 1 and args[0] == "void":
        return []
    # Remove variadic "..."
    if args and args[-1] == "...":
        args = args[:-1]
    return args


def find_wrapper_functions_in_header(header_file_name):
    """
    Parse a header file and extract all functions starting with '__wrap_'.

    Each function is returned as a Method namedtuple containing:
        - ret_type: the return type of the function
        - name: the function name (without __wrap_)
        - full_args: raw argument string from the header
        - arg_string: comma-separated type + name strings for declarations
        - args: list of Arg namedtuples (type, name)

    This parser recognizes two types of arguments:

    1. Normal arguments (e.g., int x, char* buffer)
    2. Function pointer arguments (e.g., int *(fp)(int, void*))
    """
    methods = []

    with open(header_file_name, 'r') as f:
        for line in f:
            # Match a function signature starting with __wrap_
            m = re.match(r"(.*)__wrap_(\w+)\((.*)\);", line)
            if not m:
                continue

            method_ret_type = m.group(1).strip()
            method_name = m.group(2).strip()
            full_args = m.group(3)

            args_declaration = []
            args_definition = []

            for arg in split_args(full_args):

                # Normal argument
                m_normal = re.match(r"(.+?)\s*((?:\*+\s*)*)([a-zA-Z_][a-zA-Z0-9_]*\s*(?:\[[^\]]*\])*)$", arg)
                if m_normal:
                    arg_type = (m_normal.group(1) + m_normal.group(2)).strip()
                    arg_name = m_normal.group(3).strip()
                    args_definition.append(Arg(arg_type, arg_name))
                    args_declaration.append(Arg(arg_type, arg_name))
                    continue

                # Function pointer argument
                m_func_ptr = re.match(r"(.+?)\s*\(\s*(?:\*\s*)?([a-zA-Z0-9_]+)\s*\)\s*\((.*)\)", arg)
                if m_func_ptr:
                    arg_ret_type = m_func_ptr.group(1).strip()
                    arg_name = m_func_ptr.group(2).strip()
                    params = m_func_ptr.group(3).strip()

                    arg_type = f"{arg_ret_type} ({arg_name})({params})"
                    args_definition.append(Arg(arg_type, arg_name))
                    args_declaration.append(Arg(arg_type, ""))
                    continue

                # If argument cannot be parsed
                print(f"WARNING: Could not parse argument: '{arg}', {line}")

            # Build comma-separated argument string for declaration
            arg_string = ', '.join(f"{x.type} {x.name}".strip() for x in args_declaration)

            # Append the method to the results
            methods.append(Method(
                ret_type=method_ret_type,
                name=method_name,
                full_args=full_args,
                arg_string=arg_string,
                args=args_definition
            ))

    return methods
