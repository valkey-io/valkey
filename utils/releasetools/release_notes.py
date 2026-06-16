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

import datetime
import re
from collections import OrderedDict
from typing import Dict, List, Optional, Sequence

# Canonical category order. Contributors append under these headers in the
# "## Unreleased" block; dated sections render them in this same order.
CATEGORIES: List[str] = [
    "Behavior Changes",
    "New Features and Enhanced Behavior",
    "Performance and Efficiency Improvements",
    "Bug Fixes",
    "Command and API Updates",
    "Module API Changes",
    "Observability and Logging",
    "Build and Tooling",
]

# Security fixes are not seeded in the unstable block — they are added at
# promotion time from manually supplied CVE entries — but when present they
# render first, ahead of the canonical categories.
SECURITY_CATEGORY = "Security Fixes"

UNRELEASED_HEADER = "## Unreleased"

UNRELEASED_COMMENT = """<!--
Contributors: if your change is user-facing, add the `release-notes` label to your
PR and append a bullet under the matching category below, in the form:

    * <human-readable description> by @<your-github-handle> (#<PR number>)

If your change is not user-facing, add the `no-release-notes` label instead. A CI
check requires exactly one of these two labels, and a note here when `release-notes`
is set. The `.github/workflows/prepare-release.yml` workflow promotes this block into
a dated release section when a release is cut, so keep entries user-readable.
-->"""

# Upgrade urgency legend rendered at the top of a release-branch notes file.
URGENCY_LEGEND = """Upgrade urgency levels:

| Level    | Meaning                                                             |
|----------|---------------------------------------------------------------------|
| LOW      | No need to upgrade unless there are new features you want to use.   |
| MODERATE | Program an upgrade of the server, but it's not urgent.              |
| HIGH     | There is a critical bug that may affect a subset of users. Upgrade! |
| CRITICAL | There is a critical bug affecting MOST USERS. Upgrade ASAP.         |
| SECURITY | There are security fixes in the release.                            |"""

VALID_URGENCIES = ("LOW", "MODERATE", "HIGH", "CRITICAL", "SECURITY")

_BULLET_RE = re.compile(r"^\s*[*-]\s+\S")
_CATEGORY_RE = re.compile(r"^###\s+(.*\S)\s*$")
_H2_RE = re.compile(r"^##\s+\S")
_DATED_SECTION_RE = re.compile(r"^Valkey\s+\d+\.\d+\.\d+", re.MULTILINE)
_VERSION_RE = re.compile(r"^(\d+)\.(\d+)\.(\d+)$")
_RC_STAGE_RE = re.compile(r"^rc(\d+)$")

_ORDINALS = [
    "zeroth", "first", "second", "third", "fourth", "fifth", "sixth",
    "seventh", "eighth", "ninth", "tenth", "eleventh", "twelfth",
]


def parse_version(version: str) -> "tuple[int, int, int]":
    """Split ``"M.m.p"`` into integer ``(major, minor, patch)``.

    Each component must be an integer in the inclusive range 0-255 so it fits
    a single byte of ``VALKEY_VERSION_NUM`` (see bump_version.py).
    """
    match = _VERSION_RE.match(version.strip())
    if not match:
        raise ValueError(
            "version must be in the form MAJOR.MINOR.PATCH (e.g. 9.1.0), got {!r}".format(version)
        )
    parts = tuple(int(p) for p in match.groups())
    for component, value in zip(("major", "minor", "patch"), parts):
        if not 0 <= value <= 255:
            raise ValueError(
                "{} version {} is out of range 0-255".format(component, value)
            )
    return parts  # type: ignore[return-value]


def ordinal(n: int) -> str:
    """Return a small ordinal word ("first", "second", ...) or "Nth" fallback."""
    if 0 <= n < len(_ORDINALS):
        return _ORDINALS[n]
    return "{}th".format(n)


