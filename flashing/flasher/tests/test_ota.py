from flasher.ota import (
    FLASH_CONFIG_MISMATCH, INCONSISTENT, NO_HANDLER, RETRYABLE, REVERTED,
    SUCCESS, UPLOAD_FAILED, classify_post_flash, classify_upload,
)

MD5 = "aabbccdd" * 4
OLD = "11223344" * 4


def test_flash_config_in_body_wins_over_http_code():
    assert classify_upload(200, "Flash config wrong") == FLASH_CONFIG_MISMATCH
    assert classify_upload(412, "flash config exceeds chip") == FLASH_CONFIG_MISMATCH


def test_non_200_is_upload_failed():
    assert classify_upload(500, "boom") == UPLOAD_FAILED
    assert classify_upload(0, "") == UPLOAD_FAILED


def test_200_clean_body_proceeds():
    assert classify_upload(200, "OK") is None


def test_md5_match_is_success_even_if_flag_disagrees():
    # sketchMd5 is the primary signal (#118) — pre-#118 firmware false-reports
    assert classify_post_flash(MD5, OLD, MD5, "reverted") == SUCCESS
    assert classify_post_flash(MD5, OLD, MD5, "ok") == SUCCESS
    assert classify_post_flash(MD5, OLD, MD5, "") == SUCCESS


def test_reverted_flag_without_md5_match():
    assert classify_post_flash(MD5, OLD, OLD, "reverted") == REVERTED


def test_no_handler_needs_empty_flag_and_unchanged_md5():
    assert classify_post_flash(MD5, OLD, OLD, "") == NO_HANDLER


def test_unreachable_post_md5_is_inconsistent():
    assert classify_post_flash(MD5, OLD, "?", "?") == INCONSISTENT


def test_changed_but_wrong_md5_is_inconsistent():
    assert classify_post_flash(MD5, OLD, "99" * 16, "ok") == INCONSISTENT


def test_only_reverted_is_retryable():
    assert RETRYABLE == {REVERTED}
