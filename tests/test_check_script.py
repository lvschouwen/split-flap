"""Pins scripts/check.sh choices that parallel agent slots depend on."""
import re
from pathlib import Path

CHECK = Path(__file__).resolve().parents[1] / "scripts" / "check.sh"


def test_no_build_cache_is_shared_between_checkouts():
    # #666: a shared PLATFORMIO_BUILD_CACHE_DIR holds one SCons .sconsign
    # database, and parallel slots rename each other's temp file away.
    src = CHECK.read_text()
    assert not re.search(r"^\s*export\s+PLATFORMIO_BUILD_CACHE_DIR=", src, re.M)
    assert re.search(r"^unset PLATFORMIO_BUILD_CACHE_DIR$", src, re.M)
