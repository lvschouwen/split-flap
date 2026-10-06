// Native tests for WallLinkCore.h: what the master does with its row boards'
// connections, with the sockets replaced by a recorder.
#include <unity.h>

#include <string>
#include <vector>

#include "WallLinkCore.h"

struct Recorder : WallLinkHooks {
  std::vector<std::string> known{"row-a", "row-b"};
  std::vector<std::pair<int, wl_ToRow>> written;
  std::vector<int> closed;
  std::vector<std::string> notes;
  int hellos = 0, restarts = 0, lastHelloRow = -1;
  std::vector<pb_size_t> messages;
  bool refuseWrites = false;
  int peerOf[WALL_LINK_MAX_CONNS] = {0};  // an address per connection; all the same by default

  int rowForId(const char* id) override {
    for (size_t i = 0; i < known.size(); i++) {
      if (known[i] == id) return (int)i;
    }
    return -1;
  }
  const char* masterId() override { return "the-master"; }
  bool write(int conn, const uint8_t* data, size_t n) override {
    if (refuseWrites) return false;
    WlRowReader reader;
    TEST_ASSERT_EQUAL(n, reader.feed(data, n));
    wl_ToRow m;
    TEST_ASSERT_TRUE(reader.decode(wl_ToRow_fields, &m));
    written.push_back({conn, m});
    return true;
  }
  void close(int conn) override { closed.push_back(conn); }
  bool samePeer(int a, int b) override { return peerOf[a] == peerOf[b]; }
  // Address 0 (the default of every connection here) is where rows were paired.
  bool fromPairedAddress(int conn, int) override { return peerOf[conn] == 0; }
  void rowHello(int row, const wl_Hello&, bool restarted) override {
    hellos++;
    lastHelloRow = row;
    if (restarted) restarts++;
  }
  void rowMessage(int, const wl_ToMaster& m) override { messages.push_back(m.which_body); }
  void note(int, int, const char* what) override { notes.push_back(what); }

  int count(pb_size_t tag) const {
    int n = 0;
    for (auto& w : written) n += w.second.which_body == tag;
    return n;
  }
  const wl_ToRow& last() const { return written.back().second; }
};

static WallLinkCore core;
static Recorder* rec;

void setUp() {
  core = WallLinkCore{};
  delete rec;
  rec = new Recorder;
}
void tearDown() {}

static void feed(int conn, const wl_ToMaster& m, uint32_t now) {
  uint8_t buf[wl_ToMaster_size + WL_PREFIX_MAX];
  const size_t n = wlEncodeToMaster(buf, sizeof buf, m);
  TEST_ASSERT_TRUE(n > 0);
  core.bytes(conn, buf, n, now, *rec);
}

static wl_ToMaster hello(const char* id, uint32_t bootId = 7, uint32_t protocol = WALL_LINK_PROTOCOL) {
  wl_ToMaster m;
  wlClear(m);
  m.which_body = wl_ToMaster_hello_tag;
  m.body.hello.protocol = protocol;
  strcpy(m.body.hello.id, id);
  strcpy(m.body.hello.rev, "abc1234");
  m.body.hello.boot_id = bootId;
  m.body.hello.width = 5;
  return m;
}

static wl_ToMaster plain(pb_size_t tag) {
  wl_ToMaster m;
  wlClear(m);
  m.which_body = tag;
  return m;
}

static wl_ToMaster status(bool busy) {
  wl_ToMaster m = plain(wl_ToMaster_status_tag);
  m.body.status.busy = busy;
  return m;
}

// A row connected and greeted at `now`.
static int joined(const char* id, uint32_t now, uint32_t bootId = 7) {
  const int conn = core.accept(now, *rec);
  TEST_ASSERT_TRUE(conn >= 0);
  feed(conn, hello(id, bootId), now);
  return conn;
}

// ---- hello ---------------------------------------------------------------------

