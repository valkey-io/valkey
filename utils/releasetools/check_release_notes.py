#!/usr/bin/env python3
"""CI check: enforce release-notes labelling and Unreleased edits on PRs.

Runs from .github/workflows/release-notes-check.yml for PRs targeting the
``unstable`` branch. Two rules:

1. A PR must carry exactly one of the labels ``release-notes`` or
   ``no-release-notes``. Zero labels or both is a failure.
2. If labelled ``release-notes``, the PR must add at least one new bullet to
   the ``## Unreleased`` block of 00-RELEASENOTES (compared against its merge
   base), so a user-facing change cannot ship without a note.

Inputs come from the environment so the workflow can pass GitHub Actions
context directly:

    PR_LABELS         JSON array of label names (toJSON(... .labels.*.name))
    BASE_SHA          merge-base SHA to diff 00-RELEASENOTES against (rule 2)
    RELEASE_NOTES_FILE  path to the notes file (default: 00-RELEASENOTES)
    GITHUB_STEP_SUMMARY path the job-summary markdown is appended to (optional)

Exits 0 when both rules pass, 1 on any violation, 2 on unexpected error.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from typing import List, Optional, Tuple

try:  # Allow both `python -m` and direct-script execution.
    from release_notes import count_bullets, parse_unreleased
except ImportError:  # pragma: no cover - import shim
    from utils.releasetools.release_notes import count_bullets, parse_unreleased  # type: ignore

RELEASE_LABEL = "release-notes"
NO_RELEASE_LABEL = "no-release-notes"
DEFAULT_FILE = "00-RELEASENOTES"


def parse_labels(raw: Optional[str]) -> List[str]:
    """Parse the PR_LABELS env value (a JSON array) into a list of names."""
    if not raw or not raw.strip():
        return []
    try:
        data = json.loads(raw)
    except json.JSONDecodeError:
        # Be lenient: accept a comma-separated fallback.
        return [item.strip() for item in raw.split(",") if item.strip()]
    if isinstance(data, list):
        return [str(item).strip() for item in data if str(item).strip()]
    return []


def _git_show(ref_path: str, repo_dir: str) -> Optional[str]:
    """Return ``git show <ref:path>`` content, or None if it does not exist."""
    try:
        return subprocess.run(
            ["git", "show", ref_path],
            cwd=repo_dir,
            check=True,
            capture_output=True,
            text=True,
        ).stdout
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None


def evaluate(
    labels: List[str],
    *,
    base_sha: Optional[str],
    notes_file: str = DEFAULT_FILE,
    repo_dir: str = ".",
) -> Tuple[bool, List[str]]:
    """Apply both rules. Return ``(ok, messages)``.

    *messages* always carries human-readable lines describing the outcome
    (failures and the passing summary) for the job summary.
    """
    messages: List[str] = []
    has_release = RELEASE_LABEL in labels
    has_no_release = NO_RELEASE_LABEL in labels

    # Rule 1: exactly one of the two labels.
    if has_release and has_no_release:
        messages.append(
            "❌ PR has both `{}` and `{}` labels. Keep exactly one.".format(
                RELEASE_LABEL, NO_RELEASE_LABEL
            )
        )
        return False, messages
    if not has_release and not has_no_release:
        messages.append(
            "❌ PR is missing a release-notes decision. Add either the `{}` label "
            "(and a note under `## Unreleased` in {}) or the `{}` label.".format(
                RELEASE_LABEL, notes_file, NO_RELEASE_LABEL
            )
        )
        return False, messages

    if has_no_release:
        messages.append(
            "✅ PR is labelled `{}`; no release note required.".format(NO_RELEASE_LABEL)
        )
        return True, messages

    # Rule 2: release-notes label requires a new Unreleased bullet.
    try:
        with open(os.path.join(repo_dir, notes_file), "r", encoding="utf-8") as fh:
            head_text = fh.read()
    except OSError as exc:
        messages.append("❌ Could not read {}: {}".format(notes_file, exc))
        return False, messages

    head_count = count_bullets(parse_unreleased(head_text))

    base_count = 0
    if base_sha:
        base_text = _git_show("{}:{}".format(base_sha, notes_file), repo_dir)
        if base_text is not None:
            base_count = count_bullets(parse_unreleased(base_text))

    if head_count <= base_count:
        messages.append(
            "❌ PR is labelled `{}` but adds no new entry to the `## Unreleased` block "
            "of {} (found {} bullet(s), base had {}). Add a bullet under the matching "
            "`### Category`, e.g. `* My change by @handle (#1234)`.".format(
                RELEASE_LABEL, notes_file, head_count, base_count
            )
        )
        return False, messages

    messages.append(
        "✅ PR is labelled `{}` and adds {} new release-note bullet(s).".format(
            RELEASE_LABEL, head_count - base_count
        )
    )
    return True, messages


def _emit_summary(ok: bool, messages: List[str]) -> None:
    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if not summary_path:
        return
    lines = ["## Release notes check", ""]
    lines.extend(messages)
    try:
        with open(summary_path, "a", encoding="utf-8") as fh:
            fh.write("\n".join(lines) + "\n")
    except OSError:
        pass


def main(argv=None) -> int:
    labels = parse_labels(os.environ.get("PR_LABELS"))
    base_sha = os.environ.get("BASE_SHA") or None
    notes_file = os.environ.get("RELEASE_NOTES_FILE", DEFAULT_FILE)

    try:
        ok, messages = evaluate(labels, base_sha=base_sha, notes_file=notes_file)
    except Exception as exc:  # noqa: BLE001 - surface any unexpected failure clearly
        print("error: {}".format(exc), file=sys.stderr)
        return 2

    for message in messages:
        print(message)
    _emit_summary(ok, messages)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
