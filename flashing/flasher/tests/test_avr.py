from flasher.avr import (
    EXPECTED_SIGNATURE, arduinoisp_args, fuse_read_args, fuse_write_args,
    fuses_ok, icsp_flash_args, parse_fuses, parse_signature, probe_args,
)


def test_icsp_args_use_stk500v1_at_19200():
    args = icsp_flash_args("avrdude", "avrdude.conf", "COM4", "twiboot.hex")
    joined = " ".join(args)
    assert "-c stk500v1" in joined and "-b 19200" in joined and "-p m328p" in joined
    assert "flash:w:twiboot.hex:i" in joined


def test_arduinoisp_args_use_serial_bootloader():
    args = arduinoisp_args("avrdude", "avrdude.conf", "COM4", 115200, "ArduinoISP.hex")
    joined = " ".join(args)
    assert "-c arduino" in joined and "-b 115200" in joined
    assert "flash:w:ArduinoISP.hex:i" in joined


def test_probe_args_have_no_write_operations():
    joined = " ".join(probe_args("avrdude", "avrdude.conf", "COM4"))
    assert ":w:" not in joined


def test_fuse_write_args_match_retired_bat():
    joined = " ".join(fuse_write_args("avrdude", "avrdude.conf", "COM4"))
    assert "lfuse:w:0xff:m" in joined
    assert "hfuse:w:0xdc:m" in joined
    assert "efuse:w:0xfd:m" in joined


def test_parse_signature():
    out = "avrdude: Device signature = 0x1e950f (probably m328p)"
    assert parse_signature(out) == "1e950f"
    assert parse_signature("avrdude: Device signature = 0xffffff") == "ffffff"
    assert parse_signature("no signature here") is None


def test_parse_fuses_hex_lines():
    # avrdude -U lfuse:r:-:h ... prints one value per line on stdout
    assert parse_fuses("0xff\n0xdc\n0xfd\n") == (0xFF, 0xDC, 0xFD)
    assert parse_fuses("0xff\n0xdc\n0x5\n") == (0xFF, 0xDC, 0x05)
    assert parse_fuses("garbage") is None


def test_fuses_ok_masks_efuse_undefined_bits():
    assert fuses_ok(0xFF, 0xDC, 0xFD)
    assert fuses_ok(0xFF, 0xDC, 0x05)  # same low 3 bits — m328p upper bits undefined
    assert not fuses_ok(0xFF, 0xDC, 0x04)
    assert not fuses_ok(0xFE, 0xDC, 0xFD)
    assert not fuses_ok(0xFF, 0xDA, 0xFD)


def test_signature_constant():
    assert EXPECTED_SIGNATURE == "1e950f"
