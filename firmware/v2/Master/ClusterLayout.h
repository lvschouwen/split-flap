#pragma once
// ClusterLayout.h — pure N-row grid layout engine for the multi-display
// cluster (#271, epic #270; spec docs/superpowers/specs/
// 2026-07-13-multi-display-cluster-design.md), natively tested by
// test_cluster_layout. No networking, no tasks — this is the math the
// leader runs before any fan-out.
//
// Member table semantics: each member owns the half-open unit span
// [col, col+width) of its row. Rows must be contiguous from 0 and every
// row must be tiled exactly — no gaps, no partial overlaps. Members with
// IDENTICAL {row, col, width} are legal and receive identical segments
// (a mirror is a table shape, not a mode). Row width is the tiled extent,
// so coincident members never double-count.
//
// Segment contract: segments are PRE-POSITIONED — the row text is padded
// to the full row width with the alignment applied (v1 lead math, same as
// flapFrameBuild) and then sliced by {col, width}. A follower renders its
// segment verbatim (Left), never re-wraps. Charset passes through
// untouched: case mapping and flap-index lookup happen in flapFrameBuild
// on each master, and $ & # umlaut wire-encoding is upstream of here.
//
// validateMemberTable runs at config time AND is re-run before every
// fan-out (MaintenancePolicy.h philosophy) — layoutGridText and
// clusterClockSegments therefore revalidate internally and refuse an
// invalid table.

#include <Arduino.h>

#include "DisplayCommand.h"  // DisplayAlignment

#define CLUSTER_MAX_MEMBERS 8
#define CLUSTER_HOST_MAX_LEN 40

// Cluster clock date row: v1's message-stamp date shape (%d %b %y).
// flapFrameBuild uppercases at render time, so "13 Jul 26" flips as
// "13 JUL 26".
#define CLUSTER_DATE_FORMAT "%d %b %y"

struct ClusterMemberDef {
  char host[CLUSTER_HOST_MAX_LEN + 1] = {0};
  uint8_t row = 0;
  uint8_t col = 0;    // starting unit column within the row
  uint8_t width = 0;  // units
};

struct ClusterMemberTable {
  uint8_t count = 0;
  ClusterMemberDef members[CLUSTER_MAX_MEMBERS];
};

// Derived grid facts (valid only when the verdict passed).
struct ClusterGrid {
  uint8_t rows = 0;
  uint16_t rowWidth[CLUSTER_MAX_MEMBERS] = {0};
};

// Config-time verdict: ok=false carries the 400 body for the web boundary.
struct ClusterVerdict {
  bool ok = false;
  const char* message = "";
};

inline ClusterVerdict validateMemberTable(const ClusterMemberTable& table,
                                          ClusterGrid& outGrid) {
  outGrid = ClusterGrid{};
  if (table.count == 0) return {false, "Member table is empty"};
  if (table.count > CLUSTER_MAX_MEMBERS) {
    return {false, "Too many members (max 8)"};
  }

  int maxRow = -1;
  for (int i = 0; i < table.count; i++) {
    const ClusterMemberDef& m = table.members[i];
    if (m.width == 0) return {false, "Member width must be at least 1"};
    if (m.row >= CLUSTER_MAX_MEMBERS) {
      return {false, "Row index beyond the member cap"};
    }
    if (m.row > maxRow) maxRow = m.row;
  }

  // Tile each row left to right: every cursor position must be the start
  // of exactly one member span (coincident mirror twins advance together,
  // so the row width is the tiled extent, never a sum).
  for (int r = 0; r <= maxRow; r++) {
    int inRow = 0;
    for (int i = 0; i < table.count; i++) {
      if (table.members[i].row == r) inRow++;
    }
    if (inRow == 0) return {false, "Rows must be contiguous from 0"};

    int cursor = 0;
    int placed = 0;
    while (placed < inRow) {
      int span = -1;
      for (int i = 0; i < table.count; i++) {
        const ClusterMemberDef& m = table.members[i];
        if (m.row != r || m.col != cursor) continue;
        if (span != -1 && m.width != span) {
          return {false, "Members overlap"};
        }
        span = m.width;
        placed++;
      }
      if (span == -1) return {false, "Row has a gap or overlapping members"};
      cursor += span;
    }
    outGrid.rowWidth[r] = (uint16_t)cursor;
  }

  outGrid.rows = (uint8_t)(maxRow + 1);
  return {true, ""};
}

