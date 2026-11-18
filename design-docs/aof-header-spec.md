# Append Only File (AOF) Metadata Header Specification

This document defines the specification for the metadata header in Valkey's Append Only File (AOF). The header is designed to carry optional block-level metadata, such as replication state (replication ID and offset) for state restoration, and block checksums for data integrity verification.

---

## 1. Format Rationale: Text vs. Base64

To store metadata in the AOF, we propose the header to be formatted as an AOF annotation for backwards compatibility (starting with `#` and ending with `\r\n`). 

### Ruling out Raw Binary
A raw binary format is ruled out immediately due to backward incompatibility and parsing constraints. Raw binary payloads can naturally contain null bytes (`\0`) or carriage return/line feed sequences (`\r\n`), which would prematurely terminate or corrupt the annotation line when parsed using standard line-oriented functions (like `fgets`) in the AOF loader.

Therefore, the realistic choice is between **Structured Text** and **Base64-Encoded Binary**.

### Size Comparison (Expected Overhead)

To evaluate the size impact, we compare the overhead of storing metadata fields (length, checksum, replication ID, and replication offset) across different formats:

1. **Text Format (Key-Value)**:
   - Example: `#HDR:v1;len:123456;replid:a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2;reploff:123456789012;checksum:12345678901234567890;\r\n`
   - Estimated size: **~50 to 130 bytes** depending on which optional fields are present.

2. **Base64-Encoded Binary**:
   - Encoding the binary payload of size `N` bytes: `ceil(N / 3) * 4` characters.
   - Adding prefix/suffix: `#HDR:v1;<base64_payload>\r\n` -> **~26 to 66 bytes** depending on present fields.

### Trade-off Analysis (Text vs. Base64)

| Metric | Text Format (Key-Value) | Base64-Encoded Binary |
| :--- | :--- | :--- |
| **Size Overhead** | High (~50B - 130B) | Medium (~26B - 66B) |
| **Human Readable** | Yes | No |
| **Extensibility** | Very High (Trivial to append fields) | Medium (Requires serialization like TLV) |

### Key Design Decisions for Choosing Text:

- **Readability**: Storing metadata in text preserves the ability to inspect, debug, and manually repair AOF files using standard command-line tools (e.g., `grep`, `tail`, `vi`). A Base64 blob is opaque and requires external decoding tools.
- **Extensibility**: Text-based key-value pairs are trivially extensible. New fields can be appended (`key:value;`), and older parsers will naturally ignore them. Base64 requires packing/unpacking logic (like TLV) to allow older parsers to skip unknown fields.
- **Amortized Overhead**: Because headers are written once per AOF flush cycle (grouping all commands currently buffered in `server.aof_buf`) and not per individual command, the overhead is amortized. Under high write throughput, the average byte overhead per command is negligible. With one small command per flush (for example, a single client), the headers can almost double the AOF size.

---

## 2. Formal Grammar (ABNF)

The AOF metadata header MUST conform to the following Augmented Backus-Naur Form (ABNF) grammar (RFC 5234):

```abnf
AOF-HEADER      = "#HDR:" VERSION ";" 1*FIELD CRLF
VERSION         = "v1"
FIELD           = LEN-FIELD / CHECKSUM-FIELD / OTHER-FIELD
LEN-FIELD       = "len:" BLOCK-LEN ";"
CHECKSUM-FIELD  = "checksum:" CHECKSUM ";"
OTHER-FIELD     = FIELD-KEY ":" FIELD-VALUE ";"
BLOCK-LEN       = "0" / (%x31-39 *DIGIT) ; Length of the following RESP command block in bytes, without leading zeros
CHECKSUM        = 1*DIGIT ; CRC64 checksum represented as decimal
FIELD-KEY       = "replid" / "reploff" / 1*KEY-CHAR
FIELD-VALUE     = *VALUE-CHAR
KEY-CHAR        = %x21-39 / %x3C-7E ; Printable, except ":" and ";"
VALUE-CHAR      = %x21-3A / %x3C-7E ; Printable, except ";"
CRLF            = %x0D %x0A ; \r\n
```

