#!/usr/bin/env python3
"""Parse the 00-RELEASENOTES "## Unreleased" block.

The unstable branch keeps a "## Unreleased" block in 00-RELEASENOTES that
user-facing PRs append to as they merge. This module extracts and measures
that block; it is consumed by the release-notes CI check
(check_release_notes.py) to require a net-new entry on labelled PRs.

The module is pure (no I/O, no network) so it is cheap to unit test. Rendering
and promotion of the block into dated release sections live elsewhere and are
added alongside the release-cutting tooling.
"""

from __future__ import annotations

import re
from collections import OrderedDict
from typing import Dict, List, Optional

UNRELEASED_HEADER = "## Unreleased"

_BULLET_RE = re.compile(r"^\s*[*-]\s+\S")
_CATEGORY_RE = re.compile(r"^###\s+(.*\S)\s*$")
_H2_RE = re.compile(r"^##\s+\S")


def parse_unreleased(text: str) -> "OrderedDict[str, List[str]]":
    """Extract the "## Unreleased" block as an ordered ``category -> bullets`` map.

    Only the region between the ``## Unreleased`` header and the next level-2
    header (or end of file) is considered. HTML comments and blank lines are
    skipped. Categories are returned in the order they appear.
    """
    result: "OrderedDict[str, List[str]]" = OrderedDict()
    lines = text.splitlines()

    # Locate the "## Unreleased" header.
    start = None
    for i, line in enumerate(lines):
        if line.strip() == UNRELEASED_HEADER:
            start = i + 1
            break
    if start is None:
        return result

    current: Optional[str] = None
    in_comment = False
    for line in lines[start:]:
        stripped = line.strip()
        if _H2_RE.match(line) and stripped != UNRELEASED_HEADER:
            break  # next top-level section ends the Unreleased block
        if in_comment:
            if "-->" in stripped:
                in_comment = False
            continue
        if stripped.startswith("<!--"):
            if "-->" not in stripped:
                in_comment = True
            continue
        cat_match = _CATEGORY_RE.match(line)
        if cat_match:
            current = cat_match.group(1)
            result.setdefault(current, [])
            continue
        if current is not None and _BULLET_RE.match(line):
            result[current].append(stripped)
    return result


def is_unreleased_empty(notes: "Dict[str, List[str]]") -> bool:
    """True when no category in *notes* carries any bullet."""
    return not any(bullets for bullets in notes.values())


def count_bullets(notes: "Dict[str, List[str]]") -> int:
    """Total number of bullets across all categories."""
    return sum(len(bullets) for bullets in notes.values())
