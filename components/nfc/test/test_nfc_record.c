/*
 * NFC record parser test suite
 *
 * The card supplies every byte of a record header, so this suite is mostly
 * about what the parser refuses. Each malformed header below is something a
 * hostile or half-written tag can present.
 *
 * Compile with: gcc -o test_nfc_record test_nfc_record.c ../src/nfc_record.c
 * -I../src Run: ./test_nfc_record
 */

#include "nfc_record.h"
#include <stdio.h>
#include <string.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) printf("Testing: %s... ", name)
#define PASS()                                                                 \
  do {                                                                         \
    printf("PASS\n");                                                          \
    tests_passed++;                                                            \
  } while (0)
#define FAIL(msg)                                                              \
  do {                                                                         \
    printf("FAIL: %s\n", msg);                                                 \
    tests_failed++;                                                            \
  } while (0)
#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (cond) {                                                                \
      PASS();                                                                  \
    } else {                                                                   \
      FAIL(msg);                                                               \
    }                                                                          \
  } while (0)

/* A 1K MIFARE Classic clamped to one record's worth of addressable bytes. */
#define CAPACITY 720

static const uint8_t PAYLOAD[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
                                  0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C,
                                  0x0D, 0x0E, 0x0F, 0x10};
#define PAYLOAD_LEN (sizeof(PAYLOAD))

/* Build a valid header for PAYLOAD, for tests to then corrupt. */
static void good_header(uint8_t header[NFC_HEADER_LEN]) {
  nfc_record_err_t err =
      nfc_record_build(header, PAYLOAD, PAYLOAD_LEN, CAPACITY);
  if (err != NFC_RECORD_OK) {
    printf("FATAL: could not build a valid header (%s)\n",
           nfc_record_err_str(err));
  }
}

static void set_len(uint8_t header[NFC_HEADER_LEN], uint16_t len) {
  header[6] = (uint8_t)(len >> 8);
  header[7] = (uint8_t)(len & 0xFF);
}

/* ---------- CRC ---------- */

static void test_crc32(void) {
  TEST("CRC-32 check value");
  /* The standard CRC-32/ISO-HDLC check value for "123456789". */
  uint32_t crc = nfc_record_crc32((const uint8_t *)"123456789", 9);
  CHECK(crc == 0xCBF43926u, "check value mismatch");

  TEST("CRC-32 of empty input");
  CHECK(nfc_record_crc32((const uint8_t *)"", 0) == 0u, "expected 0");

  TEST("CRC-32 detects a single flipped bit");
  uint8_t a[8] = {0}, b[8] = {0};
  b[3] ^= 0x01;
  CHECK(nfc_record_crc32(a, 8) != nfc_record_crc32(b, 8), "collision");
}

/* ---------- Round trip ---------- */

static void test_round_trip(void) {
  uint8_t header[NFC_HEADER_LEN];
  nfc_record_t rec;

  TEST("build accepts a normal payload");
  CHECK(nfc_record_build(header, PAYLOAD, PAYLOAD_LEN, CAPACITY) ==
            NFC_RECORD_OK,
        "build refused a valid payload");

  TEST("parse accepts what build produced");
  CHECK(nfc_record_parse(header, CAPACITY, &rec) == NFC_RECORD_OK,
        "parse refused its own header");

  TEST("parsed fields survive the round trip");
  CHECK(rec.type == NFC_RECORD_KEF && rec.payload_len == PAYLOAD_LEN,
        "fields do not match");

  TEST("verify accepts the matching payload");
  CHECK(nfc_record_verify(&rec, PAYLOAD, PAYLOAD_LEN) == NFC_RECORD_OK,
        "verify refused a good payload");

  TEST("maximum-size payload round trips");
  static uint8_t big[NFC_RECORD_MAX_PAYLOAD];
  for (size_t i = 0; i < sizeof(big); i++)
    big[i] = (uint8_t)i;
  CHECK(nfc_record_build(header, big, sizeof(big), CAPACITY) == NFC_RECORD_OK &&
            nfc_record_parse(header, CAPACITY, &rec) == NFC_RECORD_OK &&
            rec.payload_len == sizeof(big),
        "largest allowed payload was refused");
}

