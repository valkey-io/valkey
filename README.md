This README is under construction as we work to build a new community driven high performance key-value store.

This project was forked from the open source Redis project right before the transition to their new source available licenses.

This README is just a fast *quick start* document. We are currently working on a more permanent documentation page.

What is PlaceHolderKV?
--------------
The name is temporary, as we work to find a new name that the community accepts. It also happens to be a very easy strong to search for. 

Building PlaceHolderKV
--------------

PlaceHolderKV can be compiled and used on Linux, OSX, OpenBSD, NetBSD, FreeBSD.
We support big endian and little endian architectures, and both 32 bit
and 64 bit systems.

It may compile on Solaris derived systems (for instance SmartOS) but our
support for this platform is *best effort* and PlaceHolderKV is not guaranteed to
work as well as in Linux, OSX, and \*BSD.

It is as simple as:

    % make

To build with TLS support, you'll need OpenSSL development libraries (e.g.
libssl-dev on Debian/Ubuntu) and run:

    % make BUILD_TLS=yes

To build with systemd support, you'll need systemd development libraries (such 
as libsystemd-dev on Debian/Ubuntu or systemd-devel on CentOS) and run:

    % make USE_SYSTEMD=yes

To append a suffix to PlaceHolderKV program names, use:

    % make PROG_SUFFIX="-alt"

You can build a 32 bit PlaceHolderKV binary using:

    % make 32bit

After building PlaceHolderKV, it is a good idea to test it using:

    % make test

If TLS is built, running the tests with TLS enabled (you will need `tcl-tls`
installed):

    % ./utils/gen-test-certs.sh
    % ./runtest --tls


Fixing build problems with dependencies or cached build options
---------

PlaceHolderKV has some dependencies which are included in the `deps` directory.
`make` does not automatically rebuild dependencies even if something in
the source code of dependencies changes.

When you update the source code with `git pull` or when code inside the
dependencies tree is modified in any other way, make sure to use the following
command in order to really clean everything and rebuild from scratch:

    % make distclean

This will clean: jemalloc, lua, hiplaceholderkv, linenoise and other dependencies.

Also if you force certain build options like 32bit target, no C compiler
optimizations (for debugging purposes), and other similar build time options,
those options are cached indefinitely until you issue a `make distclean`
command.

Fixing problems building 32 bit binaries
---------

If after building PlaceHolderKV with a 32 bit target you need to rebuild it
with a 64 bit target, or the other way around, you need to perform a
`make distclean` in the root directory of the PlaceHolderKV distribution.

In case of build errors when trying to build a 32 bit binary of PlaceHolderKV, try
the following steps:

* Install the package libc6-dev-i386 (also try g++-multilib).
* Try using the following command line instead of `make 32bit`:
  `make CFLAGS="-m32 -march=native" LDFLAGS="-m32"`

Allocator
---------

Selecting a non-default memory allocator when building PlaceHolderKV is done by setting
the `MALLOC` environment variable. PlaceHolderKV is compiled and linked against libc
malloc by default, with the exception of jemalloc being the default on Linux
systems. This default was picked because jemalloc has proven to have fewer
fragmentation problems than libc malloc.

To force compiling against libc malloc, use:

    % make MALLOC=libc

To compile against jemalloc on Mac OS X systems, use:

    % make MALLOC=jemalloc

Monotonic clock
---------------

By default, PlaceHolderKV will build using the POSIX clock_gettime function as the
monotonic clock source.  On most modern systems, the internal processor clock
can be used to improve performance.  Cautions can be found here: 
    http://oliveryang.net/2015/09/pitfalls-of-TSC-usage/

To build with support for the processor's internal instruction clock, use:

    % make CFLAGS="-DUSE_PROCESSOR_CLOCK"

Verbose build
-------------

PlaceHolderKV will build with a user-friendly colorized output by default.
If you want to see a more verbose output, use the following:

    % make V=1

Running PlaceHolderKV
-------------

To run PlaceHolderKV with the default configuration, just type:

    % cd src
    % ./placeholderkv-server

If you want to provide your placeholderkv.conf, you have to run it using an additional
parameter (the path of the configuration file):

    % cd src
    % ./placeholderkv-server /path/to/placeholderkv.conf

It is possible to alter the PlaceHolderKV configuration by passing parameters directly
as options using the command line. Examples:

    % ./placeholderkv-server --port 9999 --replicaof 127.0.0.1 6379
    % ./placeholderkv-server /etc/placeholderkv/6379.conf --loglevel debug

All the options in placeholderkv.conf are also supported as options using the command
line, with exactly the same name.

Running PlaceHolderKV with TLS:
------------------

Please consult the [TLS.md](TLS.md) file for more information on
how to use PlaceHolderKV with TLS.

Playing with PlaceHolderKV
------------------

You can use placeholderkv-cli to play with PlaceHolderKV. Start a placeholderkv-server instance,
then in another terminal try the following:

    % cd src
    % ./placeholderkv-cli
    placeholderkv> ping
    PONG
    placeholderkv> set foo bar
    OK
    placeholderkv> get foo
    "bar"
    placeholderkv> incr mycounter
    (integer) 1
    placeholderkv> incr mycounter
    (integer) 2
    placeholderkv>

Installing PlaceHolderKV
-----------------

In order to install PlaceHolderKV binaries into /usr/local/bin, just use:

    % make install

You can use `make PREFIX=/some/other/directory install` if you wish to use a
different destination.

`make install` will just install binaries in your system, but will not configure
init scripts and configuration files in the appropriate place. This is not
needed if you just want to play a bit with PlaceHolderKV, but if you are installing
it the proper way for a production system, we have a script that does this
for Ubuntu and Debian systems:

    % cd utils
    % ./install_server.sh

_Note_: `install_server.sh` will not work on Mac OSX; it is built for Linux only.

The script will ask you a few questions and will setup everything you need
to run PlaceHolderKV properly as a background daemon that will start again on
system reboots.

You'll be able to stop and start PlaceHolderKV using the script named
`/etc/init.d/placeholderkv_<portnumber>`, for instance `/etc/init.d/placeholderkv_6379`.

Code contributions
-----------------

Note: By contributing code to the PlaceHolderKV project in any form, including sending
a pull request via Github, a code fragment or patch via private email or
public discussion groups, you agree to release your code under the terms
of the BSD license that you can find in the [COPYING][1] file included in the PlaceHolderKV
source distribution.

Please see the [CONTRIBUTING.md][2] file in this source distribution for more
information. For security bugs and vulnerabilities, please see [SECURITY.md][3].

[1]: https://github.com/madolson/placeholderkv/blob/unstable/COPYING
[2]: https://github.com/madolson/placeholderkv/blob/unstable/CONTRIBUTING.md
[3]: https://github.com/madolson/placeholderkv/blob/unstable/SECURITY.md
