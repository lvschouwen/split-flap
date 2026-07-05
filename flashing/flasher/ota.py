"""OTA upload + verdicts — Python port of flashing/ota-master.sh.

Decision matrix ported line-for-line (ota-master.sh:175-224, #53/#60/#118):
the running sketchMd5 matching the uploaded file's MD5 is the primary
success signal; the lastFlashResult flag is secondary. Retry policy:
ONLY 'reverted' retries — every other verdict is a config/network/logic
problem that retrying cannot fix.
"""

SUCCESS = "success"
REVERTED = "reverted"
NO_HANDLER = "no-handler"
FLASH_CONFIG_MISMATCH = "flash-config-mismatch"
UPLOAD_FAILED = "upload-failed"
UNREACHABLE = "unreachable"
INCONSISTENT = "inconsistent"

RETRYABLE = {REVERTED}


def classify_upload(http_code: int, body: str) -> str | None:
    if "flash config" in body.lower():
        return FLASH_CONFIG_MISMATCH
    if http_code != 200:
        return UPLOAD_FAILED
    return None


def classify_post_flash(uploaded_md5: str, pre_md5: str, post_md5: str, last_result: str) -> str:
    if post_md5 == uploaded_md5:
        return SUCCESS
    if last_result == "reverted":
        return REVERTED
    if last_result == "" and post_md5 == pre_md5 and post_md5 != "?":
        return NO_HANDLER
    return INCONSISTENT


import hashlib
import json
import time
import urllib.error
import urllib.request
import uuid


def encode_multipart(field: str, filename: str, data: bytes) -> tuple:
    boundary = uuid.uuid4().hex
    body = (
        f"--{boundary}\r\n"
        f'Content-Disposition: form-data; name="{field}"; filename="{filename}"\r\n'
        f"Content-Type: application/octet-stream\r\n\r\n"
    ).encode() + data + f"\r\n--{boundary}--\r\n".encode()
    return body, f"multipart/form-data; boundary={boundary}"


def fetch_settings(base_url: str, timeout: float = 5.0):
    try:
        with urllib.request.urlopen(f"{base_url}/settings", timeout=timeout) as r:
            return json.loads(r.read().decode())
    except (urllib.error.URLError, ValueError, OSError):
        return None


def fetch_setting(base_url: str, key: str) -> str:
    settings = fetch_settings(base_url)
    if settings is None or key not in settings or settings[key] is None:
        return "?"
    return str(settings[key])


def _enter_ota_mode(base_url: str, say) -> None:
    say("requesting quiet OTA mode...")
    try:
        req = urllib.request.Request(f"{base_url}/firmware/ota-mode", data=b"", method="POST")
        urllib.request.urlopen(req, timeout=10)
    except (urllib.error.URLError, OSError):
        say("/firmware/ota-mode not accepted (pre-#117 firmware) — normal-mode flash")
        return
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        time.sleep(3)
        if fetch_setting(base_url, "isInOtaMode") == "True":
            say("device is in OTA mode — display quiet")
            return
    say("device did not report OTA mode within 60 s — continuing anyway")


def _one_attempt(base_url: str, data: bytes, uploaded_md5: str, git_rev: str, say) -> str:
    pre_md5 = fetch_setting(base_url, "sketchMd5")
    if pre_md5 == "?":
        say(f"device unreachable at {base_url}")
        return UNREACHABLE
    url = f"{base_url}/firmware/master?md5={uploaded_md5}"
    if git_rev:
        url += f"&v={git_rev}"
    body, ctype = encode_multipart("firmware", "firmware.bin", data)
    req = urllib.request.Request(url, data=body, headers={"Content-Type": ctype}, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=180) as r:
            http_code, resp = r.status, r.read().decode(errors="replace")
    except urllib.error.HTTPError as e:
        http_code, resp = e.code, e.read().decode(errors="replace")
    except (urllib.error.URLError, OSError) as e:
        say(f"upload failed: {e}")
        return UPLOAD_FAILED
    say(f"HTTP {http_code}: {resp.strip()[:200]}")
    verdict = classify_upload(http_code, resp)
    if verdict:
        return verdict
    say("master rebooting — polling /settings (up to 90 s)...")
    deadline = time.monotonic() + 90
    post_md5, last_result = "?", "?"
    while time.monotonic() < deadline:
        time.sleep(3)
        post_md5 = fetch_setting(base_url, "sketchMd5")
        if post_md5 not in ("?", ""):
            last_result = fetch_setting(base_url, "lastFlashResult")
            break
    say(f"sketchMd5 {pre_md5} -> {post_md5}   lastFlashResult: {last_result}")
    return classify_post_flash(uploaded_md5, pre_md5, post_md5, last_result)


def run_ota(base_url: str, bin_path: str, git_rev: str, max_attempts: int, quiet_mode: bool, say) -> str:
    from pathlib import Path
    data = Path(bin_path).read_bytes()
    uploaded_md5 = hashlib.md5(data).hexdigest()
    verdict = INCONSISTENT
    for attempt in range(1, max_attempts + 1):
        say(f"=== OTA attempt {attempt} of {max_attempts} ===")
        if quiet_mode:
            _enter_ota_mode(base_url, say)
        verdict = _one_attempt(base_url, data, uploaded_md5, git_rev, say)
        if verdict == SUCCESS or verdict not in RETRYABLE:
            return verdict
        if attempt < max_attempts:
            say("EBOOT SILENT REVERT — retrying in 5 s...")
            time.sleep(5)
    return verdict