/* ---------- Hostile headers ---------- */

static void test_bad_magic(void) {
  uint8_t header[NFC_HEADER_LEN];
  nfc_record_t rec;

  TEST("blank card is not a record");
  memset(header, 0, sizeof(header));
  CHECK(nfc_record_parse(header, CAPACITY, &rec) == NFC_RECORD_ERR_MAGIC,
        "accepted an all-zero header");

  TEST("erased card is not a record");
  memset(header, 0xFF, sizeof(header));
  CHECK(nfc_record_parse(header, CAPACITY, &rec) == NFC_RECORD_ERR_MAGIC,
        "accepted an all-ones header");

  TEST("near-miss magic is refused");
  good_header(header);
  header[3] = '2';
  CHECK(nfc_record_parse(header, CAPACITY, &rec) == NFC_RECORD_ERR_MAGIC,
        "accepted KRN2");
}

static void test_bad_type(void) {
  uint8_t header[NFC_HEADER_LEN];
  nfc_record_t rec;

  TEST("unknown record type is refused");
  good_header(header);
  header[4] = 2;
  CHECK(nfc_record_parse(header, CAPACITY, &rec) == NFC_RECORD_ERR_TYPE,
        "accepted an unknown type");

  TEST("record type zero is refused");
  good_header(header);
  header[4] = 0;
  CHECK(nfc_record_parse(header, CAPACITY, &rec) == NFC_RECORD_ERR_TYPE,
        "accepted type 0");
}

static void test_reserved_bytes(void) {
  const int reserved[] = {5, 12, 13, 14, 15};

  for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
    uint8_t header[NFC_HEADER_LEN];
    nfc_record_t rec;
    char name[64];

    snprintf(name, sizeof(name), "reserved byte %d must be zero", reserved[i]);
    TEST(name);
    good_header(header);
    header[reserved[i]] = 0x01;
    CHECK(nfc_record_parse(header, CAPACITY, &rec) == NFC_RECORD_ERR_RESERVED,
          "accepted a non-zero reserved byte");
  }
}

static void test_bad_length(void) {
  uint8_t header[NFC_HEADER_LEN];
  nfc_record_t rec;

  TEST("zero length is refused");
  good_header(header);
  set_len(header, 0);
  CHECK(nfc_record_parse(header, CAPACITY, &rec) == NFC_RECORD_ERR_LENGTH,
        "accepted length 0");

  TEST("0xFFFF length is refused");
  good_header(header);
  set_len(header, 0xFFFF);
  CHECK(nfc_record_parse(header, CAPACITY, &rec) == NFC_RECORD_ERR_LENGTH,
        "accepted length 0xFFFF");

  TEST("length past the compile-time ceiling is refused");
  good_header(header);
  set_len(header, NFC_RECORD_MAX_PAYLOAD + 1);
  CHECK(nfc_record_parse(header, CAPACITY, &rec) == NFC_RECORD_ERR_LENGTH,
        "accepted a payload over the ceiling");

  TEST("length past what the tag holds is refused");
  good_header(header);
  set_len(header, 200);
  /* A small NTAG: 144 bytes total, so 200 bytes of payload cannot be there
     even though it is under the ceiling. */
  CHECK(nfc_record_parse(header, 144, &rec) == NFC_RECORD_ERR_LENGTH,
        "accepted a payload larger than the tag");

  TEST("payload exactly filling the tag is accepted");
  good_header(header);
  set_len(header, 144 - NFC_HEADER_LEN);
  CHECK(nfc_record_parse(header, 144, &rec) == NFC_RECORD_OK,
        "refused a payload that fits exactly");

  TEST("one byte past a full tag is refused");
  good_header(header);
  set_len(header, 144 - NFC_HEADER_LEN + 1);
  CHECK(nfc_record_parse(header, 144, &rec) == NFC_RECORD_ERR_LENGTH,
        "off-by-one accepted");

  TEST("capacity smaller than the header is refused");
  good_header(header);
  CHECK(nfc_record_parse(header, 8, &rec) == NFC_RECORD_ERR_LENGTH,
        "accepted a capacity below the header size");

  TEST("zero capacity is refused");
  good_header(header);
  CHECK(nfc_record_parse(header, 0, &rec) == NFC_RECORD_ERR_LENGTH,
        "accepted zero capacity");
}

