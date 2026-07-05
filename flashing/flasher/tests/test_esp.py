import pytest
from flasher.esp import ImageError, validate_image_header


def hdr(magic=0xE9, mode=0x03, size_freq=0x20):
    return bytes([magic, 0x02, mode, size_freq])


def test_valid_1mb_dout_header_no_warnings():
    assert validate_image_header(hdr()) == []


def test_bad_magic_fatal():
    with pytest.raises(ImageError, match="magic"):
        validate_image_header(hdr(magic=0xEA))


def test_4mb_header_fatal():
    # size nibble 0x4 = 4MB — would trip the OTA flash-config-mismatch path later
    with pytest.raises(ImageError, match="1 MB"):
        validate_image_header(hdr(size_freq=0x40))


def test_512kb_header_fatal():
    with pytest.raises(ImageError, match="1 MB"):
        validate_image_header(hdr(size_freq=0x00))


def test_non_dout_mode_is_warning_not_fatal():
    warnings = validate_image_header(hdr(mode=0x00))  # QIO
    assert len(warnings) == 1 and "DOUT" in warnings[0]


def test_truncated_header_fatal():
    with pytest.raises(ImageError, match="short"):
        validate_image_header(b"\xe9\x02")