static void test_a_known_row_is_welcomed_by_name() {
  const int conn = joined("row-b", 1000);
  TEST_ASSERT_EQUAL(2, rec->written.size());  // Welcome, then the wall's quiet state
  TEST_ASSERT_EQUAL(conn, rec->written[0].first);
  TEST_ASSERT_EQUAL(wl_ToRow_welcome_tag, rec->written[0].second.which_body);
  TEST_ASSERT_EQUAL_STRING("the-master", rec->written[0].second.body.welcome.master_id);
  TEST_ASSERT_EQUAL_UINT32(WALL_LINK_PROTOCOL, rec->written[0].second.body.welcome.protocol);
  TEST_ASSERT_EQUAL(wl_ToRow_quiet_tag, rec->written[1].second.which_body);
  TEST_ASSERT_EQUAL(1, rec->hellos);
  TEST_ASSERT_EQUAL(1, rec->lastHelloRow);
  TEST_ASSERT_EQUAL(conn, core.rows[1].conn);
  TEST_ASSERT_TRUE(WallRowReach::Up == wallRowReach(core.rows[1].contact, 1001));
  TEST_ASSERT_TRUE(rec->closed.empty());
}

static void test_a_row_this_master_does_not_know_is_closed_without_a_welcome() {
  const int conn = joined("row-z", 1000);
  TEST_ASSERT_TRUE(rec->written.empty());
  TEST_ASSERT_EQUAL(1, rec->closed.size());
  TEST_ASSERT_EQUAL(conn, rec->closed[0]);
  TEST_ASSERT_EQUAL(0, rec->hellos);
  TEST_ASSERT_FALSE(core.conns[conn].open);
}

static void test_another_protocol_is_closed() {
  const int conn = core.accept(0, *rec);
  feed(conn, hello("row-a", 7, WALL_LINK_PROTOCOL + 1), 0);
  TEST_ASSERT_EQUAL(1, rec->closed.size());
  TEST_ASSERT_TRUE(rec->written.empty());
}

static void test_anything_before_hello_closes_the_connection() {
  const int conn = core.accept(0, *rec);
  feed(conn, plain(wl_ToMaster_pong_tag), 10);
  TEST_ASSERT_EQUAL(1, rec->closed.size());
  TEST_ASSERT_TRUE(rec->messages.empty());
}

static void test_a_connection_that_never_says_hello_is_closed_after_five_seconds() {
  const int conn = core.accept(1000, *rec);
  core.tick(5999, *rec);
  TEST_ASSERT_TRUE(rec->closed.empty());
  core.tick(6000, *rec);
  TEST_ASSERT_EQUAL(1, rec->closed.size());
  TEST_ASSERT_EQUAL(conn, rec->closed[0]);
  TEST_ASSERT_FALSE(core.conns[conn].open);
}

static void test_bytes_that_are_no_message_close_the_connection() {
  const int conn = core.accept(0, *rec);
  const uint8_t junk[] = {0xFF, 0xFF, 0xFF, 0xFF};
  core.bytes(conn, junk, sizeof junk, 5, *rec);
  TEST_ASSERT_EQUAL(1, rec->closed.size());
}

// Connections that say nothing must not keep the rows out: when there is no
// room, the one that has been nobody the longest makes way.
static void test_a_full_house_evicts_the_oldest_connection_that_is_nobody() {
  joined("row-a", 0);
  for (int i = 1; i < WALL_LINK_MAX_CONNS; i++) TEST_ASSERT_TRUE(core.accept(100 + i, *rec) >= 0);
  TEST_ASSERT_TRUE(rec->closed.empty());
  const int next = core.accept(500, *rec);
  TEST_ASSERT_EQUAL(1, next);  // the slot of the oldest nobody, opened at 101
  TEST_ASSERT_EQUAL(1, rec->closed.size());
  TEST_ASSERT_EQUAL(1, rec->closed[0]);
  TEST_ASSERT_EQUAL_UINT32(500, core.conns[next].openedAtMs);
  TEST_ASSERT_EQUAL(0, core.rows[0].conn);  // a welcomed row is never the one to go
}

