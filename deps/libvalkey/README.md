# Libvalkey

Libvalkey is the official C client for the [Valkey](https://valkey.io) database. It also supports any server that uses the `RESP` protocol (version 2 or 3). This project supports both standalone and cluster modes.

## Table of Contents

- [Features](#features)
- [Supported platforms](#supported-platforms)
- [Building](#building)
  - [Building with make](#building-with-make)
  - [Building with CMake](#building-with-cmake)
- [Contributing](#contributing)
- Using the library
  - [Standalone mode](docs/standalone.md)
  - [Cluster mode](docs/cluster.md)
  - [Migration guide](docs/migration-guide.md)

## Features

- Commands are executed in a generic way, with printf-like invocation.
- Supports both `RESP2` and `RESP3` protocol versions.
- Supports both synchronous and asynchronous operation.
- Optional support for `MPTCP`, `TLS` and `RDMA` connections.
- Asynchronous API with several event libraries to choose from.
- Supports both standalone and cluster mode operation.
- Can be compiled with either `make` or `CMake`.

## Supported platforms

This library supports and is tested against `Linux`, `FreeBSD`, `macOS`, and `Windows`. It should build and run on various proprietary Unixes although we can't run CI for those platforms. If you encounter any issues, please open an issue.

## Building

Libvalkey is written in C targeting C99. Unfortunately we have no plans on supporting C89 or earlier. The project does use a few widely supported compiler extensions, specifically for the bundled `sds` string library, although we have plans to remove this from the library.

We support plain GNU make and CMake. Following is information on how to build the library.

### Building with make

```bash
# Build and install the default library
sudo make install

# With all options
sudo USE_TLS=1 USE_RDMA=1 make install

# If your openssl is in a non-default location
sudo USE_TLS=1 OPENSSL_PREFIX=/path/to/openssl make install
```

### Building with CMake

See [CMakeLists.txt](CMakeLists.txt) for all available options.

```bash
# Build and install
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo ..
sudo make install

# Build with TLS and RDMA support
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_TLS=1 -DENABLE_RDMA=1 ..
sudo make install
```

## Contributing

Please see [`CONTRIBUTING.md`](https://github.com/valkey-io/libvalkey/blob/main/CONTRIBUTING.md).
