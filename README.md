This README is just a fast *quick start* document. You can find more detailed documentation at [valkey.io](https://valkey.io/)

What is Valkey?
---------------

Valkey is an open source fork of Redis OSS, created at the point where Redis switched to a proprietary license.

Valkey is often referred to as a *data structures* server. What this means is that Valkey provides access to mutable data structures via a set of commands, which are sent using a *server-client* model with TCP sockets and a simple protocol. So different processes can query and modify the same data structures in a shared way.

Data structures implemented into Valkey have a few special properties:

* Valkey cares to store them on disk, even if they are always served and modified into the server memory. This means that Valkey is fast, but that it is also non-volatile.
* The implementation of data structures emphasizes memory efficiency, so data structures inside Valkey will likely use less memory compared to the same data structure modelled using a high-level programming language.
* Valkey offers a number of features that are natural to find in a database, like replication, tunable levels of durability, clustering, and high availability.

Building Valkey
---------------

Valkey can be compiled and used on Linux, OSX, OpenBSD, NetBSD, FreeBSD.
We support big endian and little endian architectures, and both 32 bit
and 64 bit systems.

It may compile on Solaris derived systems (for instance SmartOS) but our
support for this platform is *best effort* and Valkey is not guaranteed to
work as well as in Linux, OSX, and \*BSD.

It is as simple as:

    % make

To build with TLS support, you'll need OpenSSL development libraries (e.g.
libssl-dev on Debian/Ubuntu) and run:

    % make BUILD_TLS=yes

To build with systemd support, you'll need systemd development libraries (such 
as libsystemd-dev on Debian/Ubuntu or systemd-devel on CentOS) and run:

    % make USE_SYSTEMD=yes

To append a suffix to Valkey program names, use:

    % make PROG_SUFFIX="-alt"

You can build a 32 bit Valkey binary using:

    % make 32bit

After building, it is a good idea to test it using:

    % make test

If TLS is built, running the tests with TLS enabled (you will need `tcl-tls`
installed):

    % ./utils/gen-test-certs.sh
    % ./runtest --tls


Fixing build problems with dependencies or cached build options
---------

Valkey has some dependencies which are included in the `deps` directory.
`make` does not automatically rebuild dependencies even if something in
the source code of dependencies changes.

When you update the source code with `git pull` or when code inside the
dependencies tree is modified in any other way, make sure to use the following
command in order to really clean everything and rebuild from scratch:

    % make distclean

This will clean: jemalloc, lua, hiredis, linenoise and other dependencies.

Also if you force certain build options like 32bit target, no C compiler
optimizations (for debugging purposes), and other similar build time options,
those options are cached indefinitely until you issue a `make distclean`
command.

Fixing problems building 32 bit binaries
---------

If after building Valkey with a 32 bit target you need to rebuild it
with a 64 bit target, or the other way around, you need to perform a
`make distclean` in the root directory of the Valkey distribution.

In case of build errors when trying to build a 32 bit binary of Valkey, try
the following steps:

* Install the package libc6-dev-i386 (also try g++-multilib).
* Try using the following command line instead of `make 32bit`:
  `make CFLAGS="-m32 -march=native" LDFLAGS="-m32"`

Allocator
---------

Selecting a non-default memory allocator when building Valkey is done by setting
the `MALLOC` environment variable. Valkey is compiled and linked against libc
malloc by default, with the exception of jemalloc being the default on Linux
systems. This default was picked because jemalloc has proven to have fewer
fragmentation problems than libc malloc.

To force compiling against libc malloc, use:

    % make MALLOC=libc

To compile against jemalloc on Mac OS X systems, use:

    % make MALLOC=jemalloc

Monotonic clock
---------------

By default, Valkey will build using the POSIX clock_gettime function as the
monotonic clock source.  On most modern systems, the internal processor clock
can be used to improve performance.  Cautions can be found here: 
    http://oliveryang.net/2015/09/pitfalls-of-TSC-usage/

To build with support for the processor's internal instruction clock, use:

    % make CFLAGS="-DUSE_PROCESSOR_CLOCK"

Verbose build
-------------

Valkey will build with a user-friendly colorized output by default.
If you want to see a more verbose output, use the following:

    % make V=1

Running Valkey
-------------

To run Valkey with the default configuration, just type:

    % cd src
    % ./valkey-server

If you want to provide your valkey.conf, you have to run it using an additional
parameter (the path of the configuration file):

    % cd src
    % ./valkey-server /path/to/valkey.conf

It is possible to alter the Valkey configuration by passing parameters directly
as options using the command line. Examples:

    % ./valkey-server --port 9999 --replicaof 127.0.0.1 6379
    % ./valkey-server /etc/valkey/6379.conf --loglevel debug

All the options in valkey.conf are also supported as options using the command
line, with exactly the same name.

Running Valkey with TLS:
------------------

Please consult the [TLS.md](TLS.md) file for more information on
how to use Valkey with TLS.

Playing with Valkey
------------------

You can use valkey-cli to play with Valkey. Start a valkey-server instance,
then in another terminal try the following:

    % cd src
    % ./valkey-cli
    127.0.0.1:6379> ping
    PONG
    127.0.0.1:6379> set foo bar
    OK
    127.0.0.1:6379> get foo
    "bar"
    127.0.0.1:6379> incr mycounter
    (integer) 1
    127.0.0.1:6379> incr mycounter
    (integer) 2
    127.0.0.1:6379>

You can find the list of all the available commands at https://valkey.io/commands.

Installing Valkey
-----------------

In order to install Valkey binaries into /usr/local/bin, just use:

    % make install

You can use `make PREFIX=/some/other/directory install` if you wish to use a
different destination.

_Note_: For compatibility with Valkey, we create symlinks from the Redis names (`redis-server`, `redis-cli`, etc.) to the Valkey binaries installed by `make install`.
The symlinks are created in same directory as the Valkey binaries.
The symlinks are removed when using `make uninstall`.
The creation of the symlinks can be skipped by setting the makefile variable `USE_REDIS_SYMLINKS=no`.

`make install` will just install binaries in your system, but will not configure
init scripts and configuration files in the appropriate place. This is not
needed if you just want to play a bit with Valkey, but if you are installing
it the proper way for a production system, we have a script that does this
for Ubuntu and Debian systems:

    % cd utils
    % ./install_server.sh

_Note_: `install_server.sh` will not work on Mac OSX; it is built for Linux only.

The script will ask you a few questions and will setup everything you need
to run Valkey properly as a background daemon that will start again on
system reboots.

You'll be able to stop and start Valkey using the script named
`/etc/init.d/valkey_<portnumber>`, for instance `/etc/init.d/valkey_6379`.

Code contributions
-----------------

Note: By contributing code to the Valkey project in any form, including sending
a pull request via Github, a code fragment or patch via private email or
public discussion groups, you agree to release your code under the terms
of the BSD license that you can find in the [COPYING][1] file included in the Valkey
source distribution.

Please see the [CONTRIBUTING.md][2] file in this source distribution for more
information. For security bugs and vulnerabilities, please see [SECURITY.md][3].

[1]: https://github.com/valkey-io/valkey/blob/unstable/COPYING
[2]: https://github.com/valkey-io/valkey/blob/unstable/CONTRIBUTING.md
[3]: https://github.com/valkey-io/valkey/blob/unstable/SECURITY.md

Enjoy!