static void test_a_house_full_of_rows_takes_no_more() {
  rec->known.clear();
  for (int i = 0; i < WALL_LINK_MAX_CONNS; i++) rec->known.push_back("row-" + std::to_string(i));
  for (int i = 0; i < WALL_LINK_MAX_ROWS; i++) joined(rec->known[i].c_str(), 0);
  TEST_ASSERT_TRUE(core.accept(0, *rec) >= 0);
  TEST_ASSERT_TRUE(core.accept(0, *rec) >= 0);
  // Ten open, eight of them rows: the next one evicts a nobody, not a row.
  const int next = core.accept(10, *rec);
  TEST_ASSERT_TRUE(next >= 0);
  for (int r = 0; r < WALL_LINK_MAX_ROWS; r++) TEST_ASSERT_TRUE(core.rows[r].conn >= 0);
}

// The id is no secret: it must not be enough to take a working row's place
// from somewhere else. (The row applies the same rule to masters.)
static void test_a_working_row_is_not_replaced_from_another_address() {
  const int first = joined("row-a", 1000);
  const int intruder = core.accept(2000, *rec);
  rec->peerOf[intruder] = 99;
  feed(intruder, hello("row-a"), 2000);
  TEST_ASSERT_EQUAL(1, rec->closed.size());
  TEST_ASSERT_EQUAL(intruder, rec->closed[0]);
  TEST_ASSERT_EQUAL(first, core.rows[0].conn);
  TEST_ASSERT_EQUAL(1, rec->count(wl_ToRow_welcome_tag));
  TEST_ASSERT_EQUAL(1, rec->hellos);
}

// A row whose address changed: its old connection falls silent and is closed
// at the lost mark, and the row's next dial is taken.
static void test_a_row_at_a_new_address_is_taken_once_the_old_connection_is_gone() {
  joined("row-a", 1000);
  core.tick(31000, *rec);  // nothing heard for 30 s: closed
  TEST_ASSERT_EQUAL(-1, core.rows[0].conn);
  const int moved = core.accept(32000, *rec);
  rec->peerOf[moved] = 99;
  feed(moved, hello("row-a"), 32000);
  TEST_ASSERT_EQUAL(moved, core.rows[0].conn);
}

// The row's TCP stack may answer while its program does not: a row that is
// not busy has to be heard.
static void test_a_silent_row_that_is_not_busy_is_closed_at_the_lost_mark() {
  const int conn = joined("row-a", 1000);
  core.tick(30999, *rec);
  TEST_ASSERT_TRUE(rec->closed.empty());
  core.tick(31000, *rec);
  TEST_ASSERT_EQUAL(1, rec->closed.size());
  TEST_ASSERT_EQUAL(conn, rec->closed[0]);
  TEST_ASSERT_TRUE(WallRowReach::Lost == wallRowReach(core.rows[0].contact, 31000));
}

static void test_a_silent_busy_row_keeps_its_connection() {
  const int conn = joined("row-a", 1000);
  feed(conn, status(true), 2000);
  core.tick(400000, *rec);
  TEST_ASSERT_TRUE(rec->closed.empty());
  TEST_ASSERT_EQUAL(conn, core.rows[0].conn);
}

// A row downloading its image reads and writes nothing on the link for about
// 25 s, and says so first.
static void test_a_row_downloading_an_image_counts_as_busy_until_it_says_otherwise() {
  const int conn = joined("row-a", 1000);
  wl_ToMaster m = plain(wl_ToMaster_update_state_tag);
  m.body.update_state.phase = wl_UpdatePhase_UPDATE_DOWNLOADING;
  feed(conn, m, 2000);
  core.tick(60000, *rec);
  TEST_ASSERT_TRUE(rec->closed.empty());
  TEST_ASSERT_EQUAL(0, rec->count(wl_ToRow_ping_tag));
  m.body.update_state.phase = wl_UpdatePhase_UPDATE_FAILED;
  feed(conn, m, 61000);
  TEST_ASSERT_FALSE(core.rows[0].contact.busy);
}

