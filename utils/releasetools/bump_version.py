#!/usr/bin/env python3
"""Set the Valkey version macros in src/version.h.

Rewrites three macros in place:

    #define VALKEY_VERSION "M.m.p"
    #define VALKEY_VERSION_NUM 0x00MMmmpp
    #define VALKEY_RELEASE_STAGE "dev"|"rcN"|"ga"

``VALKEY_VERSION_NUM`` packs major/minor/patch into one byte each, matching the
documented ``0x00MMmmpp`` scheme used by ``VM_GetServerVersion`` (src/module.c)
and parsed by ``version2num`` (src/util.c). Other macros (SERVER_NAME,
REDIS_VERSION, ...) are left untouched.
"""

from __future__ import annotations

import argparse
import re
import sys

try:  # Allow both `python -m` and direct-script execution.
    from release_notes import parse_version
except ImportError:  # pragma: no cover - import shim
    from utils.releasetools.release_notes import parse_version  # type: ignore

_VERSION_DEFINE_RE = re.compile(r'^(#define\s+VALKEY_VERSION\s+)"[^"]*"', re.MULTILINE)
_VERSION_NUM_DEFINE_RE = re.compile(r"^(#define\s+VALKEY_VERSION_NUM\s+)0x[0-9A-Fa-f]+", re.MULTILINE)
_STAGE_DEFINE_RE = re.compile(r'^(#define\s+VALKEY_RELEASE_STAGE\s+)"[^"]*"', re.MULTILINE)

_STAGE_RE = re.compile(r"^(dev|ga|rc\d+)$")


def version_num(version: str) -> str:
    """Return the ``0x00MMmmpp`` hex literal for a ``"M.m.p"`` version string."""
    major, minor, patch = parse_version(version)
    return "0x00{:02x}{:02x}{:02x}".format(major, minor, patch)


def _validate_stage(stage: str) -> str:
    stage = stage.strip().lower()
    if not _STAGE_RE.match(stage):
        raise ValueError(
            "release stage must be 'dev', 'ga', or 'rcN' (e.g. rc1), got {!r}".format(stage)
        )
    return stage


def set_version(version_h_text: str, version: str, stage: str) -> str:
    """Return *version_h_text* with the three Valkey version macros updated."""
    # parse_version validates the M.m.p range and raises on bad input.
    parse_version(version)
    stage = _validate_stage(stage)

    text, n1 = _VERSION_DEFINE_RE.subn(
        lambda m: '{}"{}"'.format(m.group(1), version), version_h_text
    )
    text, n2 = _VERSION_NUM_DEFINE_RE.subn(
        lambda m: "{}{}".format(m.group(1), version_num(version)), text
    )
    text, n3 = _STAGE_DEFINE_RE.subn(
        lambda m: '{}"{}"'.format(m.group(1), stage), text
    )
    missing = [
        name
        for name, count in (
            ("VALKEY_VERSION", n1),
            ("VALKEY_VERSION_NUM", n2),
            ("VALKEY_RELEASE_STAGE", n3),
        )
        if count == 0
    ]
    if missing:
        raise ValueError(
            "could not find these macros in version.h: {}".format(", ".join(missing))
        )
    return text


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Set the Valkey version macros in version.h.")
    parser.add_argument("--version", required=True, help="Target version, e.g. 9.1.0")
    parser.add_argument(
        "--stage",
        required=True,
        help="Release stage: dev, rc1..rcN, or ga",
    )
    parser.add_argument(
        "--file",
        default="src/version.h",
        help="Path to version.h (default: src/version.h)",
    )
    args = parser.parse_args(argv)

    with open(args.file, "r", encoding="utf-8") as fh:
        original = fh.read()
    try:
        updated = set_version(original, args.version, args.stage)
    except ValueError as exc:
        print("error: {}".format(exc), file=sys.stderr)
        return 2
    if updated != original:
        with open(args.file, "w", encoding="utf-8") as fh:
            fh.write(updated)
    print(
        "Set VALKEY_VERSION={} VALKEY_VERSION_NUM={} VALKEY_RELEASE_STAGE={} in {}".format(
            args.version, version_num(args.version), args.stage.strip().lower(), args.file
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
