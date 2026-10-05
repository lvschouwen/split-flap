"""The wall link schema means the same to stock protobuf as to the boards.

The native test (Master/test/test_wall_link) asserts the bytes this build's
nanopb code produces for a handful of messages (its VEC lines). Here the stock
Python protobuf library, generating its own code from the same .proto, decodes
those bytes, checks the values, and encodes them back to the same bytes. The
host-side stand-ins (fake row, fake master) use this generated module.
"""
import importlib
import re
import subprocess
import sys
from pathlib import Path

import pytest

pytest.importorskip("grpc_tools", reason="needs grpcio-tools to generate the Python protobuf module")

LINK = Path(__file__).resolve().parents[1]
NATIVE_TEST = (LINK.parent / "Master/test/test_wall_link/test_wall_link.cpp").read_text()
VEC = {name: bytes.fromhex("".join(re.findall(r'"([0-9a-f]*)"', parts)))
       for name, parts in re.findall(r'VEC\("(\w+)",((?:\s*"[0-9a-f]*")+)\)', NATIVE_TEST)}


@pytest.fixture(scope="module")
def pb(tmp_path_factory):
    out = tmp_path_factory.mktemp("wall_link_pb")
    subprocess.run([sys.executable, "-m", "grpc_tools.protoc", f"--proto_path={LINK}",
                    f"--python_out={out}", "wall_link.proto"], check=True)
    sys.path.insert(0, str(out))
    try:
        yield importlib.import_module("wall_link_pb2")
    finally:
        sys.path.remove(str(out))


sys.path.insert(0, str(LINK))
import wall_link_io  # noqa: E402

frame = wall_link_io.frame


def delimited(data: bytes) -> bytes:
    """One length-delimited message: returns its body, requires nothing after it."""
    (body,) = wall_link_io.Splitter().feed(data)
    assert frame_len(body) == len(data)
    return body


def frame_len(body: bytes) -> int:
    return len(body) + (1 if len(body) < 128 else 2)


def test_splitter_reassembles_messages_fed_one_byte_at_a_time():
    stream = VEC["hello"] + VEC["ping"] + VEC["status"]
    splitter, bodies = wall_link_io.Splitter(), []
    for i in range(len(stream)):
        bodies += splitter.feed(stream[i:i + 1])
    assert len(bodies) == 3 and splitter.buf == b""
    with pytest.raises(ValueError):
        wall_link_io.Splitter().feed(bytes([0xFF, 0x7F]))  # 16383 bytes


def test_the_native_test_carries_the_vectors_this_file_checks():
    assert set(VEC) == {"hello", "show", "update", "update_state", "op", "status", "ping", "pong"}


def test_hello(pb):
    m = pb.ToMaster.FromString(delimited(VEC["hello"]))
    assert m.WhichOneof("body") == "hello"
    assert (m.hello.protocol, m.hello.id, m.hello.rev) == (1, "split-flap-261bb6", "3f1a516")
    assert (m.hello.boot_id, m.hello.rescue, m.hello.width) == (0xDEADBEEF, True, 5)
    assert frame(m) == VEC["hello"]


def test_show(pb):
    m = pb.ToRow.FromString(delimited(VEC["show"]))
    assert m.WhichOneof("body") == "show"
    assert (m.show.render_id, m.show.commit_at_ms, m.show.speed, m.show.text) == (
        94, 1791223510400, 80, "19:34")
    assert frame(m) == VEC["show"]


def test_update_and_op(pb):
    m = pb.ToRow.FromString(delimited(VEC["update"]))
    assert (m.update.rev, m.update.size, m.update.md5, m.update.packed) == (
        "3f1a516", 323047, bytes(range(16)), True)
    assert m.update.http_port == 8080
    assert frame(m) == VEC["update"]
    m = pb.ToMaster.FromString(delimited(VEC["update_state"]))
    assert m.WhichOneof("body") == "update_state"
    assert (m.update_state.rev, m.update_state.phase, m.update_state.reason,
            m.update_state.detail) == ("3f1a516", pb.UPDATE_FAILED, pb.UPDATE_HTTP_STATUS, 404)
    assert frame(m) == VEC["update_state"]
    m = pb.ToRow.FromString(delimited(VEC["op"]))
    assert (m.op.op_id, m.op.opcode, m.op.address, m.op.arg) == (
        0xA1B20007, pb.OPC_SET_OFFSET, 6, -500)
    assert frame(m) == VEC["op"]


def test_status_carries_a_negative_signal(pb):
    m = pb.ToMaster.FromString(delimited(VEC["status"]))
    assert (m.status.up_s, m.status.heap, m.status.min_heap) == (5429, 27312, 17464)
    assert (m.status.rssi, m.status.busy, m.status.heap2) == (-58, True, 19824)
    assert frame(m) == VEC["status"]


def test_empty_messages_still_name_their_kind(pb):
    assert pb.ToRow.FromString(delimited(VEC["ping"])).WhichOneof("body") == "ping"
    assert pb.ToMaster.FromString(delimited(VEC["pong"])).WhichOneof("body") == "pong"


def test_every_string_and_bytes_field_has_a_fixed_size(pb):
    """nanopb only keeps a field on the stack when the options file sizes it;
    a missed one would silently become a callback field on the boards."""
    options = (LINK / "wall_link.options").read_text()
    sized = set(re.findall(r"^wl\.(\w+\.\w+)\s+max_size:", options, re.M))
    unsized = []
    for message in pb.DESCRIPTOR.message_types_by_name.values():
        for field in message.fields:
            if field.type in (field.TYPE_STRING, field.TYPE_BYTES):
                name = f"{message.name}.{field.name}"
                if name not in sized:
                    unsized.append(name)
    assert unsized == []