### Grammar Constraints:
1. **Required `len`**: The `len` field is **required** and MUST appear exactly once.
2. **Optional `checksum`**: The `checksum` field MUST appear at most once.
3. **Field Order**: Fields MAY appear in any order.
4. **Decimal Representation**: Values for `len`, `checksum`, and `reploff` MUST be represented as decimal integers without a sign, and `len` and `checksum` MUST fit in 64 bits.
5. **Maximum Length**: The header line, including its CRLF, MUST NOT exceed 1023 bytes.
6. **Replication Fields**: `replid` and `reploff` MUST appear at most once. `replid` MUST have 40 characters, and `reploff` MUST have no leading zeros and fit in a signed 64-bit integer.

---

## 3. Well-Known Fields

| Field Name | Type | Description |
| :--- | :--- | :--- |
| `len` | Decimal Integer | **Required.** The number of bytes in the block, starting right after the header's CRLF. |
| `checksum` | Decimal Integer | **Required in files written with integrity.** The running CRC64 at the end of the block, see section 4. |
| `replid` | String (40 chars) | **Optional.** The replication ID, when it differs from the last one in the file, see section 6. |
| `reploff` | Decimal Integer | **Optional.** The replication offset at the end of the block, see section 6. |

### Header Variants based on Server State:

- **AOF Integrity Enabled (`aof-integrity-check yes`)**:
  Requires `len` and `checksum`.
  ```text
  #HDR:v1;len:45;checksum:1234567890123456789;
  ```

- **Replication Offsets Enabled (`aof-replication-offsets yes`)**:
  Adds `reploff`, and `replid` when needed, before `checksum` if there's one.
  ```text
  #HDR:v1;len:45;replid:a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2;reploff:100500;
  #HDR:v1;len:45;reploff:100545;checksum:1234567890123456789;
  ```

---

## 4. Checksum Calculation Semantics

If the `checksum` field is present, its value is calculated as follows:

1. **Scope**: The checksum is a continuous CRC64 of the stream.
2. **Coverage**: The checksum covers every byte of its block, the header line and the payload (of length specified by `len`), except the checksum value itself. It's calculated as if the value were empty.
   - Specifically, for the header: `#HDR:v1;len:45;reploff:100500;checksum:12345;\r\n`
   - The checksummed header string is: `#HDR:v1;len:45;reploff:100500;checksum:;\r\n`
3. **Validation**: The loader calculates `CRC64(PreviousChecksum, HeaderString + PayloadData)` and compares it to the parsed `checksum` value.
4. **Chain**: `PreviousChecksum` is the `checksum` of the previous block. The chain continues across INCR files and AOF rewrites: the first block of an INCR file continues from the end of the INCR file before it. The first INCR file after a base continues from the checksum recorded for the base. The chain restarts from 0 after a file written without integrity, or after a base without a recorded checksum.

---

## 5. Integrity Files

Integrity is a property of each AOF file, recorded in the manifest.

- **INCR files**: An INCR file written with integrity has a header before its first command, and before every block after it. Every command lies entirely inside one block.
- **RESP base files**: A RESP base file written with integrity ends with a `#CRC:<crc64>\r\n` line, holding the CRC64 (starting from 0, decimal) of every byte before that line. Nothing follows it.
- **RDB base files**: They are protected by the RDB checksum, which is written and verified even if `rdbchecksum` is disabled. Nothing follows the RDB.
- **Manifest keys**: A file line can have `integrity 1`, for a file written with integrity. The base line can also have `checksum <n>`: the running checksum when the rewrite that created it started, which the first INCR file continues from, since the base's content isn't part of the chain.
- **Manifest checksum**: If any file line has `integrity 1` or a `checksum`, the manifest ends with a `# manifest-checksum: <n>` line, holding the CRC64 (starting from 0, decimal) of all the bytes before that line, line terminators included. Nothing follows it.
- **Loading**: Files are only verified when `aof-integrity-check` is enabled. Otherwise, headers are skipped like other annotations, except to restore the replication offset, see section 6. Since the end of the last INCR file isn't verified, it's no longer treated as an integrity file. With `aof-load-truncated yes`, an integrity file is only truncated at the end of a verified block, when the block after it is incomplete, and only if it's the last file. A last block whose `len` was corrupted to a larger value can't be told apart from a torn write, so it's truncated too. A checksum mismatch is an error, and so is an empty base written with integrity.

