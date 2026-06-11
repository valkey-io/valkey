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


def test_main_exit_codes(tmp_path, monkeypatch):
    _write(tmp_path, NOTES_WITH_BULLET)
    monkeypatch.chdir(tmp_path)
    monkeypatch.setenv("PR_LABELS", '["no-release-notes"]')
    monkeypatch.delenv("BASE_SHA", raising=False)
    monkeypatch.delenv("GITHUB_STEP_SUMMARY", raising=False)
    assert crn.main([]) == 0

    monkeypatch.setenv("PR_LABELS", "[]")
    assert crn.main([]) == 1