def parse_unreleased(text: str) -> "OrderedDict[str, List[str]]":
    """Extract the "## Unreleased" block as an ordered ``category -> bullets`` map.

    Only the region between the ``## Unreleased`` header and the next level-2
    header (or end of file) is considered. HTML comments and blank lines are
    skipped. Categories are returned in the order they appear; categories that
    appear in :data:`CATEGORIES` but are absent from the text are not added.
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


def _format_date(date: str) -> str:
    """Render *date* as ``"Tue 02 June 2026"``.

    Accepts an ISO ``YYYY-MM-DD`` string (reformatted) or any other string
    (returned unchanged, so callers may pass a pre-formatted display date).
    """
    try:
        parsed = datetime.date.fromisoformat(date.strip())
    except ValueError:
        return date.strip()
    return parsed.strftime("%a %d %B %Y")


def _normalize_stage(stage: str) -> str:
    s = stage.strip().lower()
    if s == "ga":
        return "ga"
    if _RC_STAGE_RE.match(s):
        return s
    raise ValueError("release stage must be 'ga' or 'rcN' (e.g. rc1), got {!r}".format(stage))


def render_header(major: int, minor: int) -> str:
    """Render the file title and urgency legend for a ``M.m`` release line."""
    title = "Valkey {}.{} release notes".format(major, minor)
    underline = "=" * len(title)
    return "{}\n{}\n\n{}".format(title, underline, URGENCY_LEGEND)


def _stage_heading(version: str, stage: str) -> str:
    if stage == "ga":
        return "Valkey {} GA".format(version)
    return "Valkey {}-{}".format(version, stage)


def _urgency_sentence(version: str, stage: str, urgency: str) -> str:
    major, minor, patch = parse_version(version)
    if stage == "ga":
        which = ordinal(patch + 1)  # M.m.0 is the first stable release of M.m
        return (
            "Upgrade urgency {}: This is the {} stable release of Valkey {}.{}.".format(
                urgency, which, major, minor
            )
        )
    rc_num = int(_RC_STAGE_RE.match(stage).group(1))  # type: ignore[union-attr]
    which = ordinal(rc_num)
    return (
        "Upgrade urgency {}: This is the {} release candidate of Valkey {}.".format(
            urgency, which, version
        )
    )


def render_version_section(
    version: str,
    stage: str,
    urgency: str,
    date: str,
    notes: "Dict[str, List[str]]",
    contributors: Optional[Sequence[str]] = None,
    security_fixes: Optional[Sequence[str]] = None,
) -> str:
    """Render one dated release section in release-branch markdown form.

    *notes* maps category name to a list of bullet strings (already including
    the leading ``* ``). Only non-empty categories are emitted, in
    :data:`CATEGORIES` order, with any ``Security Fixes`` rendered first.
    *contributors* is a list of display strings (``"Jane Doe @jdoe"``) rendered
    under a trailing ``### Contributors`` section. *security_fixes* is an
    optional list of CVE bullet strings.
    """
    stage = _normalize_stage(stage)
    urgency = urgency.strip().upper()
    if urgency not in VALID_URGENCIES:
        raise ValueError(
            "urgency must be one of {}, got {!r}".format(", ".join(VALID_URGENCIES), urgency)
        )

    heading = "{}  -  Released {}".format(_stage_heading(version, stage), _format_date(date))
    underline = "-" * len(heading)
    out: List[str] = [heading, underline, "", _urgency_sentence(version, stage, urgency), ""]

    def emit_category(name: str, bullets: Sequence[str]) -> None:
        out.append("### {}".format(name))
        for bullet in bullets:
            bullet = bullet.strip()
            if not bullet.startswith(("* ", "- ")):
                bullet = "* " + bullet
            out.append(bullet)
        out.append("")

    if security_fixes:
        emit_category(SECURITY_CATEGORY, list(security_fixes))
    # Render Security Fixes that may have been carried in *notes* too.
    if notes.get(SECURITY_CATEGORY):
        emit_category(SECURITY_CATEGORY, notes[SECURITY_CATEGORY])
    for category in CATEGORIES:
        bullets = notes.get(category)
        if bullets:
            emit_category(category, bullets)

    if contributors:
        out.append("### Contributors")
        for name in contributors:
            name = name.strip()
            if not name.startswith(("* ", "- ")):
                name = "* " + name
            out.append(name)
        out.append("")

    return "\n".join(out).rstrip() + "\n"


def render_empty_unreleased() -> str:
    """Render the canonical empty ``## Unreleased`` block."""
    parts = [UNRELEASED_HEADER, "", UNRELEASED_COMMENT, ""]
    for category in CATEGORIES:
        parts.append("### {}".format(category))
        parts.append("")
    return "\n".join(parts).rstrip() + "\n"


def reset_unreleased(text: str) -> str:
    """Return *text* with the ``## Unreleased`` block reset to empty categories.

    Everything before ``## Unreleased`` is preserved verbatim; the block itself
    is replaced with :func:`render_empty_unreleased`.
    """
    idx = text.find("\n" + UNRELEASED_HEADER)
    if idx == -1:
        if text.startswith(UNRELEASED_HEADER):
            return render_empty_unreleased()
        # No Unreleased block at all — append a fresh one.
        return text.rstrip() + "\n\n" + render_empty_unreleased()
    return text[: idx + 1] + render_empty_unreleased()


def _existing_dated_sections(before_unreleased: str) -> str:
    """Return the dated-section region of the text preceding ``## Unreleased``."""
    match = _DATED_SECTION_RE.search(before_unreleased)
    if not match:
        return ""
    return before_unreleased[match.start():].strip()


def promote(
    text: str,
    *,
    version: str,
    stage: str,
    urgency: str,
    date: str,
    contributors: Optional[Sequence[str]] = None,
    security_fixes: Optional[Sequence[str]] = None,
) -> str:
    """Promote the ``## Unreleased`` block into a new dated release section.

    Returns the full rewritten file: a regenerated title + urgency legend, the
    new dated section first, any previously dated sections after it, and an
    emptied ``## Unreleased`` block at the end.
    """
    major, minor, _ = parse_version(version)
    notes = parse_unreleased(text)
    dated = render_version_section(
        version, stage, urgency, date, notes, contributors, security_fixes
    )

    before_unreleased = text.split("\n" + UNRELEASED_HEADER, 1)[0]
    existing = _existing_dated_sections(before_unreleased)

    parts: List[str] = [render_header(major, minor), "", dated.rstrip(), ""]
    if existing:
        parts += [existing, ""]
    parts.append(render_empty_unreleased().rstrip())
    return "\n".join(parts).rstrip() + "\n"
