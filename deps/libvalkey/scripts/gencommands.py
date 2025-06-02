#!/usr/bin/env python3

# Copyright (C) 2023 Viktor Soderqvist <viktor dot soderqvist at est dot tech>
# This file is released under the BSD license, see the COPYING file

# This script generates cmddef.h from the JSON files in the Valkey repo
# describing the commands. This is done manually when commands have been added
# to Valkey or when you want add more commands implemented in modules, etc.
#
# Usage: ./gencommands.py path/to/valkey/src/commands/*.json > cmddef.h
#
# Alternatively, the output of the script utils/generate-commands-json.py (which
# fetches the command metadata from a running Valkey node) or the file
# commands.json from the valkey-doc repo can be used as input to this script:
# https://github.com/valkey-io/valkey-doc/blob/main/commands.json
#
# Additional JSON files can be added to extend support for custom commands. The
# JSON file format is not fully documented but hopefully the format can be
# understood from reading the existing JSON files. Alternatively, you can read
# the source code of this script to see what it does.
#
# The key specifications part is documented here:
# https://valkey.io/docs/topics/key-specs/
#
# The discussion where this JSON format was added in Valkey is here:
# https://github.com/redis/redis/issues/9359
#
# For convenience, files on the output format like cmddef.h can also be used as
# input files to this script. It can be used for adding more commands to the
# existing set of commands, but please do not abuse it. Do not to write commands
# information directly in this format.

import glob
import json
import os
import sys
import re

# Returns True if any of the nested arguments is a key; False otherwise.
def any_argument_is_key(arguments):
    for arg in arguments:
        if arg.get("type") == "key":
            return True
        if "arguments" in arg and any_argument_is_key(arg["arguments"]):
            return True
    return False

# Returns a tuple (method, index) where method is one of the following:
#
#     NONE          = No keys
#     UNKNOWN       = The position of the first key is unknown or too
#                     complex to describe (example XREAD)
#     INDEX         = The first key is the argument at index i
#     KEYNUM        = The argument at index i specifies the number of keys
#                     and the first key is the next argument, unless the
#                     number of keys is zero in which case there are no
#                     keys (example EVAL)
def firstkey(props):
    if not "key_specs" in props:
        # Key specs missing. Best-effort fallback to "arguments".
        if "arguments" in props:
            args = props["arguments"]
            for i in range(1, len(args)):
                arg = args[i - 1]
                if arg.get("type") == "key":
                    return ("INDEX", i)
                elif arg.get("type") == "string" and arg.get("name") == "key":
                    # add-hoc case for RediSearch
                    return ("INDEX", i)
                elif arg.get("optional") or arg.get("multiple") or "arguments" in arg:
                    # Too complex for this fallback.
                    if any_argument_is_key(args):
                        return ("UNKNOWN", 0)
                    else:
                        return ("NONE", 0)
        return ("NONE", 0)

    if len(props["key_specs"]) == 0:
        return ("NONE", 0)

    # We detect the first key spec and only if the begin_search is by index.
    # Otherwise we return -1 for unknown (for example if the first key is
    # indicated by a keyword like KEYS or STREAMS).
    begin_search = props["key_specs"][0]["begin_search"]
    if "index" in begin_search:
        # Valkey source JSON files have this syntax
        pos = begin_search["index"]["pos"]
    elif begin_search.get("type") == "index" and "spec" in begin_search:
        # generate-commands-json.py returns this syntax
        pos = begin_search["spec"]["index"]
    else:
        return ("UNKNOWN", 0)

    find_keys = props["key_specs"][0]["find_keys"]
    if "range" in find_keys or find_keys.get("type") == "range":
        # The first key is the arg at index pos.
        # Valkey source JSON files have this syntax:
        #     "find_keys": {
        #         "range": {...}
        #     }
        # generate-commands-json.py returns this syntax:
        #     "find_keys": {
        #         "type": "range",
        #         "spec": {...}
        #     },
        return ("INDEX", pos)
    elif "keynum" in find_keys:
        # The arg at pos is the number of keys and the next arg is the first key
        # Valkey source JSON files have this syntax
        assert find_keys["keynum"]["keynumidx"] == 0
        assert find_keys["keynum"]["firstkey"] == 1
        return ("KEYNUM", pos)
    elif find_keys.get("type") == "keynum":
        # generate-commands-json.py returns this syntax
        assert find_keys["spec"]["keynumidx"] == 0
        assert find_keys["spec"]["firstkey"] == 1
        return ("KEYNUM", pos)
    else:
        return ("UNKNOWN", 0)

