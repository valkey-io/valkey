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
- **Amortized Overhead**: Because headers are written once per AOF flush cycle (grouping all commands currently buffered in `server.aof_buf`) and not per individual command, the overhead is amortized. Under high write throughput, the average byte overhead per command is negligible.

---

## 2. Formal Grammar (ABNF)

The AOF metadata header MUST conform to the following Augmented Backus-Naur Form (ABNF) grammar (RFC 5234):

```abnf
AOF-HEADER      = "#HDR:" VERSION ";len:" BLOCK-LEN ";" *FIELD [ "checksum:" CHECKSUM ";" ] CRLF
VERSION         = "v1"
BLOCK-LEN       = 1*DIGIT ; Length of the following RESP command block in bytes
CHECKSUM        = 1*DIGIT ; CRC64 checksum represented as decimal
FIELD           = FIELD-KEY ":" FIELD-VALUE ";"
FIELD-KEY       = "replid" / "reploff" / TOKEN
FIELD-VALUE     = TOKEN
TOKEN           = 1*(%x30-39 / %x41-5A / %x61-7A / "-" / "_") 
                  ; One or more alphanumeric characters, hyphens, or underscores
CRLF            = %x0D %x0A ; \r\n
```

### Grammar Constraints:
1. **Required `len`**: The `len` field is **required** and MUST be the first field in the header (immediately following the version).
2. **Ordered Checksum**: The `checksum` field, if present, MUST be the **last** field in the header.
3. **Decimal Representation**: Values for `len`, `checksum`, and `reploff` MUST be represented as decimal integers (adhering to `%x30-39` in `TOKEN` and `1*DIGIT`).

---

## 3. Well-Known Fields

| Field Name | Type | Description |
| :--- | :--- | :--- |
| `len` | Decimal Integer | **Required.** The length of the upcoming RESP command block in bytes. |
| `checksum` | Decimal Integer | **Optional.** The continuous CRC64 checksum of the stream. |
| `replid` | Hex String (40 chars) | **Optional.** The replication ID associated with this state. |
| `reploff` | Decimal Integer | **Optional.** The replication offset associated with this state. |

### Header Variants based on Server State:

- **AOF Integrity Enabled (`aof-integrity-check yes`)**:
  Requires `len` and `checksum`.
  ```text
  #HDR:v1;len:45;checksum:1234567890123456789;
  ```

- **Replication State Restore Enabled (`aof-replication-restore yes`)**:
  Requires `len` and `reploff`. `replid` is optional.
  ```text
  #HDR:v1;len:45;replid:a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2;reploff:100500;
  ```

- **Both Integrity and Replication Restore Enabled**:
  Requires `len`, `reploff`, and `checksum` (must be last). `replid` is optional.
  ```text
  #HDR:v1;len:45;replid:a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2;reploff:100500;checksum:1234567890123456789;
  ```

---

## 4. Checksum Calculation Semantics

If the `checksum` field is present, it MUST be the last field in the header, and its value is calculated as follows:

1. **Scope**: The checksum is a continuous CRC64 of the stream.
2. **Header Contribution**: The checksum calculation for the current block includes the header prefix and all preceding fields up to the `checksum:` key itself, including the colon.
   - Specifically, for the header: `#HDR:v1;len:45;replid:abc;checksum:12345;\r\n`
   - The checksummed header string is: `#HDR:v1;len:45;replid:abc;checksum:`
3. **Payload Contribution**: The calculation then continues over the payload data of the block (of length specified by `len`).
4. **Validation**: The loader calculates `CRC64(PreviousChecksum, HeaderString + PayloadData)` and compares it to the parsed `checksum` value. The trailing `;\r\n` of the header is excluded from the checksum calculation.

---

## 5. Versioning and Extensibility

- **Versioning**: The version token (e.g., `v1`) defines the strict grammar. If a breaking change is made (e.g., changing the checksum algorithm), the version MUST be bumped to `v2`. A parser encountering an unknown version MUST fail-fast.
- **Extensibility**: Within a version (e.g., `v1`), the parser MUST ignore any unrecognized fields. This allows a server running only integrity checks to safely skip `replid` and `reploff` fields, ensuring forward compatibility.

---

## 6. Alternatives Considered: Base64-Encoded Packed Binary

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
