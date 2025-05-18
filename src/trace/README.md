## Introduction

This directory contains the implementation of tracing using [LTTng](https://lttng.org/) (Linux Trace Toolkit Next Generation).

## LTTng Installation

To install LTTng on your Linux system, follow the instructions provided in the [LTTng documentation](https://lttng.org/download/)

> Dependency LTTNG version is greater than 2.12.

## LTTng QuickStart

LTTng is an open source tracing framework for Linux that provides highly efficient and low-overhead tracing capabilities. It allows developers to trace both kernel and user-space applications.

Building Valkey with LTTng support:

```
USE_LTTNG=yes make
```

Enable lttng trace events dynamically:
```
~# lttng destroy valkey
~# lttng create valkey
~# lttng enable-event -u 'valkey*'
~# lttng track -u -p `pidof valkey-server`
~# lttng start
~# lttng stop
~# lttng view
```

Examples (a client run 'SET', another run 'keys'):
```
...
[15:30:19.334463706] (+0.000001243) libai valkey:command_call: { cpu_id = 15 }, { name = "set", duration = 0 }
[15:30:19.334465183] (+0.000001477) libai valkey:command_call: { cpu_id = 15 }, { name = "set", duration = 1 }
[15:30:19.334466516] (+0.000001333) libai valkey:command_call: { cpu_id = 15 }, { name = "set", duration = 0 }
[15:30:19.334467738] (+0.000001222) libai valkey:command_call: { cpu_id = 15 }, { name = "set", duration = 0 }
[15:30:19.334469105] (+0.000001367) libai valkey:command_call: { cpu_id = 15 }, { name = "set", duration = 1 }
[15:30:19.334470327] (+0.000001222) libai valkey:command_call: { cpu_id = 15 }, { name = "set", duration = 0 }
[15:30:19.369348485] (+0.034878158) libai valkey:command_call: { cpu_id = 15 }, { name = "keys", duration = 34874 }
[15:30:19.369698322] (+0.000349837) libai valkey:command_call: { cpu_id = 15 }, { name = "set", duration = 4 }
[15:30:19.369702327] (+0.000004005) libai valkey:command_call: { cpu_id = 15 }, { name = "set", duration = 2 }
[15:30:19.369704098] (+0.000001771) libai valkey:command_call: { cpu_id = 15 }, { name = "set", duration = 1 }
[15:30:19.369705884] (+0.000001786) libai valkey:command_call: { cpu_id = 15 }, { name = "set", duration = 0 }
[15:30:19.369707501] (+0.000001617) libai valkey:command_call: { cpu_id = 15 }, { name = "set", duration = 1 }
[15:30:19.369708743] (+0.000001242) libai valkey:command_call: { cpu_id = 15 }, { name = "set", duration = 0 }
[15:30:19.369710052] (+0.000001309) libai valkey:command_call: { cpu_id = 15 }, { name = "set", duration = 1 }
[15:30:19.369711619] (+0.000001567) libai valkey:command_call: { cpu_id = 15 }, { name = "set", duration = 0 }
...
```

Then we can use another script to analyze topN slow commands and other system
level events.

About performance overhead (valkey-benchmark -t get -n 1000000 --threads 4):
1> no lttng builtin: 285632.69 requests per second
2> lttng builtin, no trace: 285551.09 requests per second (almost 0 overhead)
3> lttng builtin, trace commands: 266595.59 requests per second (about ~6.6 overhead)

Generally valkey-server would not run in full utilization, the overhead is acceptable.

## Supported Events

| event                    | provider |
| -------------------------- | ---------- |
| command-call             | commands  |
| rdb-unlink-temp-file     | sys      |
| fork                     | sys      |
| command-unblocking       | server   |
| while-blocked-cron       | server   |
| module-acquire-GIL       | server   |
| expire-del               | db       |
| active-defrag-cycle      | db       |
| eviction-del             | db       |
| eviction-lazyfree        | db       |
| eviction-cycle           | db       |
| expire-cycle             | db       |
| cluster-config-open      | cluster  |
| cluster-config-write     | cluster  |
| cluster-config-fsync     | cluster  |
| cluster-config-rename    | cluster  |
| cluster-config-dir-fsync | cluster  |
| cluster-config-close     | cluster  |
| cluster-config-unlink    | cluster  |
| aof-write-pending-fsync  | aof      |
| aof-write-active-child   | aof      |
| aof-write-alone          | aof      |
| aof-write                | aof      |
| aof-fsync-always         | aof      |
| aof-fstat                | aof      |
| aof-rename               | aof      |