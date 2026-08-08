"""Display + settings writes. Everything rides the v1-compatible form POST /
with ajax=1 (WebSettings.cpp) — there is no JSON settings route by design."""
from __future__ import annotations

from .ops import submit_op
from .transport import BoardClient


def post_form(client: BoardClient, **fields: str) -> str:
    data = {k: str(v) for k, v in fields.items()}
    data["ajax"] = "1"
    return client.post("/", data=data).text


def set_text(client: BoardClient, text: str) -> str:
    return post_form(client, inputText=text)


def set_mode(client: BoardClient, mode: str) -> str:
    return post_form(client, deviceMode=mode)


def notify(client: BoardClient, text: str, dwell_s: int) -> str:
    return post_form(client, transientText=text, transientDwell=dwell_s)


def set_setting(client: BoardClient, field: str, value: str) -> str:
    return post_form(client, **{field: value})


def stop(client: BoardClient) -> int:
    return submit_op(client, "/stop", {})


def reboot(client: BoardClient) -> None:
    client.post("/reboot")