---

## 6. Replication Offsets

With `aof-replication-offsets yes`, the AOF records the replication ID and offset of its data, so that a restarted server can partially resynchronize, as with the ones an RDB file records.

- **Headers**: A header has `reploff`, the replication offset at the end of its block, whenever an RDB file would record it: on a replica that has a primary, connected or not, or on a primary with a replication backlog. The first header with `reploff` of each file also has `replid`, the replication ID, and so does a header after the ID changes. Otherwise, the ID is the one of the last header of the file that has it. Such headers are written with or without integrity.
- **Replication Stream DB**: The replication stream can select a DB without writing to the AOF, e.g. for `PUBLISH` from another DB. If the AOF has another DB selected, a block with `reploff` ends with a `SELECT` of the DB of the replication stream, so the DB selected at the end of the block is the one of the stream at that offset.
- **Shutdown**: The offset also grows without writes, e.g. with `PUBLISH`. At shutdown, the server writes a block that only selects the DB of the stream, to record the last offset. It's best effort: a failure to write it doesn't stop the shutdown. A short write that can't be undone leaves a torn block, as for any other write.
- **Base Files**: An RDB base has the `repl-id`, `repl-offset` and `repl-stream-db` aux fields. So does the RDB file that a replica reuses as its base after a full sync, which has those of its primary. A RESP base ends with a block like those of INCR files, with `replid` and `reploff`, that only selects the DB of the stream. The CRC of the base covers it.
- **Writes Not in the AOF**: The offset assumes that the AOF gets every write that replicas get. A write sent only to replicas, e.g. with Lua's `server.set_repl()` or a module call with the `A` flag, isn't in the AOF: after a restart, the primary doesn't have it, while its replicas, which partially resynchronize, keep it.
- **Loading**: The `reploff` of a header applies once the last byte of its block is loaded. The server restores the replication ID and offset only if the loaded data ends with such a block, and not, for example, if commands without a header follow it. A block that ends past the end of the last file was torn, and it's truncated as a whole, as with integrity. Without integrity, a header corrupted on disk can't be detected.
- **valkey-check-aof**: In the last file, it also treats a header that parses as the start of a block, with or without integrity: `--fix` truncates a torn block as a whole, and `--truncate-to-timestamp` truncates the block that has the timestamp as a whole.

---

## 7. Versioning and Extensibility

- **Versioning**: The version token (e.g., `v1`) defines the strict grammar. A loader skips a header with an unknown version like any other annotation.
- **Extensibility**: Within a version (e.g., `v1`), the parser MUST ignore any unrecognized fields. This allows a server running only integrity checks to safely skip `replid` and `reploff` fields, ensuring forward compatibility.

---

## 8. Alternatives Considered: Base64-Encoded Packed Binary

We considered using a packed binary layout encoded in Base64 for maximum storage efficiency.

### How it would work:
1. **Binary Payload**: Consists of a 1-byte presence bitmask followed by only the active fields appended back-to-back in big-endian format.
   - Bit 0: `len` (4B)
   - Bit 1: `checksum` (8B)
   - Bit 2: `repl_id` (20B raw)
   - Bit 3: `repl_offset` (8B)
2. **Base64 Encoding**: The binary payload is encoded to Base64 to make it safe for AOF annotations (e.g., `#HDR:v1:AwAAAA0R2zREVVZndw==\r\n`).
3. **Extensibility**: Older parsers read the bitmask and unpack only the fields they know, ignoring any trailing bytes in the decoded buffer (which would contain newer fields).

### Trade-off Evaluation:
* **Pros**: 
  - **High Storage Efficiency**: Reduces header size by ~45% to 50% (saving ~24 to 62 bytes per header depending on the active fields).
* **Cons**:
  - **Loss of Readability**: The header becomes opaque. Administrators cannot easily verify replication offsets or checksums by simply reading the AOF file.
  - **Parsing Complexity**: Requires a Base64 decoder and bit-shifting logic in the critical path of AOF loading.
  - **Inconsistency**: If storage efficiency were the primary goal, the entire AOF format (which is verbose RESP text) would need to be optimized (e.g., to a binary AOF format). Optimizing only the header while keeping the rest of the file in RESP text yields diminishing returns and introduces inconsistency.
