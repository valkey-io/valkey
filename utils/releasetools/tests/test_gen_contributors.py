"""Unit tests for contributor collection and formatting (GitHub API mocked)."""

import gen_contributors as gc


def test_list_contributors_maps_names_dedupes_and_sorts(monkeypatch):
    # Two commits by carol, one each by alice/bob; one bot to be skipped.
    compare = {
        "commits": [
            {"author": {"login": "carol"}},
            {"author": {"login": "alice"}},
            {"author": {"login": "carol"}},  # duplicate login
            {"author": {"login": "bob"}},
            {"author": {"login": "dependabot[bot]"}},  # bot, skipped
            {"author": None},  # no author, skipped
        ]
    }
    names = {
        "alice": "Alice Anderson",
        "bob": "Bob Brown",
        "carol": None,  # no profile name -> fall back to login
    }

    def fake_api_get(url, token):
        if "/compare/" in url:
            return compare
        login = url.rsplit("/", 1)[-1]
        return {"name": names.get(login)}

    monkeypatch.setattr(gc, "_api_get", fake_api_get)

    out = gc.list_contributors("valkey-io/valkey", "v9.0.0", "HEAD", token="t")
    # Alpha-sorted by display name; carol falls back to its login.
    assert out == ["Alice Anderson @alice", "Bob Brown @bob", "carol @carol"]


def test_list_contributors_paginates(monkeypatch):
    # First page returns a full page (per_page=250) -> pager must fetch page 2.
    page1 = {"commits": [{"author": {"login": "u{}".format(i)}} for i in range(250)]}
    page2 = {"commits": [{"author": {"login": "zlast"}}]}

    calls = {"n": 0}

    def fake_api_get(url, token):
        if "/compare/" in url:
            calls["n"] += 1
            return page1 if "page=1" in url else page2
        return {"name": None}

    monkeypatch.setattr(gc, "_api_get", fake_api_get)
    out = gc.list_contributors("valkey-io/valkey", "base", "HEAD", token="t")
    assert calls["n"] == 2
    assert "zlast @zlast" in out


def test_list_contributors_falls_back_to_shortlog(monkeypatch):
    # No logins from API -> use git shortlog names.
    monkeypatch.setattr(gc, "_compare_logins", lambda *a, **k: [])
    monkeypatch.setattr(
        gc, "_git_shortlog_names", lambda base, head, repo_dir: ["Zed Zulu", "Amy Ace", "Zed Zulu"]
    )
    out = gc.list_contributors("valkey-io/valkey", "base", "HEAD", token=None)
    # Deduped and alpha-sorted, names only (no handles).
    assert out == ["Amy Ace", "Zed Zulu"]


def test_main_prints_header(monkeypatch, capsys):
    monkeypatch.setattr(gc, "list_contributors", lambda *a, **k: ["Alice A @alice"])
    rc = gc.main(["--repo", "valkey-io/valkey", "--base-ref", "v9.0.0"])
    assert rc == 0
    out = capsys.readouterr().out
    assert "### Contributors" in out
    assert "* Alice A @alice" in out