// Whoever holds a row's place from elsewhere (and may claim to be busy for
// ever) cannot keep out the board at the address the row was paired at.
static void test_the_paired_address_always_takes_its_rows_place() {
  const int squatter = core.accept(1000, *rec);
  rec->peerOf[squatter] = 99;
  feed(squatter, hello("row-a"), 1000);
  feed(squatter, status(true), 1100);
  TEST_ASSERT_EQUAL(squatter, core.rows[0].conn);  // nobody else was there
  const int real = joined("row-a", 5000);
  TEST_ASSERT_EQUAL(real, core.rows[0].conn);
  TEST_ASSERT_EQUAL(1, rec->closed.size());
  TEST_ASSERT_EQUAL(squatter, rec->closed[0]);
  // And the other way round it stays out.
  const int again = core.accept(6000, *rec);
  rec->peerOf[again] = 99;
  feed(again, hello("row-a"), 6000);
  TEST_ASSERT_EQUAL(real, core.rows[0].conn);
}

// A row that moved to a new address and restarts there replaces itself.
static void test_a_row_away_from_its_paired_address_still_replaces_itself() {
  const int first = core.accept(1000, *rec);
  rec->peerOf[first] = 50;
  feed(first, hello("row-a"), 1000);
  const int second = core.accept(2000, *rec);
  rec->peerOf[second] = 50;
  feed(second, hello("row-a", 8), 2000);
  TEST_ASSERT_EQUAL(second, core.rows[0].conn);
  TEST_ASSERT_EQUAL(1, rec->restarts);
}

static void test_a_first_message_that_cannot_be_read_closes_the_connection() {
  const int conn = core.accept(0, *rec);
  // A Hello whose id is longer than the field: nanopb refuses it.
  const uint8_t frame[] = {44, 0x0A, 42, 0x12, 40, 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a',
                           'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a',
                           'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a'};
  core.bytes(conn, frame, sizeof frame, 5, *rec);
  TEST_ASSERT_EQUAL(1, rec->closed.size());
  TEST_ASSERT_FALSE(core.conns[conn].open);
}

// From a welcomed row it is a newer build's message, and skipped.
static void test_a_later_message_that_cannot_be_read_is_skipped() {
  const int conn = joined("row-a", 1000);
  const uint8_t frame[] = {3, 0x12, 1, 0x80};  // a Status cut short
  core.bytes(conn, frame, sizeof frame, 1500, *rec);
  TEST_ASSERT_TRUE(rec->closed.empty());
  feed(conn, plain(wl_ToMaster_pong_tag), 2000);
  TEST_ASSERT_TRUE(core.conns[conn].open);
}

// ---- one connection per row ----------------------------------------------------

// A row that restarts dials again while the master still holds its old
// connection: the new one wins.
static void test_a_second_connection_of_a_row_replaces_the_first() {
  const int first = joined("row-a", 1000);
  const int second = joined("row-a", 2000);
  TEST_ASSERT_NOT_EQUAL(first, second);
  TEST_ASSERT_EQUAL(1, rec->closed.size());
  TEST_ASSERT_EQUAL(first, rec->closed[0]);
  TEST_ASSERT_EQUAL(second, core.rows[0].conn);
  TEST_ASSERT_TRUE(core.rows[0].contact.connected);
  TEST_ASSERT_EQUAL(2, rec->count(wl_ToRow_welcome_tag));
  TEST_ASSERT_EQUAL(0, rec->restarts);  // same boot id: it only redialled
}

static void test_a_new_boot_id_is_reported_as_a_restart() {
  joined("row-a", 1000, 7);
  joined("row-a", 2000, 8);
  TEST_ASSERT_EQUAL(1, rec->restarts);
}

