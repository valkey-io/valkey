# Valkey development guidelines
This document provides an general overview for writing and designing code for Valkey.
During our long development history, we've made a lot of inconsistent decisions, but we strive to get incrementally better.

## General style guidelines
Most of the style guidelines are enforced by clang format, but some additional comments are included here.

1. C style comments `/* comment */` can be used for both single and multi-line comments. C++ comments `//` can only be used for single line comments.
1. Generally keep line lengths below 90 characters, however there is no explicit line length enforcement.
1. Use static functions when a function is only intended to be accessed from the same file.

## Naming conventions
Valkey has a long history of inconsistent naming conventions. Generally follow the style of the surrounding code, but you can also always use the following conventions for variable and structure names:

- Variable names: `snake_case` or all lower case (e.g. `valkeyobject` or `valkey_object`)
- Function names: `camelCase` or `namespace_camelCase` (e.g. `createObjectList` or `networking_createObjectList`).
- Macros: `UPPER_CASE` (e.g. `DICT_CREATE`)
- Structures: `camelCase` (e.g. `user`)

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
There isn't a well defined test for what is considered a material change, but a good rule of thumb is if it's more than 100 lines of code.

## Test coverage
Valkey uses two types of tests: unit and integration tests.
All contributions should include a test of some form. 

Unit tests are present in the `src/unit` directory, and are intended to test individual structures or files.
For example, most changes to data structures should include corresponding unit tests.

Integration tests are located in the `tests/` directory, and are intended to test end-to-end functionality.
Adding new commands should come with corresponding integration tests.
When writing cluster mode tests, do not use the legacy `tests/cluster` framework, which has been deprecated.

## Best practices
1. Avoid adding configuration when a feature can be fully controlled by heuristics. 
We want Valkey to work correctly out of the box without much tuning.
Configurations can be added to provide additional tuning of features. 
When the workload characteristics can't be inferred or imply a tradeoff (CPU vs memory), then provide a configuration.
2. Try to limit the number of lines changed in a PR when possible.
We do a lot of backporting as a project, and the more lines changed, the higher the chance of having to resolve merge conflicts.