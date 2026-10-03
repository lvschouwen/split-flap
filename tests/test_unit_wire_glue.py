"""Source gates for the unit I2C glue that no native test can reach (#502, #512).

The pure decisions (reply slot semantics, guard acceptance) are unit-tested, but
the code that CALLS them lives in .ino/.cpp hardware glue. These gates pin the
call sites, so removing one cannot leave every suite green.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
V2 = ROOT / "firmware" / "v2"
UNIT_PROTOCOL = V2 / "Unit" / "UnitI2CProtocol.ino"
WIRE_CONTRACT = V2 / "shared" / "UnitWireContract.h"
MASTER_BUSES = [V2 / "Master" / "UnitBus.cpp", V2 / "FollowerEsp01" / "FollowerBus.cpp"]


def _strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def _function_body(text, signature):
    start = text.index(signature)
    i = text.index("{", start)
    depth = 0
    for j in range(i, len(text)):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return text[i + 1:j]
    raise AssertionError(f"unterminated body for {signature}")


def _noarg_mutations():
    body = _function_body(_strip_comments(WIRE_CONTRACT.read_text()),
                          "inline bool sfpIsNoArgMutation(")
    ops = re.findall(r"case\s+(SFP_CMD_\w+)\s*:", body)
    assert len(ops) >= 7, ops
    return ops


def test_every_master_write_clears_the_pending_reply_first():
    body = _function_body(_strip_comments(UNIT_PROTOCOL.read_text()),
                          "void receiveLetter(int numBytes)")
    clear = body.index("pendingReply = REPLY_NONE;")
    first_return = body.index("return")
    assert clear < first_return, "the reply slot must be cleared before any early return"


def test_request_event_consumes_the_reply_slot():
    body = _function_body(_strip_comments(UNIT_PROTOCOL.read_text()),
                          "void requestEvent()")
    take = body.index("uint8_t reply = pendingReply;")
    clear = body.index("pendingReply = REPLY_NONE;")
    assert take < clear < body.index("if (reply =="), "take, clear, then dispatch"
    assert "pendingReply ==" not in body, "dispatch on the local copy, not the slot"


def test_unit_checks_the_guard_before_dispatching_an_opcode():
    body = _function_body(_strip_comments(UNIT_PROTOCOL.read_text()),
                          "void receiveLetter(int numBytes)")
    check = body.index("if (!noArgMutationAccepted(opcode, extraLen, guard, strict))")
    dispatch = body.index("switch (opcode)")
    assert check < dispatch
    refused = body[check:dispatch]
    assert "badCommandCount++" in refused and "return;" in refused
    assert "jogDecode(pay, extraLen, strict, steps)" in body
    assert "UNIT_GATE_STRICT_OPCODES" in body


def test_masters_never_send_a_guarded_opcode_bare():
    for path in MASTER_BUSES:
        src = _strip_comments(path.read_text())
        for op in _noarg_mutations():
            bare = re.findall(r"Wire\.write\(\s*\(uint8_t\)\s*%s\s*\)" % op, src)
            guarded = re.findall(r"writeGuardedOpcode\(\s*%s\s*\)" % op, src)
            if op == "SFP_CMD_ENTER_BOOTLOADER":
                # Fixed-forever one-byte form; a unit in twiboot ACKs one byte.
                assert bare and not guarded, f"{path.name}: {op} must stay bare"
            else:
                assert not bare, f"{path.name}: {op} is sent without its guard byte"
        body = _function_body(src, "static void writeGuardedOpcode(uint8_t opcode)")
        assert "Wire.write(opcode);" in body and "Wire.write(noArgGuardByte(opcode));" in body


def test_masters_send_jog_with_its_complement():
    for path in MASTER_BUSES:
        src = _strip_comments(path.read_text())
        after = src[src.index("SFP_CMD_JOG"):][:300]
        assert "jogEncode(" in after and "Wire.write(jog, JOG_PAYLOAD_LEN);" in after, path.name
