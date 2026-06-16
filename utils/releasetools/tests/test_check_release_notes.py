"""Unit tests for the release-notes CI label/edit enforcement."""

import check_release_notes as crn


NOTES_WITH_BULLET = """preamble

## Unreleased

### Bug Fixes
* Fixed a thing by @dev (#1)

### Behavior Changes
"""

NOTES_EMPTY = """preamble

## Unreleased

### Bug Fixes

### Behavior Changes
"""

NOTES_BULLET_NO_REF = """preamble

## Unreleased

### Bug Fixes
* Fixed a thing by @dev

### Behavior Changes
"""

NOTES_BULLET_NO_AUTHOR = """preamble

## Unreleased

### Bug Fixes
* Fixed a thing (#1)

### Behavior Changes
"""


def _write(tmp_path, text):
    path = tmp_path / "00-RELEASENOTES"
    path.write_text(text)
    return path


def test_parse_labels_json_array():
    assert crn.parse_labels('["release-notes", "bug"]') == ["release-notes", "bug"]


def test_parse_labels_empty_and_comma_fallback():
    assert crn.parse_labels("") == []
    assert crn.parse_labels(None) == []
    assert crn.parse_labels("release-notes, bug") == ["release-notes", "bug"]


def test_no_label_fails(tmp_path):
    ok, messages = crn.evaluate([], base_sha=None, repo_dir=str(tmp_path))
    assert not ok
    assert any("missing a release-notes decision" in m for m in messages)


def test_both_labels_fail(tmp_path):
    ok, messages = crn.evaluate(
        ["release-notes", "no-release-notes"], base_sha=None, repo_dir=str(tmp_path)
    )
    assert not ok
    assert any("both" in m for m in messages)


def test_no_release_notes_label_passes(tmp_path):
    ok, messages = crn.evaluate(["no-release-notes"], base_sha=None, repo_dir=str(tmp_path))
    assert ok
    assert any("no release note required" in m for m in messages)


def test_no_release_notes_with_net_new_bullet_fails(tmp_path, monkeypatch):
    # Label claims no note, but the PR adds a bullet the base did not have.
    _write(tmp_path, NOTES_WITH_BULLET)
    monkeypatch.setattr(crn, "_git_show", lambda ref, repo_dir: NOTES_EMPTY)
    ok, messages = crn.evaluate(
        ["no-release-notes"], base_sha="abc123", repo_dir=str(tmp_path)
    )
    assert not ok
    assert any("adds 1 new entry" in m for m in messages)


def test_no_release_notes_with_carried_over_bullet_passes(tmp_path, monkeypatch):
    # The bullet was already in the base, so this PR added nothing -> pass.
    _write(tmp_path, NOTES_WITH_BULLET)
    monkeypatch.setattr(crn, "_git_show", lambda ref, repo_dir: NOTES_WITH_BULLET)
    ok, messages = crn.evaluate(
        ["no-release-notes"], base_sha="abc123", repo_dir=str(tmp_path)
    )
    assert ok
    assert any("no release note required" in m for m in messages)


def test_release_notes_label_with_bullet_passes(tmp_path):
    _write(tmp_path, NOTES_WITH_BULLET)
    ok, messages = crn.evaluate(["release-notes"], base_sha=None, repo_dir=str(tmp_path))
    assert ok
    assert any("adds 1 new release-note" in m for m in messages)


def test_release_notes_label_without_bullet_fails(tmp_path):
    _write(tmp_path, NOTES_EMPTY)
    ok, messages = crn.evaluate(["release-notes"], base_sha=None, repo_dir=str(tmp_path))
    assert not ok
    assert any("adds no new entry" in m for m in messages)


def test_release_notes_missing_file_fails(tmp_path):
    ok, messages = crn.evaluate(["release-notes"], base_sha=None, repo_dir=str(tmp_path))
    assert not ok
    assert any("Could not read" in m for m in messages)


def test_base_count_blocks_when_no_net_new(tmp_path, monkeypatch):
    # Head has one bullet; base (via git show) also has one -> no net-new -> fail.
    _write(tmp_path, NOTES_WITH_BULLET)
    monkeypatch.setattr(crn, "_git_show", lambda ref, repo_dir: NOTES_WITH_BULLET)
    ok, messages = crn.evaluate(
        ["release-notes"], base_sha="abc123", repo_dir=str(tmp_path)
    )
    assert not ok
    assert any("adds no new entry" in m for m in messages)


def test_net_new_against_base_passes(tmp_path, monkeypatch):
    # Head has one bullet; base had none -> net-new -> pass.
    _write(tmp_path, NOTES_WITH_BULLET)
    monkeypatch.setattr(crn, "_git_show", lambda ref, repo_dir: NOTES_EMPTY)
    ok, messages = crn.evaluate(
        ["release-notes"], base_sha="abc123", repo_dir=str(tmp_path)
    )
    assert ok