// Pads `rowText` to exactly `width` with the v1 alignment contract
// (flapFrameBuild): at/over width keeps the FIRST width chars regardless
// of alignment; shorter text pads left/right/center with blanks.
inline String clusterAlignRow(const String& rowText, int width,
                              DisplayAlignment align) {
  const char* text = rowText.c_str();
  int len = (int)rowText.length();
  if (len > width) len = width;

  int lead = 0;
  if (len < width) {
    if (align == DisplayAlignment::Right) lead = width - len;
    else if (align == DisplayAlignment::Center) lead = (width - len) / 2;
  }

  String out;
  out.reserve(width);
  for (int i = 0; i < width; i++) {
    int textIndex = i - lead;
    out += (textIndex >= 0 && textIndex < len) ? text[textIndex] : ' ';
  }
  return out;
}

// Greedy word wrap of `text` into rows[0..grid.rows-1] (unpadded). Space
// runs collapse to single joins. A word that fits neither the current
// row's remainder nor a fresh next row but is wider than the current
// row's FULL width hard-splits (fills the remainder, spills onward); a
// word that would fit a row that doesn't exist truncates the rest.
inline void clusterWrapRows(const String& text, const ClusterGrid& grid,
                            String* rows) {
  for (int r = 0; r < grid.rows; r++) rows[r] = "";

  const char* p = text.c_str();
  int n = (int)text.length();
  int i = 0;
  int row = 0;
  while (i < n && row < grid.rows) {
    while (i < n && p[i] == ' ') i++;
    if (i >= n) break;
    int j = i;
    while (j < n && p[j] != ' ') j++;
    int len = j - i;

    int width = grid.rowWidth[row];
    int pos = (int)rows[row].length();
    int need = (pos == 0) ? len : pos + 1 + len;
    if (need <= width) {
      if (pos > 0) rows[row] += ' ';
      rows[row] += text.substring(i, j);
      i = j;
      continue;
    }
    if (row + 1 < grid.rows && len <= (int)grid.rowWidth[row + 1]) {
      row++;
      rows[row] = text.substring(i, j);
      i = j;
      continue;
    }
    if (len <= width) break;  // fits a row shape the grid has run out of

    // Hard-split: wider than the current row entirely — fill the
    // remainder, then keep chunking down the remaining rows.
    while (i < j && row < grid.rows) {
      int chunkWidth = grid.rowWidth[row];
      int chunkPos = (int)rows[row].length();
      if (chunkPos > 0) {
        if (chunkPos + 1 >= chunkWidth) {
          row++;
          continue;
        }
        rows[row] += ' ';
        chunkPos++;
      }
      int take = chunkWidth - chunkPos;
      if (take > j - i) take = j - i;
      rows[row] += text.substring(i, i + take);
      i += take;
      if (i < j) row++;
    }
  }
}

// Wraps `text` across the grid's rows, applies the alignment per row,
// slices into segments[0..table.count-1] (parallel to table.members).
// Overflowing text is truncated. Returns false on an invalid table.
inline bool layoutGridText(const String& text, DisplayAlignment align,
                           const ClusterMemberTable& table, String* segments) {
  ClusterGrid grid;
  if (!validateMemberTable(table, grid).ok) return false;

  String rows[CLUSTER_MAX_MEMBERS];
  clusterWrapRows(text, grid, rows);
  for (int r = 0; r < grid.rows; r++) {
    rows[r] = clusterAlignRow(rows[r], grid.rowWidth[r], align);
  }
  for (int m = 0; m < table.count; m++) {
    const ClusterMemberDef& def = table.members[m];
    segments[m] = rows[def.row].substring(def.col, def.col + def.width);
  }
  return true;
}

// Cluster clock frame: row 0 time, row 1 date (when the grid has one),
// rows 2+ blank at launch. Time/date arrive pre-formatted so this stays
// host-testable (ClockPolicy.h pattern). Returns false on an invalid table.
inline bool clusterClockSegments(const String& timeText, const String& dateText,
                                 DisplayAlignment align,
                                 const ClusterMemberTable& table,
                                 String* segments) {
  ClusterGrid grid;
  if (!validateMemberTable(table, grid).ok) return false;

  String rows[CLUSTER_MAX_MEMBERS];
  rows[0] = timeText;
  if (grid.rows >= 2) rows[1] = dateText;
  for (int r = 0; r < grid.rows; r++) {
    rows[r] = clusterAlignRow(rows[r], grid.rowWidth[r], align);
  }
  for (int m = 0; m < table.count; m++) {
    const ClusterMemberDef& def = table.members[m];
    segments[m] = rows[def.row].substring(def.col, def.col + def.width);
  }
  return true;
}
