#pragma once
// WallLinkCore.h — what the master does with its row boards' connections on
// the wall link (#559/#566): who is behind a connection, what each row is
// sent and when. Pure, natively tested by test_wall_link_core; the sockets
// are the link task's (WallLink.cpp), reached through WallLinkHooks. The
// timing rules are WallLinkPolicy.h.
//
// A connection is nobody until its first message, which must be a Hello from
// a row in this master's table. A row has one connection: a newer one from the
// same address replaces the older (a row that restarted dials while the master
// still holds the old socket). From another address it is refused while the
// older stands: a row's id is no secret and must not be enough to take a
// working row's place. A connection that is not heard for the lost mark is
// closed unless the row is busy, which also frees a row whose address moved.
// Rows are named by their index in the table, so a changed table means
// reset(): every row dials again.
//
// One task only. Nothing here blocks or allocates.

#include <stdint.h>
#include <string.h>

#include "WallLinkPolicy.h"

#define WALL_LINK_MAX_ROWS 8
// Every row, and two more for a connection that has not said who it is yet.
#define WALL_LINK_MAX_CONNS (WALL_LINK_MAX_ROWS + 2)

struct WallLinkHooks {
  virtual ~WallLinkHooks() = default;
  // The index of the row with this id in the rows table, -1 when unknown.
  virtual int rowForId(const char* id) = 0;
  virtual const char* masterId() = 0;
  // Writes one whole message to the connection, or nothing: false when the
  // socket cannot take it now (the row is not reading).
  virtual bool write(int conn, const uint8_t* data, size_t n) = 0;
  // Closes the socket of a connection the core has let go of.
  virtual void close(int conn) = 0;
  // Do these two connections come from the same address?
  virtual bool samePeer(int a, int b) = 0;
  // A row was welcomed. `restarted`: its boot id differs from the last one.
  virtual void rowHello(int row, const wl_Hello& hello, bool restarted) = 0;
  // Every later message of a welcomed row except Pong.
  virtual void rowMessage(int row, const wl_ToMaster& message) = 0;
  // A line for the log; row is -1 for a connection that is nobody yet.
  virtual void note(int conn, int row, const char* what) = 0;
};

struct WallLinkConn {
  bool open = false;
  int8_t row = -1;  // -1 until its Hello was accepted
  uint32_t openedAtMs = 0;
  WlMasterReader reader;
};

struct WallLinkRow {
  int8_t conn = -1;
  WallRowContact contact;
  WallRowText text;
};

struct WallLinkCore {
  WallLinkConn conns[WALL_LINK_MAX_CONNS];
  WallLinkRow rows[WALL_LINK_MAX_ROWS];

  // A socket was accepted: its connection number, -1 when there is no room
  // (the caller closes the socket). When every slot is taken, the connection
  // that has been nobody the longest makes way, so sockets that say nothing
  // cannot keep the rows out.
  int accept(uint32_t nowMs, WallLinkHooks& hooks) {
    int slot = -1;
    for (int i = 0; i < WALL_LINK_MAX_CONNS && slot < 0; i++) {
      if (!conns[i].open) slot = i;
    }
    if (slot < 0) {
      for (int i = 0; i < WALL_LINK_MAX_CONNS; i++) {
        if (conns[i].row >= 0) continue;
        if (slot < 0 || (uint32_t)(nowMs - conns[i].openedAtMs) >
                            (uint32_t)(nowMs - conns[slot].openedAtMs)) {
          slot = i;
        }
      }
      if (slot < 0) return -1;
      drop(slot, "made way for a newer connection", hooks);
    }
    conns[slot] = WallLinkConn{};
    conns[slot].open = true;
    conns[slot].openedAtMs = nowMs;
    return slot;
  }

  // The socket of this connection is gone (closed by the row, or failed).
  void closed(int conn) {
    WallLinkConn& c = conns[conn];
    if (!c.open) return;
    if (c.row >= 0 && rows[c.row].conn == conn) {
      rows[c.row].conn = -1;
      wallRowDropped(rows[c.row].contact);
    }
    c.open = false;
    c.row = -1;
  }

  // Bytes read from a connection's socket.
  void bytes(int conn, const uint8_t* data, size_t n, uint32_t nowMs, WallLinkHooks& hooks) {
    WallLinkConn& c = conns[conn];
    while (c.open) {
      const size_t took = c.reader.feed(data, n);
      data += took;
      n -= took;
      WlFeed state;
      while (c.open && (state = c.reader.peek()) == WlFeed::Message) {
        // A message this build cannot read (a newer row's) is skipped.
        const bool readable = c.reader.decode(wl_ToMaster_fields, &in);
        c.reader.pop();
        if (readable) {
          message(conn, nowMs, hooks);
        } else if (c.row < 0) {
          hooks.note(conn, -1, "a first message this build cannot read");
        }
      }
      if (!c.open) return;
      if (state == WlFeed::Bad) return drop(conn, "not a link message", hooks);
      if (n == 0) return;
    }
  }

