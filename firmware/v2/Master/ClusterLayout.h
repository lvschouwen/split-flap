#pragma once
// ClusterLayout.h — pure N-row grid layout engine for the multi-display
// cluster (#271, epic #270; spec docs/superpowers/specs/
// 2026-07-13-multi-display-cluster-design.md), natively tested by
// test_cluster_layout. No networking, no tasks — this is the math the
// leader runs before any fan-out.
//
// Member table semantics: each member owns the half-open unit span
// [col, col+width) of its row. Row indices must be contiguous from 0 and
// spans in a row must not partially overlap. Members with IDENTICAL
// {row, col, width} are legal and receive identical segments (a mirror is
// a table shape, not a mode). A row may have GAPS — columns no member
// owns: `col` says where a display physically hangs (a narrow row centred
// under a wider one), and there are no flaps in a gap.
//
// So text never lands in a gap: a row's width is the number of columns
// that HAVE flaps (coincident members counted once), text is wrapped and
// aligned to that, and its characters fill the owned columns left to
// right, skipping gaps. Only the wall mirror is laid out in physical
// columns, with gaps blank.
//
// Segment contract: segments are PRE-POSITIONED — the row text is padded
// to the full row width with the alignment applied (v1 lead math, same as
// flapFrameBuild) and then sliced per member. A follower renders its
// segment verbatim (Left), never re-wraps. Charset passes through
// untouched: case mapping and flap-index lookup happen in flapFrameBuild
// on each master, and $ & # umlaut wire-encoding is upstream of here.
//
// validateMemberTable runs at config time AND is re-run before every
// fan-out (MaintenancePolicy.h philosophy) — layoutGridText and
// clusterClockSegments therefore revalidate internally and refuse an
// invalid table.

#include <Arduino.h>

#include "DisplayCommand.h"     // DisplayAlignment

// Boards a Split-Flap can be made of (the rows table holds this many), and
// the longest address one is reached at.
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
  uint16_t rowWidth[CLUSTER_MAX_MEMBERS] = {0};   // columns with flaps
  uint16_t rowExtent[CLUSTER_MAX_MEMBERS] = {0};  // physical: max col+width
};

// Is members[i] the first of its span? Coincident mirror twins share one
// span, and only the first counts toward widths and offsets.
inline bool clusterSpanIsFirst(const ClusterMemberTable& table, int i) {
  const ClusterMemberDef& m = table.members[i];
  for (int j = 0; j < i; j++) {
    const ClusterMemberDef& o = table.members[j];
    if (o.width != 0 && o.row == m.row && o.col == m.col) return false;
  }
  return true;
}

