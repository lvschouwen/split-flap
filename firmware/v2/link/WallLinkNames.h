#pragma once
// WallLinkNames.h — the hostname a board of a Split-Flap carries from its
// place on the wall (#611). Pure; compiled by the master (which derives the
// names and sends each row its own in a Name message) and by the row board
// (which checks a name before taking it). Natively tested in Master's
// test_wall_link.
//
// A board's id (split-flap-<chip id>) is its identity on the link, in the
// rows table and in the master's scan; the hostname is a second thing, for
// people: split-flap-row<N> for the board on grid row N (counted from 1, as
// the page shows it), with -c<C> behind it (the column, from 1) only when
// another board hangs on the same grid row. The master answers for its own
// row's name on mDNS beside its own.

#include <stddef.h>
#include <stdio.h>
#include <string.h>

// A hostname is one DNS label: mDNS, the DHCP request and the master's board
// name setting all take at most this many characters.
#define WALL_HOST_NAME_MAX 24
#define WALL_HOST_NAME_PREFIX "split-flap-row"

// Label-safe: 1..WALL_HOST_NAME_MAX of [a-z0-9-], neither starting nor ending
// with a hyphen. An empty name is not valid here; a row reads it as "none".
inline bool wallHostNameValid(const char* name) {
  if (name == nullptr) return false;
  const size_t n = strlen(name);
  if (n == 0 || n > WALL_HOST_NAME_MAX) return false;
  if (name[0] == '-' || name[n - 1] == '-') return false;
  for (size_t i = 0; i < n; i++) {
    const char c = name[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    if (!ok) return false;
  }
  return true;
}

// The name of the board on grid row `row` (from 1) at column `col` (from 1);
// `shared` = another board hangs on that grid row. Returns the length written,
// 0 (and an empty string) when the place is no place or `out` is too small.
inline size_t wallRowHostName(int row, int col, bool shared, char* out, size_t cap) {
  if (cap == 0) return 0;
  out[0] = 0;
  if (row < 1 || row > 256 || col < 1 || col > 256) return 0;
  int n = shared ? snprintf(out, cap, WALL_HOST_NAME_PREFIX "%d-c%d", row, col)
                 : snprintf(out, cap, WALL_HOST_NAME_PREFIX "%d", row);
  if (n <= 0 || (size_t)n >= cap || n > WALL_HOST_NAME_MAX) {
    out[0] = 0;
    return 0;
  }
  return (size_t)n;
}