static void test_a_second_hello_on_a_connection_closes_it() {
  const int conn = joined("row-a", 1000);
  feed(conn, hello("row-b"), 1500);
  TEST_ASSERT_EQUAL(1, rec->closed.size());
  TEST_ASSERT_EQUAL(-1, core.rows[0].conn);
  TEST_ASSERT_EQUAL(-1, core.rows[1].conn);
}

// ---- contact -------------------------------------------------------------------

static void test_a_closed_socket_leaves_the_row_away_then_lost() {
  const int conn = joined("row-a", 1000);
  core.closed(conn);
  TEST_ASSERT_EQUAL(-1, core.rows[0].conn);
  TEST_ASSERT_FALSE(core.conns[conn].open);
  TEST_ASSERT_TRUE(WallRowReach::Away == wallRowReach(core.rows[0].contact, 2000));
  TEST_ASSERT_TRUE(WallRowReach::Lost == wallRowReach(core.rows[0].contact, 31000));
  TEST_ASSERT_TRUE(rec->closed.empty());  // the socket was gone already
}

static void test_every_message_is_contact_and_is_handed_on_except_pong() {
  const int conn = joined("row-a", 1000);
  feed(conn, plain(wl_ToMaster_pong_tag), 20000);
  TEST_ASSERT_TRUE(rec->messages.empty());
  TEST_ASSERT_TRUE(WallRowReach::Up == wallRowReach(core.rows[0].contact, 49999));
  feed(conn, status(false), 40000);
  TEST_ASSERT_EQUAL(1, rec->messages.size());
  TEST_ASSERT_EQUAL(wl_ToMaster_status_tag, rec->messages[0]);
  TEST_ASSERT_TRUE(WallRowReach::Up == wallRowReach(core.rows[0].contact, 69999));
}

static void test_ping_goes_to_an_idle_connection_only_and_never_to_a_busy_row() {
  const int conn = joined("row-a", 1000);
  core.tick(10999, *rec);
  TEST_ASSERT_EQUAL(0, rec->count(wl_ToRow_ping_tag));
  core.tick(11000, *rec);
  TEST_ASSERT_EQUAL(1, rec->count(wl_ToRow_ping_tag));
  core.tick(11020, *rec);
  TEST_ASSERT_EQUAL(1, rec->count(wl_ToRow_ping_tag));
  feed(conn, status(true), 12000);
  core.tick(60000, *rec);
  TEST_ASSERT_EQUAL(1, rec->count(wl_ToRow_ping_tag));
  TEST_ASSERT_TRUE(WallRowReach::Busy == wallRowReach(core.rows[0].contact, 60000));
  feed(conn, status(false), 61000);
  core.tick(61020, *rec);
  TEST_ASSERT_EQUAL(2, rec->count(wl_ToRow_ping_tag));
}

// ---- text ----------------------------------------------------------------------

static void test_a_text_is_shown_at_the_next_pass_and_only_the_latest() {
  joined("row-a", 1000);
  core.setText(0, "FIRST", 80, 5000);
  core.setText(0, "SECOND", 80, 6000);
  core.tick(1020, *rec);
  TEST_ASSERT_EQUAL(1, rec->count(wl_ToRow_show_tag));
  TEST_ASSERT_EQUAL_STRING("SECOND", rec->last().body.show.text);
  TEST_ASSERT_EQUAL_UINT32(2, rec->last().body.show.render_id);
  TEST_ASSERT_EQUAL_UINT32(80, rec->last().body.show.speed);
  TEST_ASSERT_TRUE(6000 == rec->last().body.show.commit_at_ms);
  core.tick(1040, *rec);
  TEST_ASSERT_EQUAL(1, rec->count(wl_ToRow_show_tag));
}