static void test_bad_payload(void) {
  uint8_t header[NFC_HEADER_LEN];
  nfc_record_t rec;
  uint8_t corrupt[PAYLOAD_LEN];

  good_header(header);
  if (nfc_record_parse(header, CAPACITY, &rec) != NFC_RECORD_OK) {
    printf("FATAL: valid header did not parse\n");
    return;
  }

  TEST("flipped payload bit fails the checksum");
  memcpy(corrupt, PAYLOAD, PAYLOAD_LEN);
  corrupt[7] ^= 0x01;
  CHECK(nfc_record_verify(&rec, corrupt, PAYLOAD_LEN) == NFC_RECORD_ERR_CRC,
        "accepted a corrupted payload");

  TEST("payload shorter than the header claims is refused");
  CHECK(nfc_record_verify(&rec, PAYLOAD, PAYLOAD_LEN - 1) ==
            NFC_RECORD_ERR_LENGTH,
        "accepted a short payload");

  TEST("payload longer than the header claims is refused");
  CHECK(nfc_record_verify(&rec, PAYLOAD, PAYLOAD_LEN + 1) ==
            NFC_RECORD_ERR_LENGTH,
        "accepted a long payload");
}

static void test_build_limits(void) {
  uint8_t header[NFC_HEADER_LEN];
  static uint8_t big[NFC_RECORD_MAX_PAYLOAD + 1];

  TEST("build refuses an oversize payload");
  CHECK(nfc_record_build(header, big, sizeof(big), CAPACITY) ==
            NFC_RECORD_ERR_LENGTH,
        "built a record over the ceiling");

  TEST("build refuses a payload the tag cannot hold");
  CHECK(nfc_record_build(header, PAYLOAD, PAYLOAD_LEN, 20) ==
            NFC_RECORD_ERR_LENGTH,
        "built a record larger than the tag");

  TEST("build refuses an empty payload");
  CHECK(nfc_record_build(header, PAYLOAD, 0, CAPACITY) == NFC_RECORD_ERR_LENGTH,
        "built an empty record");
}

static void test_null_args(void) {
  uint8_t header[NFC_HEADER_LEN];
  nfc_record_t rec;

  TEST("parse rejects NULL arguments");
  CHECK(nfc_record_parse(NULL, CAPACITY, &rec) == NFC_RECORD_ERR_ARG &&
            nfc_record_parse(header, CAPACITY, NULL) == NFC_RECORD_ERR_ARG,
        "NULL accepted");

  TEST("verify rejects NULL arguments");
  CHECK(nfc_record_verify(NULL, PAYLOAD, PAYLOAD_LEN) == NFC_RECORD_ERR_ARG &&
            nfc_record_verify(&rec, NULL, PAYLOAD_LEN) == NFC_RECORD_ERR_ARG,
        "NULL accepted");

  TEST("build rejects NULL arguments");
  CHECK(nfc_record_build(NULL, PAYLOAD, PAYLOAD_LEN, CAPACITY) ==
                NFC_RECORD_ERR_ARG &&
            nfc_record_build(header, NULL, PAYLOAD_LEN, CAPACITY) ==
                NFC_RECORD_ERR_ARG,
        "NULL accepted");
}

int main(void) {
  printf("=== NFC record parser tests ===\n\n");

  test_crc32();
  test_round_trip();
  test_bad_magic();
  test_bad_type();
  test_reserved_bytes();
  test_bad_length();
  test_bad_payload();
  test_build_limits();
  test_null_args();

  printf("\nPassed: %d, Failed: %d\n", tests_passed, tests_failed);
  return tests_failed == 0 ? 0 : 1;
}