def extract_command_info(name, props):
    (firstkeymethod, firstkeypos) = firstkey(props)
    container = props.get("container", "")
    name = name.upper()
    subcommand = None
    if container != "":
        subcommand = name
        name = container.upper()
    else:
        # Ad-hoc handling of command and subcommand in the same string,
        # sepatated by a space. This form is used in e.g. RediSearch's JSON file
        # in commands like "FT.CONFIG GET".
        tokens = name.split(maxsplit=1)
        if len(tokens) > 1:
            name, subcommand = tokens
            if firstkeypos > 0 and not "key_specs" in props:
                # Position was inferred from "arguments"
                firstkeypos += 1

    arity = props["arity"] if "arity" in props else -1
    return (name, subcommand, arity, firstkeymethod, firstkeypos);

# Parses a file with lines like
# COMMAND(identifier, cmd, subcmd, arity, firstkeymethod, firstkeypos)
def collect_command_from_cmddef_h(f, commands):
   for line in f:
       m = re.match(r'^COMMAND\(\S+, *"(\S+)", NULL, *(-?\d+), *(\w+), *(\d+)\)', line)
       if m:
           commands[m.group(1)] = (m.group(1), None, int(m.group(2)), m.group(3), int(m.group(4)))
           continue
       m = re.match(r'^COMMAND\(\S+, *"(\S+)", *"(\S+)", *(-?\d+), *(\w+), *(\d)\)', line)
       if m:
           key = m.group(1) + "_" + m.group(2)
           commands[key] = (m.group(1), m.group(2), int(m.group(3)), m.group(4), int(m.group(5)))
           continue
       if re.match(r'^(?:/\*.*\*/)?\s*$', line):
           # Comment or blank line
           continue
       else:
           print("Error processing line: %s" % (line))
           exit(1)

def collect_commands_from_files(filenames):
    # The keys in the dicts are "command" or "command_subcommand".
    commands = dict()
    commands_that_have_subcommands = set()
    for filename in filenames:
        with open(filename, "r") as f:
            if filename.endswith(".h"):
                collect_command_from_cmddef_h(f, commands)
                continue
            try:
                d = json.load(f)
                for name, props in d.items():
                    cmd = extract_command_info(name, props)
                    (name, subcmd, _, _, _) = cmd

                    # For commands with subcommands, we want only the
                    # command-subcommand pairs, not the container command alone
                    if subcmd is not None:
                        commands_that_have_subcommands.add(name)
                        if name in commands:
                            del commands[name]
                        name += "_" + subcmd
                    elif name in commands_that_have_subcommands:
                        continue

                    commands[name] = cmd

            except json.decoder.JSONDecodeError as err:
                print("Error processing %s: %s" % (filename, err))
                exit(1)
    return commands

def generate_c_code(commands):
    print("/* This file was generated using gencommands.py */")
    print("")
    print("/* clang-format off */")
    for key in sorted(commands):
        (name, subcmd, arity, firstkeymethod, firstkeypos) = commands[key]
        # Make valid C identifier (macro name)
        key = re.sub(r'\W', '_', key)
        if subcmd is None:
            print("COMMAND(%s, \"%s\", NULL, %d, %s, %d)" %
                  (key, name, arity, firstkeymethod, firstkeypos))
        else:
            print("COMMAND(%s, \"%s\", \"%s\", %d, %s, %d)" %
                  (key, name, subcmd, arity, firstkeymethod, firstkeypos))

# MAIN

if len(sys.argv) < 2 or sys.argv[1] == "--help":
    print("Usage: %s path/to/valkey/src/commands/*.json > cmddef.h" % sys.argv[0])
    exit(1)

# Find all JSON files
filenames = []
for filename in sys.argv[1:]:
    if os.path.isdir(filename):
        # A valkey repo root dir (accepted for backward compatibility)
        jsondir = os.path.join(filename, "src", "commands")
        if not os.path.isdir(jsondir):
            print("The directory %s is not a Valkey source directory." % filename)
            exit(1)

        filenames += glob.glob(os.path.join(jsondir, "*.json"))
    else:
        filenames.append(filename)

# Collect all command info
commands = collect_commands_from_files(filenames)

# Print C code
generate_c_code(commands)