static void test_a_text_for_a_row_that_is_away_is_shown_when_it_connects() {
  core.setText(0, "LATER", 80, 5000);
  core.tick(500, *rec);
  TEST_ASSERT_TRUE(rec->written.empty());
  joined("row-a", 1000);
  core.tick(1020, *rec);
  TEST_ASSERT_EQUAL(1, rec->count(wl_ToRow_show_tag));
  TEST_ASSERT_EQUAL_STRING("LATER", rec->last().body.show.text);
  TEST_ASSERT_TRUE(0 == rec->last().body.show.commit_at_ms);  // long past: flip on arrival
}

static void test_shown_marks_the_text_applied() {
  const int conn = joined("row-a", 1000);
  core.setText(0, "HELLO", 80, 0);
  core.tick(1020, *rec);
  TEST_ASSERT_FALSE(wallRowTextApplied(core.rows[0].text));
  wl_ToMaster m = plain(wl_ToMaster_shown_tag);
  m.body.shown.render_id = rec->last().body.show.render_id;
  feed(conn, m, 5000);
  TEST_ASSERT_TRUE(wallRowTextApplied(core.rows[0].text));
}

static void test_a_text_is_held_while_the_row_is_busy_and_a_ping_is_not_sent_with_it() {
  const int conn = joined("row-a", 1000);
  feed(conn, status(true), 1100);
  core.setText(0, "WAIT", 80, 0);
  core.tick(20000, *rec);
  TEST_ASSERT_EQUAL(2, rec->written.size());  // Welcome and quiet, at the Hello
  feed(conn, status(false), 21000);
  core.tick(21020, *rec);
  TEST_ASSERT_EQUAL(1, rec->count(wl_ToRow_show_tag));
  TEST_ASSERT_EQUAL(0, rec->count(wl_ToRow_ping_tag));  // the Show was the contact
}

// The row's receive window is full (it is not reading): the socket takes
// nothing, and nothing piles up for it.
static void test_a_connection_that_takes_nothing_keeps_the_text_for_later() {
  joined("row-a", 1000);
  core.setText(0, "ONE", 80, 0);
  rec->refuseWrites = true;
  core.tick(1020, *rec);
  core.setText(0, "TWO", 80, 0);
  core.tick(1040, *rec);
  TEST_ASSERT_EQUAL(0, rec->count(wl_ToRow_show_tag));
  rec->refuseWrites = false;
  core.tick(1060, *rec);
  TEST_ASSERT_EQUAL(1, rec->count(wl_ToRow_show_tag));
  TEST_ASSERT_EQUAL_STRING("TWO", rec->last().body.show.text);
}

static void test_a_welcome_that_cannot_be_written_closes_the_connection() {
  rec->refuseWrites = true;
  const int conn = joined("row-a", 1000);
  TEST_ASSERT_EQUAL(1, rec->closed.size());
  TEST_ASSERT_EQUAL(conn, rec->closed[0]);
  TEST_ASSERT_EQUAL(-1, core.rows[0].conn);
  TEST_ASSERT_EQUAL(0, rec->hellos);
}

static void test_a_message_to_a_row_goes_out_only_while_it_is_connected() {
  wl_ToRow m;
  wlClear(m);
  m.which_body = wl_ToRow_quiet_tag;
  m.body.quiet.on = true;
  TEST_ASSERT_FALSE(core.send(0, m, 500, *rec));
  joined("row-a", 1000);
  TEST_ASSERT_TRUE(core.send(0, m, 1500, *rec));
  TEST_ASSERT_EQUAL(wl_ToRow_quiet_tag, rec->last().which_body);
  // It counts as writing: the ping waits another ten seconds.
  core.tick(11000, *rec);
  TEST_ASSERT_EQUAL(0, rec->count(wl_ToRow_ping_tag));
  core.tick(11500, *rec);
  TEST_ASSERT_EQUAL(1, rec->count(wl_ToRow_ping_tag));
}

// ---- quiet and settings ----------------------------------------------------------

static wl_Config config(const char* tz, bool unitsAtStart = false) {
  wl_Config c = wl_Config_init_zero;
  c.fallback = wl_Fallback_FALLBACK_TIME;
  c.update_units_at_start = unitsAtStart;
  strcpy(c.tz, tz);
  return c;
}

