"""PlatformIO pre-build patch for esp32async/ESPAsyncWebServer (#347).

The vendored request parser buffers a multipart NON-file part — both its
header line (`_temp`) and its value (`_itemValue`) — into unbounded heap
Strings, and pushes every completed field into `_params`. Because that
happens inside the parser BEFORE any route handler (so before the upload
routes' CSRF / md5 / Update.begin gate), an unauthenticated LAN client can
exhaust SRAM with one large multipart POST to /firmware/master (or the other
upload routes the app-level WebBodyLimit guard must exempt to let genuine
firmware through). A Content-Length cap cannot fix those routes — legit
firmware already dwarfs the device heap — so the bound has to live in the
parser.

This applies a minimal, idempotent source patch to the fetched library:
a per-body byte budget shared by the non-file header line and value
accumulation (truncate past the budget), plus a cap on the number of
non-file field params. Legit uploads carry a FILE part (streamed through a
fixed chunk buffer, untouched here) and no large non-file fields, so they are
unaffected. The exact-pinned version (#357) keeps the anchors stable; if an
anchor ever moves the patch FAILS LOUD rather than silently no-op'ing.

#631: a response is marked ended (`RESPONSE_END`) the moment its last piece
is read from the source, before the TCP queue has taken it. When that add
fails (`tcp_write` ERR_MEM: the queue is full of segments, which a small-MSS
peer like the ESP-01 row brings about), the piece stays in `_send_buffer`,
the next ACK finds the response finished and closes the connection: the
peer gets a clean FIN with the tail of the file missing. The patch keeps
"the source has ended" apart from "the response has ended" and ends the
response only once the buffer has gone out; it also takes an in-flight
credit only for a pass that put bytes in flight (fixed so upstream after
3.12.1), since a pass that sent nothing gets no ACK to give it back.
Upstream ESP32Async/ESPAsyncWebServer PR #497 covers the same.

One script for Master / Rescue / FollowerEsp01 (unified stack, #356), wired
in each platformio.ini via `extra_scripts = pre:../buildtools/patch_asyncweb.py`.
"""

import pathlib
import sys

# (relative path under the library src dir, anchor, replacement). Each anchor
# is an exact substring of the pinned 3.12.1 source; the replacement contains
# the SENTINEL so a second run is a no-op.
SENTINEL = "_sf347FieldBudget"

_HEADER = "ESPAsyncWebServer.h"
_REQ = "WebRequest.cpp"
_RESP_H = "WebResponseImpl.h"
_RESP = "WebResponses.cpp"

