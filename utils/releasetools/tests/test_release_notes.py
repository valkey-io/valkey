"""Unit tests for release_notes "## Unreleased" parsing."""

import release_notes as rn


SAMPLE = """Hello! placeholder text.

## Unreleased

<!-- a comment
spanning lines -->

### Behavior Changes
* Changed the default of foo by @alice (#100)

### New Features and Enhanced Behavior

### Bug Fixes
* Fixed a crash in bar by @bob (#101)
* Fixed a leak in baz by @carol (#102)
"""


def test_parse_unreleased_collects_bullets_by_category():
    notes = rn.parse_unreleased(SAMPLE)
    assert notes["Behavior Changes"] == ["* Changed the default of foo by @alice (#100)"]
    assert notes["Bug Fixes"] == [
        "* Fixed a crash in bar by @bob (#101)",
        "* Fixed a leak in baz by @carol (#102)",
    ]
    # Empty category is present but has no bullets.
    assert notes["New Features and Enhanced Behavior"] == []


def test_parse_unreleased_skips_html_comments():
    notes = rn.parse_unreleased(SAMPLE)
    # The multi-line comment must not leak into any category.
    for bullets in notes.values():
        for bullet in bullets:
            assert "comment" not in bullet


def test_parse_unreleased_missing_block_returns_empty():
    assert rn.parse_unreleased("no block here") == {}


def test_count_and_empty_helpers():
    notes = rn.parse_unreleased(SAMPLE)
    assert rn.count_bullets(notes) == 3
    assert not rn.is_unreleased_empty(notes)
    assert rn.is_unreleased_empty({"Bug Fixes": []})
