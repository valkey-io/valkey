#!/usr/bin/env python3
"""Orchestrate a release cut: promote notes, bump version, list contributors.

Invoked by .github/workflows/prepare-release.yml. Given a target version,
release stage, urgency, and date, it:

1. promotes the ``## Unreleased`` block of 00-RELEASENOTES into a dated section
   (resetting ``## Unreleased`` to empty),
2. injects a generated ``### Contributors`` list into that new section, and
3. sets the version macros in src/version.h.

It only edits files; the workflow handles branch/commit/PR. Use ``--dry-run``
to print the would-be results without writing.
"""

from __future__ import annotations

import argparse
import datetime
import os
import subprocess
import sys
from typing import List, Optional

try:  # Allow both `python -m` and direct-script execution.
    from bump_version import set_version, version_num
    from gen_contributors import list_contributors
    from release_notes import promote
except ImportError:  # pragma: no cover - import shim
    from utils.releasetools.bump_version import set_version, version_num  # type: ignore
    from utils.releasetools.gen_contributors import list_contributors  # type: ignore
    from utils.releasetools.release_notes import promote  # type: ignore


def _last_tag(repo_dir: str) -> Optional[str]:
    """Return the most recent tag reachable from HEAD, or None."""
    try:
        return subprocess.run(
            ["git", "describe", "--tags", "--abbrev=0"],
            cwd=repo_dir,
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip() or None
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None


def _read(path: str) -> str:
    with open(path, "r", encoding="utf-8") as fh:
        return fh.read()


def _write(path: str, text: str) -> None:
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text)


def run(
    *,
    version: str,
    stage: str,
    urgency: str,
    date: Optional[str],
    repo: str,
    base_ref: Optional[str],
    token: Optional[str],
    repo_dir: str,
    notes_file: str,
    version_file: str,
    security_fixes: Optional[List[str]],
    dry_run: bool,
) -> int:
    if not date:
        date = datetime.date.today().isoformat()

    # Contributor range: explicit --base-ref, else the most recent tag.
    base = base_ref or _last_tag(repo_dir)
    contributors: List[str] = []
    if base:
        contributors = list_contributors(repo, base, "HEAD", token, repo_dir=repo_dir)
        print("Collected {} contributor(s) over {}..HEAD".format(len(contributors), base))
    else:
        print("No base ref or tag found; skipping contributor generation.", file=sys.stderr)

    notes_text = _read(os.path.join(repo_dir, notes_file))
    new_notes = promote(
        notes_text,
        version=version,
        stage=stage,
        urgency=urgency,
        date=date,
        contributors=contributors,
        security_fixes=security_fixes,
    )

    version_text = _read(os.path.join(repo_dir, version_file))
    new_version = set_version(version_text, version, stage)

    print(
        "version.h -> VALKEY_VERSION={} VALKEY_VERSION_NUM={} VALKEY_RELEASE_STAGE={}".format(
            version, version_num(version), stage.strip().lower()
        )
    )

    if dry_run:
        print("\n===== {} (dry run) =====\n".format(notes_file))
        print(new_notes)
        print("\n===== {} (dry run) =====\n".format(version_file))
        print(new_version)
        return 0

    _write(os.path.join(repo_dir, notes_file), new_notes)
    _write(os.path.join(repo_dir, version_file), new_version)
    print("Wrote {} and {}.".format(notes_file, version_file))
    return 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Prepare a Valkey release: notes + version bump.")
    parser.add_argument("--version", required=True, help="Target version, e.g. 9.1.0")
    parser.add_argument("--stage", required=True, help="Release stage: rc1..rcN or ga")
    parser.add_argument(
        "--urgency",
        required=True,
        help="Upgrade urgency: LOW, MODERATE, HIGH, CRITICAL, or SECURITY",
    )
    parser.add_argument(
        "--date", default=None, help="Release date YYYY-MM-DD (default: today)"
    )
    parser.add_argument("--repo", required=True, help="owner/name, e.g. valkey-io/valkey")
    parser.add_argument(
        "--base-ref",
        default=None,
        help="Contributor range start (default: most recent tag reachable from HEAD)",
    )
    parser.add_argument(
        "--token", default=os.environ.get("GITHUB_TOKEN"), help="GitHub token ($GITHUB_TOKEN)"
    )
    parser.add_argument("--repo-dir", default=".", help="Repository checkout dir (default: .)")
    parser.add_argument("--notes-file", default="00-RELEASENOTES")
    parser.add_argument("--version-file", default="src/version.h")
    parser.add_argument(
        "--security-fix",
        action="append",
        default=None,
        dest="security_fixes",
        help="A Security Fixes bullet (repeatable), e.g. '(CVE-2026-12345) ...'",
    )
    parser.add_argument(
        "--dry-run", action="store_true", help="Print results without writing files"
    )
    args = parser.parse_args(argv)

    try:
        return run(
            version=args.version,
            stage=args.stage,
            urgency=args.urgency,
            date=args.date,
            repo=args.repo,
            base_ref=args.base_ref,
            token=args.token,
            repo_dir=args.repo_dir,
            notes_file=args.notes_file,
            version_file=args.version_file,
            security_fixes=args.security_fixes,
            dry_run=args.dry_run,
        )
    except ValueError as exc:
        print("error: {}".format(exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
