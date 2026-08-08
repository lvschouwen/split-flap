from splitflap_tui.flapwall import wall_cells, wall_width_needed


def test_one_cell_per_char_with_unit_gaps():
    t = wall_cells(["AB"], "", 80)
    assert t.plain == "row0  ▐A▌ ▐B▌"


def test_two_rows_render_stacked_with_labels():
    t = wall_cells(["AB", "C"], "", 80)
    assert t.plain == "row0  ▐A▌ ▐B▌\nrow1  ▐C▌"


def test_narrow_terminal_returns_none_for_fallback():
    # 8 units need 6 + 4*8 - 1 = 37 columns
    assert wall_width_needed(8) == 37
    assert wall_cells(["ABCDEFGH"], "", 36) is None
    assert wall_cells(["ABCDEFGH"], "", 37) is not None


def test_no_rows_falls_back_to_text_field():
    t = wall_cells(None, "HI", 80)
    assert t.plain == "row0  ▐H▌ ▐I▌"


def test_no_data_returns_none():
    assert wall_cells(None, "", 80) is None
    assert wall_cells([], "", 80) is None


def test_bracket_payload_stays_literal():
    t = wall_cells(None, "[/]", 80)
    assert "[" in t.plain and "/" in t.plain and "]" in t.plain
