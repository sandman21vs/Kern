/*
 * descriptor_checksum_verify_text test suite
 *
 * This gate is the only integrity check an unsealed descriptor gets when it
 * arrives on a medium that carries none of its own — an NFC card. libwally
 * parses a descriptor whether or not its checksum agrees, and
 * descriptor_to_unambiguous() strips the checksum before the validator ever
 * sees it, so what is checked here is never checked anywhere else.
 *
 * Build/run: make -C main/core/test run
 */

#include "core/descriptor_checksum.h"
#include <stdio.h>
#include <string.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) printf("Testing: %s... ", name)
#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (cond) {                                                                \
      printf("PASS\n");                                                        \
      tests_passed++;                                                          \
    } else {                                                                   \
      printf("FAIL: %s\n", msg);                                               \
      tests_failed++;                                                          \
    }                                                                          \
  } while (0)

/* Checksums here were computed from the BIP-380 spec in Python, not from the
   code under test — a self-generated vector would pass by construction. */
#define GOOD                                                                   \
  "wpkh([d34db33f/84h/0h/0h]0279be667ef9dcbbac55a06295Ce870b07029Bf"           \
  "cdb2dce28d959f2815b16f81798/0/*)#qucct0zs"

static void test_accepts_a_good_descriptor(void) {
  TEST("a descriptor and its own checksum are accepted");
  CHECK(descriptor_checksum_verify_text(GOOD), "refused a valid checksum");
}

/*
 * The reason this function exists. Only the xpubs inside a descriptor defend
 * themselves, being base58check; a bit flipped anywhere else is accepted in
 * silence and points the wallet at different addresses.
 */
static void test_detects_body_corruption(void) {
  char buf[sizeof(GOOD)];

  TEST("a flipped bit in the derivation path is caught");
  memcpy(buf, GOOD, sizeof(GOOD));
  buf[strlen("wpkh([d34db33f/84h/0h/")] = '1'; /* 0h -> 1h */
  CHECK(!descriptor_checksum_verify_text(buf),
        "accepted a descriptor whose path had changed");

  TEST("a flipped bit in the fingerprint is caught");
  memcpy(buf, GOOD, sizeof(GOOD));
  buf[strlen("wpkh([d34db3")] = '4'; /* 3f -> 4f */
  CHECK(!descriptor_checksum_verify_text(buf),
        "accepted a descriptor whose fingerprint had changed");

  TEST("a changed script wrapper is caught");
  memcpy(buf, GOOD, sizeof(GOOD));
  buf[0] = 's'; /* wpkh -> spkh */
  CHECK(!descriptor_checksum_verify_text(buf),
        "accepted a descriptor whose wrapper had changed");
}

static void test_detects_checksum_corruption(void) {
  char buf[sizeof(GOOD)];

  TEST("a flipped character in the checksum is caught");
  memcpy(buf, GOOD, sizeof(GOOD));
  buf[strlen(GOOD) - 1] = (buf[strlen(GOOD) - 1] == 's') ? 't' : 's';
  CHECK(!descriptor_checksum_verify_text(buf), "accepted a wrong checksum");
}

static void test_malformed_tails(void) {
  char buf[sizeof(GOOD) + 8];

  TEST("a descriptor with no checksum is refused");
  memcpy(buf, GOOD, sizeof(GOOD));
  buf[strlen(GOOD) - 9] = '\0'; /* drop the "#xxxxxxxx" tail */
  CHECK(!descriptor_checksum_verify_text(buf),
        "accepted a descriptor carrying no checksum");

  TEST("a seven-character checksum is refused");
  memcpy(buf, GOOD, sizeof(GOOD));
  buf[strlen(GOOD) - 1] = '\0';
  CHECK(!descriptor_checksum_verify_text(buf), "accepted a short checksum");

  TEST("a nine-character checksum is refused");
  snprintf(buf, sizeof(buf), "%sq", GOOD);
  CHECK(!descriptor_checksum_verify_text(buf), "accepted a long checksum");

  TEST("a second '#' is refused rather than treated as the separator");
  snprintf(buf, sizeof(buf), "%s#qucct0zs", GOOD);
  CHECK(!descriptor_checksum_verify_text(buf),
        "split on the wrong '#' and accepted the descriptor");

  TEST("an empty body is refused");
  CHECK(!descriptor_checksum_verify_text("#qucct0zs"),
        "accepted a checksum with nothing to check");

  TEST("a bare '#' is refused");
  CHECK(!descriptor_checksum_verify_text("#"), "accepted a lone separator");

  TEST("an empty string is refused");
  CHECK(!descriptor_checksum_verify_text(""), "accepted an empty string");

  TEST("NULL is refused");
  CHECK(!descriptor_checksum_verify_text(NULL), "accepted NULL");
}

/* The checksum is defined over the string as written. Normalizing first would
   reject a descriptor spelled with "'" whose checksum is correct for that
   spelling — which is what another implementation may well hand us. */
static void test_literal_body_not_normalized(void) {
  TEST("a checksum over an apostrophe-hardened body is accepted as written");
  CHECK(descriptor_checksum_verify_text(
            "wpkh([d34db33f/84'/0'/0']0279be667ef9dcbbac55a06295Ce870b07029Bfc"
            "db2dce28d959f2815b16f81798/0/*)#7njm5dvd"),
        "refused a valid checksum over an apostrophe-spelled body");
}

int main(void) {
  printf("=== descriptor checksum verification tests ===\n\n");

  test_accepts_a_good_descriptor();
  test_detects_body_corruption();
  test_detects_checksum_corruption();
  test_malformed_tails();
  test_literal_body_not_normalized();

  printf("\nPassed: %d, Failed: %d\n", tests_passed, tests_failed);
  return tests_failed == 0 ? 0 : 1;
}