def test_suggests_pr_ref_for_new_bullet_without_one(tmp_path):
    _write(tmp_path, NOTES_BULLET_NO_REF)
    ok, messages = crn.evaluate(
        ["release-notes"], base_sha=None, repo_dir=str(tmp_path), pr_number="1234"
    )
    assert ok
    joined = "\n".join(messages)
    assert "Suggested edit" in joined
    assert "* Fixed a thing by @dev (#1234)" in joined


def test_no_suggestion_when_bullet_already_has_ref(tmp_path):
    _write(tmp_path, NOTES_WITH_BULLET)  # bullet already ends with (#1)
    ok, messages = crn.evaluate(
        ["release-notes"], base_sha=None, repo_dir=str(tmp_path), pr_number="1234"
    )
    assert ok
    assert not any("Suggested edit" in m for m in messages)


def test_no_suggestion_without_pr_number(tmp_path):
    _write(tmp_path, NOTES_BULLET_NO_REF)
    ok, messages = crn.evaluate(
        ["release-notes"], base_sha=None, repo_dir=str(tmp_path), pr_number=None
    )
    assert ok
    assert not any("Suggested edit" in m for m in messages)


def test_suggestion_only_targets_net_new_bullet(tmp_path, monkeypatch):
    # Base already had the @dev bullet; head adds a second, unreferenced one.
    head = NOTES_BULLET_NO_REF.replace(
        "* Fixed a thing by @dev\n",
        "* Fixed a thing by @dev\n* Added a flag by @newdev\n",
    )
    _write(tmp_path, head)
    monkeypatch.setattr(crn, "_git_show", lambda ref, repo_dir: NOTES_BULLET_NO_REF)
    ok, messages = crn.evaluate(
        ["release-notes"], base_sha="abc123", repo_dir=str(tmp_path), pr_number="1234"
    )
    assert ok
    joined = "\n".join(messages)
    # Only the net-new bullet is suggested, not the carried-over one.
    assert "* Added a flag by @newdev (#1234)" in joined
    assert "* Fixed a thing by @dev (#1234)" not in joined


def test_suggests_author_for_new_bullet_without_one(tmp_path):
    _write(tmp_path, NOTES_BULLET_NO_AUTHOR)
    ok, messages = crn.evaluate(
        ["release-notes"], base_sha=None, repo_dir=str(tmp_path), pr_author="dev"
    )
    assert ok
    joined = "\n".join(messages)
    assert "Suggested edit" in joined
    # Canonical order: attribution goes before the existing (#N) reference.
    assert "* Fixed a thing by @dev (#1)" in joined


def test_suggests_author_when_no_pr_ref_present(tmp_path):
    notes = NOTES_BULLET_NO_AUTHOR.replace("* Fixed a thing (#1)", "* Fixed a thing")
    _write(tmp_path, notes)
    ok, messages = crn.evaluate(
        ["release-notes"], base_sha=None, repo_dir=str(tmp_path), pr_author="dev"
    )
    assert ok
    assert any("* Fixed a thing by @dev" in m for m in messages)


def test_no_author_suggestion_when_bullet_already_has_one(tmp_path):
    _write(tmp_path, NOTES_BULLET_NO_REF)  # bullet already says "by @dev"
    ok, messages = crn.evaluate(
        ["release-notes"], base_sha=None, repo_dir=str(tmp_path), pr_author="other"
    )
    assert ok
    assert not any("@other" in m for m in messages)


def test_no_author_suggestion_without_pr_author(tmp_path):
    _write(tmp_path, NOTES_BULLET_NO_AUTHOR)
    ok, messages = crn.evaluate(
        ["release-notes"], base_sha=None, repo_dir=str(tmp_path), pr_author=None
    )
    assert ok
    assert not any("credits the contributor" in m for m in messages)


def test_author_suggestion_only_targets_net_new_bullet(tmp_path, monkeypatch):
    # Base already had an unattributed bullet; head adds a second one.
    head = NOTES_BULLET_NO_AUTHOR.replace(
        "* Fixed a thing (#1)\n",
        "* Fixed a thing (#1)\n* Added a flag (#2)\n",
    )
    _write(tmp_path, head)
    monkeypatch.setattr(crn, "_git_show", lambda ref, repo_dir: NOTES_BULLET_NO_AUTHOR)
    ok, messages = crn.evaluate(
        ["release-notes"], base_sha="abc123", repo_dir=str(tmp_path), pr_author="dev"
    )
    assert ok
    joined = "\n".join(messages)
    assert "* Added a flag by @dev (#2)" in joined
    assert "* Fixed a thing by @dev (#1)" not in joined


def test_main_exit_codes(tmp_path, monkeypatch):
    _write(tmp_path, NOTES_WITH_BULLET)
    monkeypatch.chdir(tmp_path)
    monkeypatch.setenv("PR_LABELS", '["no-release-notes"]')
    monkeypatch.delenv("BASE_SHA", raising=False)
    monkeypatch.delenv("GITHUB_STEP_SUMMARY", raising=False)
    assert crn.main([]) == 0

    monkeypatch.setenv("PR_LABELS", "[]")
    assert crn.main([]) == 1
