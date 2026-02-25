# Valkey development guidelines
This document provides a general overview for writing and designing code for Valkey.
During our long development history, we've made a lot of inconsistent decisions, but we strive to get incrementally better.

## General Best practices
1. Try to limit the number of lines changed in a PR when possible.
   We do a lot of backporting as a project, and the more lines changed, the higher the chance of having to resolve merge conflicts.
   Please separate refactoring and functional changes into separate PRs, to make it easier to handle backporting.
1. Avoid adding configuration when a feature can be fully controlled by heuristics.
   We want Valkey to work correctly out of the box without much tuning.
   Configurations can be added to provide additional tuning of features.
   When the workload characteristics can't be inferred or imply a tradeoff (CPU vs memory), then provide a configuration.

## General style guidelines
Most of the style guidelines are enforced by clang format, but some additional comments are included here.

1. C style comments `/* comment */` can be used for both single and multi-line comments.
   C++ comments `//` can only be used for single line comments.
   Multi line comments should have the leading `*` align and the final `*/` should be on the same line as the last line of text.
   e.g.
```c
/* Blah Blah
 * Blah Blah. */
   ```
2. Comments should generally be used to describe behavior that is not obvious from reading the code itself.
   This includes complex behavior, why code was written the way it was and describing non-obvious behavior.
   Additionally, functions should be documented to explain all of the function's behavior without having to read the code.
1. Generally keep line lengths below 90 characters when reasonable, however there is no explicit line length enforcement.
   Use your best judgement for readability.
1. Use static functions when a function is only intended to be accessed from the same file.
   For historical reasons, some private functions are prefixed by `_`, and they are kept as is to make it easier to backport changes.
1. Use the boolean type for true/false values.
   For historical reasons, some functions used the integer type, and they are kept as is to make it easier to backport changes.

## Naming conventions
Valkey has a long history of inconsistent naming conventions.
Generally follow the style of the surrounding code, but you can also always use the following conventions for variable and structure names:

- Variable names: `snake_case` or all lower case for short names  (e.g. `cached_reply` or `keylen`).
- Function names: `camelCase` or `namespace_camelCase` (e.g. `createStringObject` or `IOJobQueue_isFull`).
- Macros: `UPPER_CASE` (e.g. `MAKE_CMD`).
- Structures: `camelCase` (e.g. `user`).

## Licensing information
When creating new source code files, use the following snippet to indicate the license:
```
/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
```

If you are making material changes to a file that has a different license at the top, also add the above license snippet.
There isn't a well defined test for what is considered a material change, but a good rule of thumb is that material changes are more than 100 lines of code.

## Test coverage
Valkey uses two types of tests: unit and integration tests.
All contributions should include a test of some form.

Unit tests are present in the `src/unit` directory, and are intended to test individual structures or files.
For example, most changes to data structures should include corresponding unit tests.

Integration tests are located in the `tests/` directory, and are intended to test end-to-end functionality.
Adding new commands should come with corresponding integration tests.
When writing cluster mode tests, do not use the legacy `tests/cluster` framework, which has been deprecated, and instead write tests in `unit/cluster`.

## Documentation
Valkey keeps most of the user documentation in the [valkey-doc](https://github.com/valkey-io/valkey-doc) repository in a few areas:
1. Major functionality is documented in the [topics](https://github.com/valkey-io/valkey-doc/tree/main/topics) section.
1. Specific command behavior is documented in the [commands](https://github.com/valkey-io/valkey-doc/tree/main/commands) section.
   Command history is also documented in the [command json file](https://github.com/valkey-io/valkey/tree/unstable/src/commands).
1. Server info fields are documented in the [INFO](https://github.com/valkey-io/valkey-doc/blob/main/commands/info.md) command.

When a PR is opened that requires documentation to be updated, the `needs-doc-pr` should be added until the corresponding documentation PR is open.
