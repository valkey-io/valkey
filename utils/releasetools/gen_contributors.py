#!/usr/bin/env python3
"""Generate the deduplicated, alpha-sorted contributor list for a release.

Collects the GitHub authors of every commit in a ``base..head`` range and
renders them as ``* Full Name @handle`` bullets, sorted by display name. The
commit range and author logins come from the GitHub compare API; each unique
login is then resolved to a display name via the users API. When the API is
unavailable (no token / offline), it falls back to ``git shortlog`` over the
same range for names only.

Stdlib only (urllib) so it runs in the same minimal environment as the rest of
utils/.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import urllib.error
import urllib.request
from typing import List, Optional

_API_ROOT = "https://api.github.com"


def _api_get(url: str, token: Optional[str]) -> object:
    headers = {
        "Accept": "application/vnd.github+json",
        "User-Agent": "valkey-release-tools",
        "X-GitHub-Api-Version": "2022-11-28",
    }
    if token:
        headers["Authorization"] = "Bearer {}".format(token)
    req = urllib.request.Request(url, headers=headers)
    with urllib.request.urlopen(req, timeout=30) as resp:  # noqa: S310 (trusted host)
        return json.loads(resp.read().decode("utf-8"))


def _compare_logins(repo: str, base_ref: str, head_ref: str, token: Optional[str]) -> List[str]:
    """Return unique author logins for commits in ``base..head`` (compare API).

    The compare endpoint paginates commits; we walk pages until fewer than the
    page size are returned. Bot authors (login ending in ``[bot]``) are skipped.
    """
    logins: List[str] = []
    seen = set()
    page = 1
    per_page = 250
    while True:
        url = "{}/repos/{}/compare/{}...{}?per_page={}&page={}".format(
            _API_ROOT, repo, base_ref, head_ref, per_page, page
        )
        data = _api_get(url, token)
        if not isinstance(data, dict):
            break
        commits = data.get("commits") or []
        for commit in commits:
            author = commit.get("author") or {}
            login = author.get("login")
            if not login or login in seen or login.endswith("[bot]"):
                continue
            seen.add(login)
            logins.append(login)
        if len(commits) < per_page:
            break
        page += 1
    return logins


def _display_name(repo_login: str, token: Optional[str]) -> Optional[str]:
    """Resolve a login to its profile full name, or None if unavailable."""
    try:
        data = _api_get("{}/users/{}".format(_API_ROOT, repo_login), token)
    except (urllib.error.URLError, urllib.error.HTTPError, ValueError):
        return None
    if isinstance(data, dict):
        name = data.get("name")
        if name and name.strip():
            return name.strip()
    return None


def _git_shortlog_names(base_ref: str, head_ref: str, repo_dir: str) -> List[str]:
    """Fallback: author names from ``git shortlog -sn base..head`` (no handles)."""
    try:
        out = subprocess.run(
            ["git", "shortlog", "-sn", "{}..{}".format(base_ref, head_ref)],
            cwd=repo_dir,
            check=True,
            capture_output=True,
            text=True,
        ).stdout
    except (subprocess.CalledProcessError, FileNotFoundError):
        return []
    names = []
    for line in out.splitlines():
        line = line.strip()
        if not line:
            continue
        # Format: "<count>\t<name>"
        parts = line.split("\t", 1)
        if len(parts) == 2 and parts[1].strip():
            names.append(parts[1].strip())
    return names


def _sort_key(entry: str) -> str:
    """Case-insensitive sort key on the display name portion."""
    name = entry.split(" @", 1)[0]
    return name.casefold()


def list_contributors(
    repo: str,
    base_ref: str,
    head_ref: str,
    token: Optional[str] = None,
    *,
    repo_dir: str = ".",
) -> List[str]:
    """Return alpha-sorted ``"Full Name @handle"`` strings for the commit range.

    Falls back to git-shortlog names (no handles) if the compare API yields no
    logins (e.g. no token, network error, or unknown range).
    """
    try:
        logins = _compare_logins(repo, base_ref, head_ref, token)
    except (urllib.error.URLError, urllib.error.HTTPError, ValueError):
        logins = []

    entries: List[str] = []
    if logins:
        for login in logins:
            name = _display_name(login, token) or login
            entries.append("{} @{}".format(name, login))
    else:
        # Fallback path — names only, deduplicated preserving first sight.
        seen = set()
        for name in _git_shortlog_names(base_ref, head_ref, repo_dir):
            if name not in seen:
                seen.add(name)
                entries.append(name)

    entries.sort(key=_sort_key)
    return entries


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="Generate the deduplicated contributor list for a release range."
    )
    parser.add_argument("--repo", required=True, help="owner/name, e.g. valkey-io/valkey")
    parser.add_argument("--base-ref", required=True, help="Range start (e.g. last tag)")
    parser.add_argument("--head-ref", default="HEAD", help="Range end (default: HEAD)")
    parser.add_argument(
        "--repo-dir", default=".", help="Local clone dir for the git fallback (default: .)"
    )
    parser.add_argument(
        "--token",
        default=os.environ.get("GITHUB_TOKEN"),
        help="GitHub token (defaults to $GITHUB_TOKEN)",
    )
    args = parser.parse_args(argv)

    entries = list_contributors(
        args.repo, args.base_ref, args.head_ref, args.token, repo_dir=args.repo_dir
    )
    print("### Contributors")
    for entry in entries:
        print("* {}".format(entry))
    return 0


if __name__ == "__main__":
    sys.exit(main())
