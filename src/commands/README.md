Command JSON files
==================

This directory contains JSON files, one for each command.

Each JSON file contains all the information about the command itself. It is the
"single source of truth" (SSOT) for the command's metadata.

These JSON files were originally not intended to be used directly, since they
contain internals and some fields like "acl_categories" that are not the final
ACL categories. (Valkey will apply some implicit rules to compute the final ACL
categories.) However, people see JSON files and use them directly anyway.

Any third party who needs access to command information were originally supposed
to get it from `COMMAND INFO` and `COMMAND DOCS`. These commands can be combined
into a JSON file by the script `utils/generate-commands-json.py`. Confusingly
enough, this JSON file as a slightly different format!

Structure
---------

Each JSON file contains an object with a single key. The key is the command name
in uppercase, e.g. "HSCAN" (hscan.json). The value is a JSON object with the
following keys. To be safe, assume all of them are optional.

* `"summary"`: a string with a short description of the command. One sentence.
* `"complexity"`: a string like `"O(1)"` or longer, like `"O(1) for every call. O(N) for a complete iteration, including enough command calls for the cursor to return back to 0. N is the number of elements inside the collection."`.
* `"group"`: a string used for categorization in documentation. One of these:
  * `"bitmap"`
  * `"cluster"`
  * `"connection"`
  * `"generic"`
  * `"geo"`
  * `"hash"`
  * `"hyperloglog"`
  * `"list"`
  * `"pubsub"`
  * `"scripting"`
  * `"sentinel"`
  * `"server"`
  * `"set"`
  * `"sorted_set"`
  * `"stream"`
  * `"string"`
  * `"transactions"`
* `"since"`: a string with a version number, like "7.0.0". It's the version
  (Redis OSS or Valkey) where the command was introduced.
* `"arity"`: The number of arguments, including the command name itself. A
  negative number means "at least", e.g. -3 means at least 3.
* `"container"`: Only present for subcommands. See below.
* `"history"`: An array of changes, each change represented by a 2-element array
  on the form `[VERSION, DESCRIPTION]`. Omit if empty. Don't add an empty array.
