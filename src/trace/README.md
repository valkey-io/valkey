## Introduction

This directory contains the implementation of tracing using [LTTng](https://lttng.org/) (Linux Trace Toolkit Next Generation).

## LTTng Installation

To install LTTng on your Linux system, follow the instructions provided in the [LTTng documentation](https://lttng.org/download/)

> Dependency LTTNG version is greater than 2.12.

### Install from package manager

#### [Ubuntu](https://lttng.org/docs/v2.13/#doc-ubuntu)

LTTng 2.13 is available on Ubuntu 22.04 LTS *Jammy Jellyfish*, Ubuntu 23.04 *Lunar Lobster*, and Ubuntu 23.10 *Mantic Minotaur*. For previous supported releases of Ubuntu, [use the LTTng Stable 2.13 PPA](https://lttng.org/docs/v2.13/#doc-ubuntu-ppa).

To install LTTng 2.13 on Ubuntu 22.04 LTS *Jammy Jellyfish*:

1. Install the main LTTng 2.13 packages:
   ```
   apt-get install lttng-tools
   apt-get install lttng-modules-dkms
   apt-get install liblttng-ust-dev
   ```

#### [Debian](https://lttng.org/docs/v2.13/#doc-debian)

To install LTTng 2.13 on Debian 12 *bookworm*:

1. Install the main LTTng 2.13 packages:
   ```
   apt install lttng-modules-dkms
   apt install liblttng-ust-dev
   apt install lttng-tools
   ```

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
~# lttng enable-event -u 'valkey_server:*'
~# lttng enable-event -u 'valkey_commands:*'
~# lttng track -u -p `pidof valkey-server`
~# lttng start
~# lttng stop
~# lttng view
```

Examples (a client run 'SET', another run 'keys'):
```
...
[15:30:19.334467738] (+0.000001222) xxx valkey_commands:command_call: { cpu_id = 15 }, { name = "set", duration = 0 }
[15:30:19.334469105] (+0.000001367) xxx valkey_commands:command_call: { cpu_id = 15 }, { name = "set", duration = 1 }
[15:30:19.334470327] (+0.000001222) xxx valkey_commands:command_call: { cpu_id = 15 }, { name = "set", duration = 0 }
[15:30:19.369348485] (+0.034878158) xxx valkey_commands:command_call: { cpu_id = 15 }, { name = "keys", duration = 34874 }
[15:30:19.369698322] (+0.000349837) xxx valkey_commands:command_call: { cpu_id = 15 }, { name = "set", duration = 4 }
[15:30:19.369702327] (+0.000004005) xxx valkey_commands:command_call: { cpu_id = 15 }, { name = "set", duration = 2 }
[15:30:19.369704098] (+0.000001771) xxx valkey_commands:command_call: { cpu_id = 15 }, { name = "set", duration = 1 }
[15:30:19.369705884] (+0.000001786) xxx valkey_commands:command_call: { cpu_id = 15 }, { name = "set", duration = 0 }
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

| event                    | provider        |
|--------------------------|-----------------|
| command_call             | valkey_commands |
| eventloop                | valkey_server   |
| eventloop_cron           | valkey_server   |
| while_blocked_cron       | valkey_server   |
| module_acquire_gil       | valkey_server   |
| command_unblocking       | valkey_server   |
| fast_command             | valkey_server   |
| command                  | valkey_server   |
| expire_del               | valkey_db       |
| active_defrag_cycle      | valkey_db       |
| eviction_del             | valkey_db       |
| eviction_lazyfree        | valkey_db       |
| eviction_cycle           | valkey_db       |
| expire_cycle             | valkey_db       |
| expire_cycle_fields      | valkey_db       |
| expire_cycle_keys        | valkey_db       |
| fork                     | valkey_cluster  |
| cluster_config_open      | valkey_cluster  |
| cluster_config_write     | valkey_cluster  |
| cluster_config_fsync     | valkey_cluster  |
| cluster_config_rename    | valkey_cluster  |
| cluster_config_dir_fsync | valkey_cluster  |
| cluster_config_close     | valkey_cluster  |
| cluster_config_unlink    | valkey_cluster  |
| fork                     | valkey_rdb      |
| rdb_unlink_temp_file     | valkey_rdb      |
| fork                     | valkey_aof      |
| aof_write_pending_fsync  | valkey_aof      |
| aof_write_active_child   | valkey_aof      |
| aof_write_alone          | valkey_aof      |
| aof_write                | valkey_aof      |
| aof_fsync_always         | valkey_aof      |
| aof_fstat                | valkey_aof      |
| aof_rename               | valkey_aof      |
| aof_flush                | valkey_aof      |
