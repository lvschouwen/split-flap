"""Shelf packing of part footprints onto the printer bed. Pure: no CAD."""
from typing import Sequence


def shelf_pack(sizes: Sequence[tuple[float, float]], bed: float, margin: float,
               spacing: float) -> tuple[list[list[tuple[float, float]]], list[list[int]]]:
    """(corners, indices) per plate: the (x, y) corner and the input index of each part, in input order.

    Largest first on shelves; a part that fits no plate raises ValueError."""
    usable = bed - 2 * margin
    order = sorted(range(len(sizes)), key=lambda i: (-sizes[i][1], -sizes[i][0]))
    plates: list[list[tuple[int, float, float]]] = []
    shelves: list[list[float]] = []  # per plate: [cursor_x, shelf_y, shelf_h, next_y]
    for i in order:
        w, h = sizes[i]
        if w > usable or h > usable:
            raise ValueError(f"part {i} ({w:.1f} x {h:.1f}) does not fit the {bed} mm bed")
        placed = False
        for plate, sh in zip(plates, shelves):
            if sh[0] + w <= usable:
                plate.append((i, margin + sh[0], margin + sh[1]))
                sh[0] += w + spacing
                sh[2] = max(sh[2], h)
                placed = True
                break
            if sh[1] + sh[2] + spacing + h <= usable:
                sh[1] += sh[2] + spacing
                sh[0], sh[2] = 0.0, h
                plate.append((i, margin, margin + sh[1]))
                sh[0] = w + spacing
                placed = True
                break
        if not placed:
            plates.append([(i, margin, margin)])
            shelves.append([w + spacing, 0.0, h, 0.0])
    return [[(x, y) for _, x, y in sorted(plate)] for plate in plates], [[i for i, _, _ in sorted(plate)] for plate in plates]
