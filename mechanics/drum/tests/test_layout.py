import pytest

from drum.layout import shelf_pack


def test_parts_do_not_overlap_and_stay_on_the_bed():
    sizes = [(95, 73), (95, 73), (38, 95), (38, 95), (40, 40), (20, 10), (200, 20)]
    corners, idx = shelf_pack(sizes, 256, 5, 3)
    assert sorted(i for ids in idx for i in ids) == list(range(len(sizes)))
    for cs, ids in zip(corners, idx):
        rects = [(x, y, x + sizes[i][0], y + sizes[i][1]) for (x, y), i in zip(cs, ids)]
        for r in rects:
            assert r[0] >= 5 and r[1] >= 5 and r[2] <= 251 and r[3] <= 251
        for a in range(len(rects)):
            for b in range(a + 1, len(rects)):
                ra, rb = rects[a], rects[b]
                assert ra[2] <= rb[0] or rb[2] <= ra[0] or ra[3] <= rb[1] or rb[3] <= ra[1]


def test_overflow_opens_a_second_plate():
    corners, _ = shelf_pack([(200, 200), (200, 200)], 256, 5, 3)
    assert len(corners) == 2


def test_oversize_part_is_rejected():
    with pytest.raises(ValueError, match="does not fit"):
        shelf_pack([(300, 10)], 256, 5, 3)
