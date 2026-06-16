"""Unit tests for bump_version macro rewriting and hex encoding."""

import bump_version as bv
import pytest


VERSION_H = """/* header */
#define SERVER_NAME "valkey"
#define SERVER_TITLE "Valkey"
#define VALKEY_VERSION "255.255.255"
#define VALKEY_VERSION_NUM 0x00ffffff
#define VALKEY_RELEASE_STAGE "dev"

#define REDIS_VERSION "7.2.4"
#define REDIS_VERSION_NUM 0x00070204
"""


def test_version_num_encoding():
    assert bv.version_num("9.1.0") == "0x00090100"
    assert bv.version_num("7.2.4") == "0x00070204"
    assert bv.version_num("255.255.255") == "0x00ffffff"
    assert bv.version_num("0.0.0") == "0x00000000"
    assert bv.version_num("16.10.3") == "0x00100a03"


def test_set_version_rewrites_three_macros():
    out = bv.set_version(VERSION_H, "9.1.0", "ga")
    assert '#define VALKEY_VERSION "9.1.0"' in out
    assert "#define VALKEY_VERSION_NUM 0x00090100" in out
    assert '#define VALKEY_RELEASE_STAGE "ga"' in out


def test_set_version_leaves_redis_macros_untouched():
    out = bv.set_version(VERSION_H, "9.1.0", "rc1")
    assert '#define REDIS_VERSION "7.2.4"' in out
    assert "#define REDIS_VERSION_NUM 0x00070204" in out
    assert '#define SERVER_NAME "valkey"' in out


def test_set_version_accepts_rc_and_normalizes_case():
    out = bv.set_version(VERSION_H, "9.1.0", "RC3")
    assert '#define VALKEY_RELEASE_STAGE "rc3"' in out


def test_set_version_rejects_bad_version_and_stage():
    with pytest.raises(ValueError):
        bv.set_version(VERSION_H, "9.1", "ga")
    with pytest.raises(ValueError):
        bv.set_version(VERSION_H, "9.1.256", "ga")
    with pytest.raises(ValueError):
        bv.set_version(VERSION_H, "9.1.0", "beta")


def test_set_version_raises_when_macro_missing():
    with pytest.raises(ValueError):
        bv.set_version('#define SERVER_NAME "valkey"\n', "9.1.0", "ga")


def test_main_writes_file(tmp_path):
    path = tmp_path / "version.h"
    path.write_text(VERSION_H)
    rc = bv.main(["--version", "9.1.0", "--stage", "ga", "--file", str(path)])
    assert rc == 0
    text = path.read_text()
    assert '#define VALKEY_VERSION "9.1.0"' in text
    assert "0x00090100" in text


def test_main_returns_error_on_bad_input(tmp_path, capsys):
    path = tmp_path / "version.h"
    path.write_text(VERSION_H)
    rc = bv.main(["--version", "bogus", "--stage", "ga", "--file", str(path)])
    assert rc == 2
    assert "error:" in capsys.readouterr().err
