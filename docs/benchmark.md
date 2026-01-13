# Valkey Benchmark

Benchmark utility for measuring Valkey server performance.

```bash
valkey-benchmark [OPTIONS] [--] [COMMAND ARGS...]
```

## Connection Options

| Option | Description |
|--------|-------------|
| `-h <hostname>` | Server hostname (default: 127.0.0.1) |
| `-p <port>` | Server port (default: 6379) |
| `-s <socket>` | Server socket (overrides host and port) |
| `-u <uri>` | Server URI: `valkey://user:password@host:port/dbnum` |
| `-a <password>` | Password for Valkey Auth |
| `--user <username>` | Used to send ACL style 'AUTH username pass'. Needs `-a` |
| `--dbnum <db>` | SELECT the specified db number (default: 0) |
| `-3` | Start session in RESP3 protocol mode |

## Performance Options

| Option | Description |
|--------|-------------|
| `-c <clients>` | Number of parallel connections (default: 50) |
| `-n <requests>` | Total number of requests (default: 100000) |
| `--duration <seconds>` | Run benchmark for specified number of seconds (mutually exclusive with `-n`) |
| `--warmup <seconds>` | Run benchmark for specified warmup period before recording data |
| `-d <size>` | Data size of SET/GET value in bytes (default: 3) |
| `-P <numreq>` | Pipeline requests (default: 1, no pipeline) |
| `-k <boolean>` | Keep alive: 1=keep alive, 0=reconnect (default: 1) |
| `--threads <num>` | Enable multi-thread mode |
| `--rps <requests>` | Limit requests per second (default: 0, no limit) |

## Test Selection

| Option | Description |
|--------|-------------|
| `-t <tests>` | Comma-separated list of tests to run |
| `-l` | Loop mode: run tests forever |
| `-I` | Idle mode: open N idle connections and wait |

Available tests: `ping`, `ping_inline`, `ping_mbulk`, `set`, `get`, `incr`, `lpush`, `rpush`, `lpop`, `rpop`, `sadd`, `hset`, `spop`, `zadd`, `zpopmin`, `lrange`, `lrange_100`, `lrange_300`, `lrange_500`, `lrange_600`, `mset`, `mget`, `xadd`, `function_load`, `fcall`

## Output Options

| Option | Description |
|--------|-------------|
| `-q` | Quiet mode: show only query/sec values |
| `--csv` | Output in CSV format |
| `--precision` | Number of decimal places in latency output (default: 0) |

## Cluster Options

| Option | Description |
|--------|-------------|
| `--cluster` | Enable cluster mode |
| `--rfr <mode>` | Read from replicas: `no`/`yes`/`all` (default: `no`) |

## Randomization Options

| Option | Description |
|--------|-------------|
| `-r <keyspacelen>` | Use random keys in range [0, keyspacelen-1] |
| `--sequential` | Use sequential numbers instead of random |
| `--seed <num>` | Set random number generator seed |

## Dataset Support

| Option | Description |
|--------|-------------|
| `--dataset <file>` | Dataset file for field placeholder replacement |
| `--maxdocs <num>` | Maximum number of documents to load from dataset (default: unlimited) |
| `--xml-root-element <name>` | Root element name for XML dataset parsing (required for XML files) |

### File Formats

**CSV**
```csv
term,category
anarchism,politics
democracy,politics
```
Header row required, comma-delimited, field names become `__field:name__` placeholders.

**TSV**  
Tab-delimited with header row.

**XML**
```xml
<page>
    <title>Anarchism</title>
    <id>12</id>
    <revision>
        <id>1317806107</id>
        <text bytes="112881">Article content...</text>
    </revision>
</page>
```
Requires `--xml-root-element` parameter. Root element choice affects discovered fields - deeper elements include nested content.

### Dataset Behavior

- One row per command
- Sequential iteration with wraparound  
- Thread-safe atomic selection
- Duplicate XML field names: first occurrence wins

### Usage

```bash
# CSV dataset
valkey-benchmark --dataset terms.csv \
  -n 50000 FT.SEARCH myindex "__field:term__"

# Wikipedia XML  
valkey-benchmark --dataset wiki.xml --xml-root-element page \
  -n 10000 HSET "doc:__rand_int__" title "__field:title__" body "__field:text__"
```

**Memory:** Large datasets may require GB-scale RAM.

