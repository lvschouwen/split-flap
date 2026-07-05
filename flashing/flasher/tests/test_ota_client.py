import json
from unittest.mock import patch
from flasher.ota import encode_multipart, fetch_setting


def test_multipart_is_well_formed():
    body, ctype = encode_multipart("firmware", "fw.bin", b"\x01\x02")
    boundary = ctype.split("boundary=")[1]
    assert ctype.startswith("multipart/form-data; boundary=")
    assert body.startswith(b"--" + boundary.encode())
    assert b'name="firmware"' in body
    assert b'filename="fw.bin"' in body
    assert b"\x01\x02" in body
    assert body.endswith(b"--" + boundary.encode() + b"--\r\n")


def test_fetch_setting_soft_fails_to_question_mark():
    with patch("flasher.ota.fetch_settings", return_value=None):
        assert fetch_setting("http://x", "sketchMd5") == "?"
    with patch("flasher.ota.fetch_settings", return_value={"other": 1}):
        assert fetch_setting("http://x", "sketchMd5") == "?"
    with patch("flasher.ota.fetch_settings", return_value={"sketchMd5": "abc"}):
        assert fetch_setting("http://x", "sketchMd5") == "abc"
