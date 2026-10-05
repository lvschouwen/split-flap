// Host-side tests for who may become this row's master (POST /pair).

#include <unity.h>

#include <cstring>

#include "../../FollowerPairPolicy.h"
#include "../../FollowerSettings.h"

void setUp() {}
void tearDown() {}

// The row's present state, as the handler reads it.
static FollowerPairing paired(const char* id, const char* host, bool inContact,
                              bool lost) {
  FollowerPairing p;
  p.paired = true;
  p.masterId = id;
  p.masterHost = host;
  p.masterInContact = inContact;
  p.masterLost = lost;
  return p;
}

static PairVerdict decide(const char* id, const char* host, const FollowerPairing& now) {
  return followerPairDecide(id, host, now);
}

static void test_unpaired_row_takes_the_first_master() {
  TEST_ASSERT_TRUE(PairVerdict::Store ==
                   decide("master-a", "10.0.0.2", FollowerPairing()));
}

static void test_its_own_master_may_pair_again_and_nothing_changes() {
  for (int state = 0; state < 3; state++) {
    TEST_ASSERT_TRUE(PairVerdict::Same ==
                     decide("master-a", "10.0.0.2",
                            paired("master-a", "10.0.0.2", state == 0, state == 2)));
  }
}

static void test_its_masters_id_from_another_address_is_refused_while_the_master_talks() {
  // The id is no secret (the row serves it, the master advertises it): a
  // caller that only knows it must not be able to move a working row's link.
  TEST_ASSERT_TRUE(PairVerdict::Refuse ==
                   decide("master-a", "10.0.0.66",
                          paired("master-a", "10.0.0.2", true, false)));
}

static void test_its_master_at_a_new_address_is_taken_once_the_old_one_is_silent() {
  // A master whose address moved: the link to the old one has gone quiet.
  TEST_ASSERT_TRUE(PairVerdict::Store ==
                   decide("master-a", "10.0.0.9",
                          paired("master-a", "10.0.0.2", false, false)));
  TEST_ASSERT_TRUE(PairVerdict::Store ==
                   decide("master-a", "10.0.0.9",
                          paired("master-a", "10.0.0.2", false, true)));
}

static void test_another_master_is_refused_until_its_own_is_written_off() {
  TEST_ASSERT_TRUE(PairVerdict::Refuse ==
                   decide("master-b", "10.0.0.3",
                          paired("master-a", "10.0.0.2", true, false)));
  // Silent for a while (grace) is not yet written off.
  TEST_ASSERT_TRUE(PairVerdict::Refuse ==
                   decide("master-b", "10.0.0.3",
                          paired("master-a", "10.0.0.2", false, false)));
  // Not even from its own master's address.
  TEST_ASSERT_TRUE(PairVerdict::Refuse ==
                   decide("master-b", "10.0.0.2",
                          paired("master-a", "10.0.0.2", true, false)));
}

static void test_another_master_is_taken_once_its_own_is_written_off() {
  TEST_ASSERT_TRUE(PairVerdict::Store ==
                   decide("master-b", "10.0.0.3",
                          paired("master-a", "10.0.0.2", false, true)));
}

static void test_id_must_be_storable() {
  const FollowerPairing none;
  TEST_ASSERT_TRUE(PairVerdict::BadId == decide("", "10.0.0.2", none));
  TEST_ASSERT_TRUE(PairVerdict::BadId == decide(nullptr, "10.0.0.2", none));
  TEST_ASSERT_TRUE(PairVerdict::BadId == decide("two words", "10.0.0.2", none));
  TEST_ASSERT_TRUE(PairVerdict::BadId == decide("tab\there", "10.0.0.2", none));
  TEST_ASSERT_TRUE(PairVerdict::BadId == decide("caf\xc3\xa9", "10.0.0.2", none));
  char longest[FOLLOWER_MASTER_ID_MAX + 2];
  memset(longest, 'a', sizeof(longest) - 1);
  longest[sizeof(longest) - 1] = 0;
  TEST_ASSERT_TRUE(PairVerdict::BadId == decide(longest, "10.0.0.2", none));
  longest[FOLLOWER_MASTER_ID_MAX] = 0;
  TEST_ASSERT_TRUE(PairVerdict::Store == decide(longest, "10.0.0.2", none));
  // A bad id is refused before anything else is weighed.
  TEST_ASSERT_TRUE(PairVerdict::BadId ==
                   decide("", "10.0.0.2", paired("master-a", "10.0.0.2", false, true)));
}

static void test_an_accepted_id_fits_the_record_and_the_welcome() {
  // The id is stored in the pairing record and compared with Welcome.
  TEST_ASSERT_EQUAL(FOLLOWER_NAME_MAX, FOLLOWER_MASTER_ID_MAX);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_unpaired_row_takes_the_first_master);
  RUN_TEST(test_its_own_master_may_pair_again_and_nothing_changes);
  RUN_TEST(test_its_masters_id_from_another_address_is_refused_while_the_master_talks);
  RUN_TEST(test_its_master_at_a_new_address_is_taken_once_the_old_one_is_silent);
  RUN_TEST(test_another_master_is_refused_until_its_own_is_written_off);
  RUN_TEST(test_another_master_is_taken_once_its_own_is_written_off);
  RUN_TEST(test_id_must_be_storable);
  RUN_TEST(test_an_accepted_id_fits_the_record_and_the_welcome);
  return UNITY_END();
}