// A row that connects is told how to behave before it is given a text.
static void test_a_connecting_row_gets_its_settings_then_quiet_then_the_text() {
  core.setConfig(config("CET-1CEST,M3.5.0,M10.5.0/3"));
  core.setQuiet(true);
  core.setText(0, "HELLO", 80, 0);
  joined("row-a", 1000);
  TEST_ASSERT_EQUAL(3, rec->written.size());  // all of it at the Hello, the text at the next pass
  core.tick(1020, *rec);
  TEST_ASSERT_EQUAL(4, rec->written.size());
  TEST_ASSERT_EQUAL(wl_ToRow_welcome_tag, rec->written[0].second.which_body);
  TEST_ASSERT_EQUAL(wl_ToRow_config_tag, rec->written[1].second.which_body);
  TEST_ASSERT_EQUAL_STRING("CET-1CEST,M3.5.0,M10.5.0/3", rec->written[1].second.body.config.tz);
  TEST_ASSERT_EQUAL(wl_Fallback_FALLBACK_TIME, rec->written[1].second.body.config.fallback);
  TEST_ASSERT_EQUAL(wl_ToRow_quiet_tag, rec->written[2].second.which_body);
  TEST_ASSERT_TRUE(rec->written[2].second.body.quiet.on);
  TEST_ASSERT_EQUAL(wl_ToRow_show_tag, rec->written[3].second.which_body);
}

static void test_a_change_of_quiet_or_settings_reaches_every_connected_row_once() {
  joined("row-a", 1000);
  joined("row-b", 1000);
  for (uint32_t t = 1020; t <= 1100; t += 20) core.tick(t, *rec);
  TEST_ASSERT_EQUAL(2, rec->count(wl_ToRow_quiet_tag));  // the state at connect: not quiet
  TEST_ASSERT_EQUAL(0, rec->count(wl_ToRow_config_tag));  // none was ever set
  core.setQuiet(true);
  core.setQuiet(true);
  core.setConfig(config("UTC0"));
  core.setConfig(config("UTC0"));
  for (uint32_t t = 1120; t <= 1300; t += 20) core.tick(t, *rec);
  TEST_ASSERT_EQUAL(4, rec->count(wl_ToRow_quiet_tag));
  TEST_ASSERT_EQUAL(2, rec->count(wl_ToRow_config_tag));
  core.setConfig(config("UTC0", true));
  for (uint32_t t = 1320; t <= 1400; t += 20) core.tick(t, *rec);
  TEST_ASSERT_EQUAL(4, rec->count(wl_ToRow_config_tag));
}

static void test_quiet_and_settings_wait_for_a_busy_row_and_are_sent_again_after_a_redial() {
  const int conn = joined("row-a", 1000);
  feed(conn, status(true), 1200);
  core.setQuiet(true);
  core.tick(1300, *rec);
  TEST_ASSERT_EQUAL(1, rec->count(wl_ToRow_quiet_tag));
  feed(conn, status(false), 1400);
  core.tick(1420, *rec);
  TEST_ASSERT_EQUAL(2, rec->count(wl_ToRow_quiet_tag));
  core.closed(conn);
  joined("row-a", 5000);
  for (uint32_t t = 5020; t <= 5100; t += 20) core.tick(t, *rec);
  TEST_ASSERT_EQUAL(3, rec->count(wl_ToRow_quiet_tag));
  TEST_ASSERT_TRUE(rec->last().body.quiet.on);
}

static void test_a_setting_the_socket_did_not_take_is_tried_again() {
  joined("row-a", 1000);
  rec->refuseWrites = true;
  core.setQuiet(true);
  core.tick(1020, *rec);
  rec->refuseWrites = false;
  for (uint32_t t = 1040; t <= 1100; t += 20) core.tick(t, *rec);
  TEST_ASSERT_EQUAL(2, rec->count(wl_ToRow_quiet_tag));  // "off" at the Hello, "on" once it fits
  TEST_ASSERT_TRUE(rec->last().body.quiet.on);
}

