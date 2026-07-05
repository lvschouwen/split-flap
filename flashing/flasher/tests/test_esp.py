import sys
from unittest.mock import Mock

import pytest
from flasher.esp import ImageError, flash_master, validate_image_header


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


def test_flash_master_bad_image_never_invokes_esptool(tmp_path, monkeypatch):
    run_mock = Mock()
    monkeypatch.setattr("subprocess.run", run_mock)
    bin_path = tmp_path / "bad.bin"
    bin_path.write_bytes(hdr(size_freq=0x40))  # 4MB size nibble — rejected

    with pytest.raises(ImageError, match="1 MB"):
        flash_master("/dev/ttyUSB0", str(bin_path))

    run_mock.assert_not_called()


def test_flash_master_dev_path_invokes_esptool_with_exact_args(tmp_path, monkeypatch):
    run_mock = Mock()
    monkeypatch.setattr("subprocess.run", run_mock)
    bin_path = tmp_path / "good.bin"
    bin_path.write_bytes(hdr())
    port = "/dev/ttyUSB0"

    warnings = flash_master(port, str(bin_path))

    assert warnings == []
    run_mock.assert_called_once_with(
        [
            sys.executable, "-m", "esptool",
            "--chip", "esp8266", "--port", port, "--baud", "115200",
            "--before", "no_reset", "--after", "no_reset",
            "write_flash", "0x0", str(bin_path),
        ],
        check=True,
    )


def test_flash_master_valid_header_qio_mode_returns_one_warning(tmp_path, monkeypatch):
    monkeypatch.setattr("subprocess.run", Mock())
    bin_path = tmp_path / "qio.bin"
    bin_path.write_bytes(hdr(mode=0x00))  # QIO, not DOUT

    warnings = flash_master("/dev/ttyUSB0", str(bin_path))

    assert len(warnings) == 1 and "DOUT" in warnings[0]


def test_flash_master_frozen_path_wraps_systemexit_as_runtimeerror(tmp_path, monkeypatch):
    monkeypatch.setattr(sys, "frozen", True, raising=False)
    fake_esptool = Mock()
    fake_esptool.main = Mock(side_effect=SystemExit(2))
    monkeypatch.setitem(sys.modules, "esptool", fake_esptool)
    bin_path = tmp_path / "good.bin"
    bin_path.write_bytes(hdr())

    with pytest.raises(RuntimeError, match="esptool exited with 2"):
        flash_master("/dev/ttyUSB0", str(bin_path))