// How many columns with flaps lie left of `col` in `row` — where a member
// starting at `col` begins in the row's text. Valid tables only.
inline int clusterTextOffset(const ClusterMemberTable& table, uint8_t row,
                             uint8_t col) {
  int offset = 0;
  for (int i = 0; i < table.count; i++) {
    const ClusterMemberDef& m = table.members[i];
    if (m.width == 0 || m.row != row || m.col >= col) continue;
    if (clusterSpanIsFirst(table, i)) offset += m.width;
  }
  return offset;
}

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
    if (m.row >= CLUSTER_MAX_MEMBERS) {
      return {false, "Row index beyond the member cap"};
    }
    // #333 warm-standby: a width-0 member is OFF-GRID — it keeps membership
    // (pings, digest, promote rank) but claims no columns, so it never tiles
    // a row. Its row/col are ignored; skip it from every grid computation.
    if (m.width == 0) continue;
    if (m.row > maxRow) maxRow = m.row;
  }
  // #333: at least one member must render — an all-off-grid table (every
  // member width 0) would "enable" a cluster that shows nothing anywhere.
  if (maxRow == -1) return {false, "No rendering members (all off-grid)"};

  // Two spans in a row are either the same span (mirror twins) or disjoint.
  for (int i = 0; i < table.count; i++) {
    const ClusterMemberDef& a = table.members[i];
    if (a.width == 0) continue;
    for (int j = i + 1; j < table.count; j++) {
      const ClusterMemberDef& b = table.members[j];
      if (b.width == 0 || b.row != a.row) continue;
      if (a.col == b.col && a.width == b.width) continue;
      if ((int)a.col < (int)b.col + b.width &&
          (int)b.col < (int)a.col + a.width) {
        return {false, "Members overlap"};
      }
    }
  }

  for (int i = 0; i < table.count; i++) {
    const ClusterMemberDef& m = table.members[i];
    if (m.width == 0) continue;
    uint16_t extent = (uint16_t)(m.col + m.width);
    if (extent > outGrid.rowExtent[m.row]) outGrid.rowExtent[m.row] = extent;
    if (clusterSpanIsFirst(table, i)) outGrid.rowWidth[m.row] += m.width;
  }
  for (int r = 0; r <= maxRow; r++) {
    if (outGrid.rowWidth[r] == 0) {
      outGrid = ClusterGrid{};
      return {false, "Rows must be contiguous from 0"};
    }
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
//
// Row breaks (#290): the wire's literal "\n" marker (the v1 composer
// contract — v1 turned it into temporal paging) and a raw newline both
// force the next row here, uniformly: consecutive/leading breaks leave
// deliberate blank rows, breaks past the last row truncate the rest.
inline void clusterWrapRows(const String& text, const ClusterGrid& grid,
                            String* rows) {
  for (int r = 0; r < grid.rows; r++) rows[r] = "";

  String norm = text;
  norm.replace("\\n", "\n");

  const char* p = norm.c_str();
  int n = (int)norm.length();
  int i = 0;
  int row = 0;
  while (i < n && row < grid.rows) {
    while (i < n && p[i] == ' ') i++;
    if (i >= n) break;
    if (p[i] == '\n') {
      row++;
      i++;
      continue;
    }
    int j = i;
    while (j < n && p[j] != ' ' && p[j] != '\n') j++;
    int len = j - i;

    int width = grid.rowWidth[row];
    int pos = (int)rows[row].length();
    int need = (pos == 0) ? len : pos + 1 + len;
    if (need <= width) {
      if (pos > 0) rows[row] += ' ';
      rows[row] += norm.substring(i, j);
      i = j;
      continue;
    }
    if (row + 1 < grid.rows && len <= (int)grid.rowWidth[row + 1]) {
      row++;
      rows[row] = norm.substring(i, j);
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
      rows[row] += norm.substring(i, i + take);
      i += take;
      if (i < j) row++;
    }
  }
}

// A member's share of its (padded) row text. Off-grid members get "".
inline String clusterSliceSegment(const ClusterMemberTable& table,
                                  const String* rows, int m) {
  const ClusterMemberDef& def = table.members[m];
  if (def.width == 0) return String();
  int from = clusterTextOffset(table, def.row, def.col);
  return rows[def.row].substring(from, from + def.width);
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
    segments[m] = clusterSliceSegment(table, rows, m);
  }
  return true;
}

// Wall-mirror reconstruction (#277) — the inverse of the slice above:
// rebuild every full row text from the per-member segments so the leader
// can push the whole wall over SSE and into HA's text/state. Segments are
// pre-positioned, so remote slots copy verbatim; every empty-host slot
// takes `selfRowText` instead (this master's LIVE currentText — a
// transient/notification may own it, and the display re-applies the
// alignment lead, so the same lead math runs here). Self slots overlay in
// a second pass: on a coincident self+remote twin the live text must win
// over the twin's stale segment. Rows come out in PHYSICAL columns (the
// wall as it hangs): each as long as its rightmost member reaches, gaps
// blank. rows[] needs CLUSTER_MAX_MEMBERS entries. Returns the grid's row
// count, 0 on an invalid table.
inline int clusterMirrorRows(const ClusterMemberTable& table,
                             const String* segments, const String& selfRowText,
                             DisplayAlignment selfAlign, String* rows) {
  ClusterGrid grid;
  if (!validateMemberTable(table, grid).ok) return 0;

  for (int r = 0; r < grid.rows; r++) {
    rows[r] = clusterAlignRow(String(), grid.rowExtent[r],
                              DisplayAlignment::Left);
  }
  for (int pass = 0; pass < 2; pass++) {
    for (int m = 0; m < table.count; m++) {
      const ClusterMemberDef& def = table.members[m];
      bool self = def.host[0] == '\0';
      if (self != (pass == 1)) continue;
      String slot = self ? clusterAlignRow(selfRowText, def.width, selfAlign)
                         : clusterAlignRow(segments[m], def.width,
                                           DisplayAlignment::Left);
      for (int c = 0; c < def.width; c++) {
        rows[def.row].setCharAt((unsigned int)(def.col + c), slot[c]);
      }
    }
  }
  return grid.rows;
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
    segments[m] = clusterSliceSegment(table, rows, m);
  }
  return true;
}
