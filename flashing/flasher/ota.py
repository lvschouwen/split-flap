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