PATCHES = [
    # 1) Budget member + tunables, right after the field-value String decl.
    (
        _HEADER,
        "  String _itemValue;\n  uint8_t *_itemBuffer;",
        "  String _itemValue;\n"
        "#ifndef SF347_MULTIPART_VALUE_BUDGET\n"
        "#define SF347_MULTIPART_VALUE_BUDGET 2048  // #347: non-file multipart bytes / body\n"
        "#define SF347_MULTIPART_MAX_PARAMS 64      // #347: non-file multipart fields / body\n"
        "#endif\n"
        "  size_t _sf347FieldBudget = SF347_MULTIPART_VALUE_BUDGET;  // #347\n"
        "  uint8_t *_itemBuffer;",
    ),
    # 2) Reset the budget at the start of every multipart body.
    (
        _REQ,
        "  if (!_parsedLength) {\n    _multiParseState = EXPECT_BOUNDARY;",
        "  if (!_parsedLength) {\n"
        "    _sf347FieldBudget = SF347_MULTIPART_VALUE_BUDGET;  // #347: reset per body\n"
        "    _multiParseState = EXPECT_BOUNDARY;",
    ),
    # 3) Cap the non-file field VALUE accumulation (itemWriteByte macro).
    (
        _REQ,
        "    else                          \\\n"
        "      _itemValue += (char)(b);    \\\n"
        "  } while (0)",
        "    else if (_sf347FieldBudget) { /* #347 */ \\\n"
        "      _itemValue += (char)(b);    \\\n"
        "      _sf347FieldBudget--;        \\\n"
        "    }                             \\\n"
        "  } while (0)",
    ),
    # 4) Cap the multipart part-HEADER line accumulation (same budget).
    (
        _REQ,
        "    if ((char)data != '\\r' && (char)data != '\\n') {\n"
        "      _temp += (char)data;\n"
        "    }",
        "    if ((char)data != '\\r' && (char)data != '\\n') {\n"
        "      if (_sf347FieldBudget) { _temp += (char)data; _sf347FieldBudget--; }  // #347\n"
        "    }",
    ),
    # 5) Cap the number of non-file field params.
    (
        _REQ,
        "      if (!_itemIsFile) {\n"
        "        _params.emplace_back(_itemName, _itemValue, true);",
        "      if (!_itemIsFile) {\n"
        "        if (_params.size() < SF347_MULTIPART_MAX_PARAMS)  // #347\n"
        "        _params.emplace_back(_itemName, _itemValue, true);",
    ),
    # 6) Same cap on the file-part completion emplace (symmetry — an
    # explicit bound instead of relying on the header budget indirectly).
    (
        _REQ,
        "        _itemBufferIndex = 0;\n"
        "        _params.emplace_back(_itemName, _itemFilename, true, true, _itemSize);",
        "        _itemBufferIndex = 0;\n"
        "        if (_params.size() < SF347_MULTIPART_MAX_PARAMS)  // #347\n"
        "        _params.emplace_back(_itemName, _itemFilename, true, true, _itemSize);",
    ),
    # 7) #631: the source's end, apart from the response's.
    (
        _RESP_H,
        "  size_t _send_buffer_offset{0}, _send_buffer_len{0};\n",
        "  size_t _send_buffer_offset{0}, _send_buffer_len{0};\n"
        "  bool _sf631SourceEnded{false};  // #631: read to the end, not yet all queued\n",
    ),
    # 8) Nothing more to read once the source has ended.
    (
        _RESP,
        "      if (tcp_win == 0 || _state == RESPONSE_END) {\n",
        "      if (tcp_win == 0 || _sf631SourceEnded) {  // #631\n",
    ),
    # 9-11) The three places the source ends: the last chunk, nothing more
    # read, the last piece of a known length.
    (
        _RESP,
        "          if (!readLen) {\n"
        "            // last chunk?\n"
        "            _state = RESPONSE_END;\n",
        "          if (!readLen) {\n"
        "            // last chunk?\n"
        "            _sf631SourceEnded = true;  // #631\n",
    ),
    (
        _RESP,
        "          // no more data to send\n"
        "          _state = RESPONSE_END;\n",
        "          // no more data to send\n"
        "          _sf631SourceEnded = true;  // #631\n",
    ),
    (
        _RESP,
        "            // it was last piece of content\n"
        "            _state = RESPONSE_END;\n",
        "            // it was last piece of content\n"
        "            _sf631SourceEnded = true;  // #631\n",
    ),
    # 12) The response ends once what was read has all gone to the queue.
    (
        _RESP,
        "    } while (_send_buffer_len);  // go on till we have something in buffer pending to send\n",
        "    } while (_send_buffer_len);  // go on till we have something in buffer pending to send\n"
        "    if (_sf631SourceEnded && !_send_buffer_len) {\n"
        "      _state = RESPONSE_END;  // #631: only now, with nothing left behind\n"
        "    }\n",
    ),
    # 13) A credit is spent only on bytes put in flight; their ACK returns it.
    (
        _RESP,
        "    _writtenLength += payloadlen;\n"
        "#if ASYNCWEBSERVER_USE_CHUNK_INFLIGHT\n"
        "    _in_flight += payloadlen;\n"
        "    --_in_flight_credit;  // take a credit\n"
        "#endif\n",
        "    _writtenLength += payloadlen;\n"
        "#if ASYNCWEBSERVER_USE_CHUNK_INFLIGHT\n"
        "    if (payloadlen) {  // #631: a pass that sent nothing gets no ACK back\n"
        "      _in_flight += payloadlen;\n"
        "      --_in_flight_credit;  // take a credit\n"
        "    }\n"
        "#endif\n",
    ),
]


def _lib_src_dir(env):
    libdeps = pathlib.Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env["PIOENV"]
    return libdeps / "ESPAsyncWebServer" / "src"


def apply_patches(src_dir, patches=PATCHES):
    if not src_dir.is_dir():
        # Native / non-web envs don't pull the library — nothing to do.
        print(f"[patch_asyncweb] no ESPAsyncWebServer at {src_dir} — skipping")
        return
    for rel, anchor, repl in patches:
        path = src_dir / rel
        text = path.read_text()
        if SENTINEL in repl and repl in text:
            continue  # already patched (idempotent)
        # More precise idempotency: the specific replacement already present.
        if repl in text:
            continue
        if text.count(anchor) != 1:
            sys.exit(
                f"[patch_asyncweb] FATAL: anchor found {text.count(anchor)} times in {path.name} — "
                f"the pinned ESPAsyncWebServer changed shape (#347). Re-verify "
                f"the patch against the new version.\nAnchor:\n{anchor}"
            )
        path.write_text(text.replace(anchor, repl, 1))
        print(f"[patch_asyncweb] patched {rel}: {anchor.splitlines()[0][:48]}…")


try:
    Import("env")  # noqa: F821  (provided by PlatformIO SCons env)
    apply_patches(_lib_src_dir(env))  # noqa: F821
except NameError:
    # Imported as a plain module (e.g. from pytest) — do nothing.
    pass