## Additional Options

| Option | Description |
|--------|-------------|
| `--enable-tracking` | Send CLIENT TRACKING on |
| `--num-functions <num>` | Functions in Lua lib (default: 10) |
| `--num-keys-in-fcall <num>` | Keys for FCALL (default: 1) |
| `--seed <num>` | RNG seed |
| `-x` | Read last arg from STDIN |
| `--mptcp` | Enable MPTCP |
| `--help` | Show help |
| `--version` | Show version |

## Placeholder System

### Random Placeholders

| Placeholder | Behavior |
|-------------|----------|
| `__rand_int__` | Different random value per occurrence |
| `__rand_1st__` | Same random value for all occurrences in command |
| `__rand_2nd__` | Same random value for all occurrences in command |
| ... | ... |
| `__rand_9th__` | Same random value for all occurrences in command |

Random values are 12-digit zero-padded numbers in range [0, keyspacelen-1].

### Data Placeholders

| Placeholder | Description |
|-------------|-------------|
| `__data__` | Random data of size specified by `-d` option |

### Cluster Placeholders

| Placeholder | Description |
|-------------|-------------|
| `{tag}` | Cluster slot hashtag for proper key distribution |

Required in cluster mode to ensure commands route to correct nodes.

## Command Sequences

Commands can be chained using semicolon separators:

```bash
valkey-benchmark -- multi ';' set key:__rand_int__ __data__ ';' incr counter ';' exec
```

### Repetition Syntax

Prefix commands with a number to repeat:

```bash
valkey-benchmark -- 5 set key:__rand_int__ value ';' get key:__rand_int__
```

This executes 5 SET commands followed by 1 GET command per pipeline iteration.


## Examples

### Basic Benchmarking

```bash
# Default benchmark suite
valkey-benchmark

# Specific tests
valkey-benchmark -t ping,set,get -n 100000

# Custom data size
valkey-benchmark -t set -d 1024 -n 50000
```

### Random Key Distribution

```bash
# Random keys in range [0, 999999]
valkey-benchmark -t set,get -r 1000000 -n 100000

# Sequential keys
valkey-benchmark -t set --sequential -r 1000000 -n 100000
```

### Dataset-Driven Benchmarking

```bash
# CSV dataset
valkey-benchmark --dataset terms.csv \
  -n 50000 FT.SEARCH myindex "__field:term__"

# Wikipedia XML dataset (page-level)
valkey-benchmark --dataset wiki_sample.xml --xml-root-element page \
  -n 10000 HSET "doc:__rand_int__" title "__field:title__" content "__field:text__" id "__field:id__"

# Wikipedia XML dataset (revision-level)  
valkey-benchmark --dataset wiki_sample.xml --xml-root-element revision \
  -n 10000 HSET "doc:__rand_int__" content "__field:text__" timestamp "__field:timestamp__"

# Multiple field usage
valkey-benchmark --dataset products.csv \
  -- HSET product:__field:id__ name "__field:name__" price __field:price__
```

### Cluster Benchmarking

```bash
# Cluster mode with proper key distribution
valkey-benchmark --cluster -t set,get \
  -- SET key:{tag}:__rand_int__ __data__

# Read from replicas
valkey-benchmark --cluster --rfr yes -t get \
  -- GET key:{tag}:__rand_int__
```

### Pipelining

```bash
# Pipeline 10 requests
valkey-benchmark -P 10 -t set -n 100000

# Pipeline with datasets
valkey-benchmark --dataset terms.csv -P 5 \
  -n 50000 FT.SEARCH index "__field:term__"
```

### Complex Command Sequences

```bash
# Transaction benchmark
valkey-benchmark -r 100000 -n 10000 \
  -- multi ';' set key:__rand_int__ __data__ ';' \
     incr counter:__rand_int__ ';' exec

# Mixed operations with repetition
valkey-benchmark -r 100000 \
  -- 3 set key:__rand_int__ __data__ ';' \
     2 get key:__rand_int__ ';' \
     del key:__rand_int__
```

### Rate Limiting

```bash
# Limit to 1000 requests/second
valkey-benchmark --rps 1000 -t set -n 50000

# Dataset with rate limiting
valkey-benchmark --dataset search_terms.csv --rps 500 \
  -n 10000 FT.SEARCH index "__field:term__"
```