// A changed rows table renumbers the rows: every connection goes, and the rows
// dial again into the new table.
static void test_reset_closes_everything() {
  joined("row-a", 1000);
  joined("row-b", 1000);
  core.setText(0, "GONE", 80, 0);
  core.reset(*rec);
  TEST_ASSERT_EQUAL(2, rec->closed.size());
  TEST_ASSERT_EQUAL(-1, core.rows[0].conn);
  TEST_ASSERT_EQUAL_UINT32(0, core.rows[0].text.renderId);
  TEST_ASSERT_TRUE(WallRowReach::Never == wallRowReach(core.rows[0].contact, 2000));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_known_row_is_welcomed_by_name);
  RUN_TEST(test_a_row_this_master_does_not_know_is_closed_without_a_welcome);
  RUN_TEST(test_another_protocol_is_closed);
  RUN_TEST(test_anything_before_hello_closes_the_connection);
  RUN_TEST(test_a_connection_that_never_says_hello_is_closed_after_five_seconds);
  RUN_TEST(test_bytes_that_are_no_message_close_the_connection);
  RUN_TEST(test_a_full_house_evicts_the_oldest_connection_that_is_nobody);
  RUN_TEST(test_a_house_full_of_rows_takes_no_more);
  RUN_TEST(test_a_working_row_is_not_replaced_from_another_address);
  RUN_TEST(test_a_row_at_a_new_address_is_taken_once_the_old_connection_is_gone);
  RUN_TEST(test_a_silent_row_that_is_not_busy_is_closed_at_the_lost_mark);
  RUN_TEST(test_a_silent_busy_row_keeps_its_connection);
  RUN_TEST(test_a_row_downloading_an_image_counts_as_busy_until_it_says_otherwise);
  RUN_TEST(test_the_paired_address_always_takes_its_rows_place);
  RUN_TEST(test_a_row_away_from_its_paired_address_still_replaces_itself);
  RUN_TEST(test_a_first_message_that_cannot_be_read_closes_the_connection);
  RUN_TEST(test_a_later_message_that_cannot_be_read_is_skipped);
  RUN_TEST(test_a_second_connection_of_a_row_replaces_the_first);
  RUN_TEST(test_a_new_boot_id_is_reported_as_a_restart);
  RUN_TEST(test_a_second_hello_on_a_connection_closes_it);
  RUN_TEST(test_a_closed_socket_leaves_the_row_away_then_lost);
  RUN_TEST(test_every_message_is_contact_and_is_handed_on_except_pong);
  RUN_TEST(test_ping_goes_to_an_idle_connection_only_and_never_to_a_busy_row);
  RUN_TEST(test_a_text_is_shown_at_the_next_pass_and_only_the_latest);
  RUN_TEST(test_a_text_for_a_row_that_is_away_is_shown_when_it_connects);
  RUN_TEST(test_shown_marks_the_text_applied);
  RUN_TEST(test_a_text_is_held_while_the_row_is_busy_and_a_ping_is_not_sent_with_it);
  RUN_TEST(test_a_connection_that_takes_nothing_keeps_the_text_for_later);
  RUN_TEST(test_a_welcome_that_cannot_be_written_closes_the_connection);
  RUN_TEST(test_a_message_to_a_row_goes_out_only_while_it_is_connected);
  RUN_TEST(test_a_connecting_row_gets_its_settings_then_quiet_then_the_text);
  RUN_TEST(test_a_change_of_quiet_or_settings_reaches_every_connected_row_once);
  RUN_TEST(test_quiet_and_settings_wait_for_a_busy_row_and_are_sent_again_after_a_redial);
  RUN_TEST(test_a_setting_the_socket_did_not_take_is_tried_again);
  RUN_TEST(test_reset_closes_everything);
  return UNITY_END();
}
