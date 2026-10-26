# Lua 5.1 Scripting Module

This module provides a Lua 5.1 scripting engine for Valkey, enabling server-side script execution with atomic operations and enhanced performance.

## Overview

The Lua module enables execution of Lua 5.1 scripts within Valkey using the `EVAL` and `FCALL` commands. This allows for:

- Atomic execution of complex operations
- Reduced network round trips
- Server-side data processing and transformation
- Custom command implementations

Scripts are executed in a sandboxed environment with access to Valkey's data structures and a subset of Lua's standard library.

For comprehensive information about using Lua scripts in Valkey, please refer to the **Extending Valkey** section of the official [documentation site](https://valkey.io/topics/).

## Building

This module is automatically built as part of the Valkey core build process. However, you can also build it independently for development or testing purposes:

```bash
cd src/modules/lua
make
```

The independent build will create the module binary that can be loaded into Valkey using the `MODULE LOAD` command.

## Dependencies

This Lua module is designed to be mostly self-contained with minimal external dependencies.

Currently, the module has only two dependencies on Valkey core files:
- `src/sha1.c` - for SHA1 hash computation
- `src/random.c` - for random number generation

These dependencies were chosen to leverage existing Valkey functionality while maintaining modularity. Should the need arise to relocate this Lua module to a separate repository, these files can be easily copied alongside the module without introducing additional transitive dependencies.

When adding new dependencies in the future, please ensure they are lightweight and self-contained to preserve the module's portability and minimize complexity.