  // Once per pass of the link task.
  void tick(uint32_t nowMs, WallLinkHooks& hooks) {
    for (int i = 0; i < WALL_LINK_MAX_CONNS; i++) {
      const WallLinkConn& c = conns[i];
      if (c.open && c.row < 0 && wallLinkElapsed(nowMs, c.openedAtMs, WALL_LINK_HELLO_TIMEOUT_MS)) {
        drop(i, "no Hello", hooks);
      }
    }
    for (int r = 0; r < WALL_LINK_MAX_ROWS; r++) {
      WallLinkRow& row = rows[r];
      if (row.conn < 0) continue;
      // Keepalive only proves the row's TCP stack: a row that is not busy
      // answers pings, so silence means its program is stuck or gone.
      if (!row.contact.busy && wallLinkElapsed(nowMs, row.contact.lastHeardMs, WALL_LINK_LOST_MS)) {
        drop(row.conn, "not heard from", hooks);
        continue;
      }
      if (wallRowTextDue(row.text, row.contact)) {
        wlClear(out);
        out.which_body = wl_ToRow_show_tag;
        wl_Show& show = out.body.show;
        show.render_id = row.text.renderId;
        show.commit_at_ms = row.text.commitAtMs;
        show.speed = row.text.speed;
        memcpy(show.text, row.text.text, sizeof(show.text));
        if (send(r, out, nowMs, hooks)) wallRowTextSent(row.text);
      } else if (wallLinkPingDue(row.contact, nowMs)) {
        wlClear(out);
        out.which_body = wl_ToRow_ping_tag;
        send(r, out, nowMs, hooks);
      }
    }
  }

  // The latest text for a row; it goes out when the row can take it.
  void setText(int row, const char* text, uint16_t speed, uint64_t commitAtMs) {
    wallRowTextSet(rows[row].text, text, speed, commitAtMs);
  }

  // Writes one message to a welcomed row now. False when the row is not
  // connected or its socket takes nothing: the caller keeps what it wanted
  // to say and tries again, it is not queued here.
  bool send(int row, const wl_ToRow& message, uint32_t nowMs, WallLinkHooks& hooks) {
    WallLinkRow& r = rows[row];
    if (r.conn < 0) return false;
    const size_t n = wlEncodeToRow(frame, sizeof(frame), message);
    if (n == 0 || !hooks.write(r.conn, frame, n)) return false;
    wallRowSent(r.contact, nowMs);
    return true;
  }

  // The rows table changed: every connection goes and nothing is remembered.
  void reset(WallLinkHooks& hooks) {
    for (int i = 0; i < WALL_LINK_MAX_CONNS; i++) {
      if (conns[i].open) drop(i, "rows table changed", hooks);
    }
    for (int r = 0; r < WALL_LINK_MAX_ROWS; r++) rows[r] = WallLinkRow{};
  }

 private:
  wl_ToMaster in;
  wl_ToRow out;
  uint8_t frame[wl_ToRow_size + WL_PREFIX_MAX];

  void drop(int conn, const char* why, WallLinkHooks& hooks) {
    hooks.note(conn, conns[conn].row, why);
    closed(conn);
    hooks.close(conn);
  }

  void message(int conn, uint32_t nowMs, WallLinkHooks& hooks) {
    WallLinkConn& c = conns[conn];
    const bool isHello = in.which_body == wl_ToMaster_hello_tag;
    if (c.row < 0) {
      if (!isHello) return drop(conn, "no Hello first", hooks);
      return hello(conn, nowMs, hooks);
    }
    if (isHello) return drop(conn, "a second Hello", hooks);
    WallLinkRow& row = rows[c.row];
    wallRowHeard(row.contact, nowMs);
    if (in.which_body == wl_ToMaster_pong_tag) return;
    if (in.which_body == wl_ToMaster_status_tag) row.contact.busy = in.body.status.busy;
    // A download holds the row as a unit job does, but its Status does not
    // say so: the row announces it with this message instead.
    if (in.which_body == wl_ToMaster_update_state_tag) {
      row.contact.busy = in.body.update_state.phase == wl_UpdatePhase_UPDATE_DOWNLOADING;
    }
    if (in.which_body == wl_ToMaster_shown_tag) {
      wallRowTextShown(row.text, in.body.shown.render_id);
    }
    hooks.rowMessage(c.row, in);
  }

  void hello(int conn, uint32_t nowMs, WallLinkHooks& hooks) {
    const wl_Hello& h = in.body.hello;
    const int r = hooks.rowForId(h.id);
    const bool known = r >= 0 && r < WALL_LINK_MAX_ROWS;
    switch (wallLinkJudgeHello(h.protocol, h.id, known)) {
      case WallHello::Accept: break;
      case WallHello::WrongProtocol: return drop(conn, "another link protocol", hooks);
      case WallHello::NoId: return drop(conn, "a Hello without an id", hooks);
      case WallHello::NotPaired: return drop(conn, "a row this master does not know", hooks);
    }
    WallLinkRow& row = rows[r];
    if (row.conn >= 0) {
      if (!hooks.samePeer(row.conn, conn)) {
        return drop(conn, "its row is connected from another address", hooks);
      }
      drop(row.conn, "replaced by a newer connection", hooks);
    }
    row.conn = (int8_t)conn;
    conns[conn].row = (int8_t)r;
    wallRowConnected(row.contact, nowMs);
    row.contact.helloSeen = true;

    wlClear(out);
    out.which_body = wl_ToRow_welcome_tag;
    out.body.welcome.protocol = WALL_LINK_PROTOCOL;
    strncpy(out.body.welcome.master_id, hooks.masterId(), sizeof(out.body.welcome.master_id) - 1);
    if (!send(r, out, nowMs, hooks)) return drop(conn, "Welcome not written", hooks);

    wallRowHeard(row.contact, nowMs);
    const bool restarted = wallRowNoteBoot(row.contact, h.boot_id);
    wallRowTextResend(row.text);
    hooks.rowHello(r, h, restarted);
  }
};