* `"function"`: The name of the C function in Valkey's source code implementing
  the command. (Don't use it for anything else.)
* `"command_flags"`: An array of flags represented as strings. Command flags:
  * `"ADMIN"`
  * `"ALLOW_BUSY"`
  * `"ASKING"`
  * `"BLOCKING"`
  * `"DENYOOM"`
  * `"FAST"`
  * `"LOADING"`
  * `"MAY_REPLICATE"`
  * `"NO_ASYNC_LOADING"`
  * `"NO_AUTH"`
  * `"NO_MANDATORY_KEYS"`
  * `"NO_MULTI"`
  * `"NOSCRIPT"`
  * `"ONLY_SENTINEL"`
  * `"PROTECTED"`
  * `"PUBSUB"`
  * `"READONLY"`
  * `"SENTINEL"`
  * `"SKIP_MONITOR"`
  * `"SKIP_SLOWLOG"`
  * `"STALE"`
  * `"TOUCHES_ARBITRARY_KEYS"`
  * `"WRITE"`
* `"acl_categories"`: A list of ACL categories in uppercase. Note that the
  effective ACL categies include "implicit ACL categories" explained below.
  * `"ADMIN"`
  * `"BITMAP"`
  * `"CONNECTION"`
  * `"DANGEROUS"`
  * `"GEO"`
  * `"HASH"`
  * `"HYPERLOGLOG"`
  * `"KEYSPACE"`
  * `"LIST"`
  * `"SCRIPTING"`
  * `"SET"`
  * `"SORTEDSET"`
  * `"STREAM"`
  * `"STRING"`
  * `"TRANSACTION"`
* `"command_tips"`: Optional. A list of one or more of these strings:
  * `"NONDETERMINISTIC_OUTPUT"`
  * `"NONDETERMINISTIC_OUTPUT_ORDER"`
  * `"REQUEST_POLICY:ALL_NODES"`
  * `"REQUEST_POLICY:ALL_SHARDS"`
  * `"REQUEST_POLICY:MULTI_SHARD"`
  * `"REQUEST_POLICY:SPECIAL"`
  * `"RESPONSE_POLICY:AGG_LOGICAL_AND"`
  * `"RESPONSE_POLICY:AGG_MIN"`
  * `"RESPONSE_POLICY:AGG_SUM"`
  * `"RESPONSE_POLICY:ALL_SUCCEEDED"`
  * `"RESPONSE_POLICY:ONE_SUCCEEDED"`
  * `"RESPONSE_POLICY:SPECIAL"`
* `"key_specs"`: An array of key specifications. See below.
* `"reply_schema"`: A [JSON Schema](https://json-schema.org/) that describes the
  reply of the command. This isn't complete. For example, JSON Schema can't
  distinguish arrays from sets, commands returning a set are declared to return
  an array.
* `"arguments"`: An array of arguments. Each argument is an object with the following keys:
  * `"name"`: A string identifying the argument. It's unique among the arguments.
  * `"type"`: The type of the argument.
    * `"block"`: A group of arguments. The elements are in the key `"arguments"`.
    * `"double"`: A number, not necessarily an integer.
    * `"integer"`: An integer.
    * `"key"`: A string representing a key in the database.
    * `"oneof"`: One of a list of alternatives. The alternatives are in the key `"arguments"`.
    * `"pattern"`: A string representing a glob-style pattern.
    * `"pure-token"`: A fixed string. The string is in the key `"token"`.
    * `"string"`: A string.
    * `"unix-time"`: An integer representing a unix time in either seconds or milliseconds.
  * `"arguments"`: A list with the same structure as its parent. Present if type is "block" or "oneof".
  * `"display"`: ("entries-read", "key" or "pattern")
  * `"key_spec_index"`: An index into the `"key_specs"` array. Only if `"type"` is `"key"`.
  * `"multiple":` true if the argument can be repeated multiple times. Omitted means false.
  * `"multiple_token"`: Unclear meaning. Maybe meaningless.
  * `"optional":` True if the argument is optional. Omitted means false.
  * `"since"`: Version (string) when the argument was introduced.
  * `"token"`: A string indicating a fixed string value. This is always present
    if type is "pure-token". If type is anything else, then `"token"` indicates
    the argument is preceded by an extra (fixed string) argument.

Implicit ACL categories
-----------------------

The ACL categories specified as `"acl_categories"` are not the ones actually used.
The effective ACL categories are affected also by command flags.

The logic for this can be found in the function `setImplicitACLCategories()` in
`server.c`. Here are the rules (unless they have changed since this
documentation was written):

* Command flag WRITE implies ACL category WRITE.
* Command flag READONLY and not ACL category SCRIPTING implies ACL category READ.
  "Exclude scripting commands from the RO category."
* Command flag ADMIN implies ACL categories ADMIN and DANGEROUS.
* Command flag PUBSUB implies ACL category PUBSUB.
* Command flag FAST implies ACL category FAST.
* Command flag BLOCKING implies ACL category BLOCKING.
* Not ACL category FAST implies ACL category SLOW. "If it's not fast, it's slow."

There's an issue about explicitly listing all categories, removing this
discrepancy: https://github.com/valkey-io/valkey/issues/417

Key specs
---------

Key specifications are specified in the array `"key_specs"` key of a command.

Each element in this array is an object with the following keys:

* `"flags"`: An array of strings indicating what kind of access is the command does on the key.
  * `"ACCESS"`
  * `"DELETE"`
  * `"INCOMPLETE"`
  * `"INSERT"`
  * `"NOT_KEY"`
  * `"OW"`
  * `"RM"`
  * `"RO"`
  * `"RW"`
  * `"UPDATE"`
  * `"VARIABLE_FLAGS"`
* `"begin_search"`: How to find the first key used by this key spec. It's an
  object with only one key. The key determines the method for finding the first
  key. Here are the possible forms of the `"begin_search"` object:
  * `{"index": {"pos": N}}`: The first key is at position N in the command line,
    where 0 is the command name.
  * `{"keyword": KEYWORD, "startfrom": N}`: The first key is found by searching
    for an argument with the exact value KEYWORD starting from index N in the
    command line. The first key is the argument after the keyword.
  * `{"unknown": null}`: Finding the keys of this command is too complicated to
    explain.
* `"find_keys"`: How to find the remainnig keys of this key spec. It's an object
  on one of these forms:
  * `{"range": {"lastkey": LAST, "step": STEP, "limit": LIMIT}}`: A range of keys.
    * LAST: If LAST is positive, it's the index of the last key relative to the
      first key. If last is negative, then -1 is the end of the whole command
      line, -2 is the penultimate argument of the command, and so on.
    * STEP: The number of arguments to skip to find the next one. Typically 1.
    * LIMIT: If LAST is -1, we use the limit to stop the search by a factor. 0
      and 1 mean no limit. 2 means half of the remaining arguments, 3 means a
      third, and so on.

Commands with subcommands
-------------------------

Commands with subcommands are special. Examples of commands with subcommands are
`CLUSTER` and `ACL`. Their first argument is a subcommand which determines the
syntax of the rest the command, which is stored in its own JSON file.

For example `CLUSTER INFO` is stored in a file called `cluster-info.json`. The
toplevel key is called `"INFO"`. Within the body, there's a key called
`"container"` with the value `"CLUSTER"`. The file `cluster.json` exists by it
doesn't have an `"arguments"` key.

Appendix
--------

How to list all the `group`, `command_flags` and `acl_categries`, etc. used in all these files:

    cat *.json | jq '.[].group'             | grep -F '"' | sed 's/^ *//;s/, *$//;s/^/  * `/;s/$/`/' | sort | uniq
    cat *.json | jq '.[].command_flags'     | grep -F '"' | sed 's/^ *//;s/, *$//;s/^/  * `/;s/$/`/' | sort | uniq
    cat *.json | jq '.[].acl_categories'    | grep -F '"' | sed 's/^ *//;s/, *$//;s/^/  * `/;s/$/`/' | sort | uniq
    cat *.json | jq '.[].arguments[]?.type' | grep -F '"' | sed 's/^ *//;s/, *$//;s/^/  * `/;s/$/`/' | sort | uniq
