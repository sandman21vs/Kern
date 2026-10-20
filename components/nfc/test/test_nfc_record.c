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

#define PAYLOAD_LEN 16

/* What a caller that does not care which type it gets asks for — the question
   "is there anything here", not "can I parse it". */
#define ANY NFC_RECORD_MASK_ANY_KNOWN

/* Build a valid header for tests to then corrupt. */
static void good_header(uint8_t header[NFC_HEADER_LEN]) {
  nfc_record_err_t err =
      nfc_record_build(header, PAYLOAD_LEN, CAPACITY, NFC_RECORD_KEF);
  if (err != NFC_RECORD_OK) {
    printf("FATAL: could not build a valid header (%s)\n",
           nfc_record_err_str(err));
  }
}

static void set_len(uint8_t header[NFC_HEADER_LEN], uint16_t len) {
  header[6] = (uint8_t)(len >> 8);
  header[7] = (uint8_t)(len & 0xFF);
}

/* ---------- Round trip ---------- */

static void test_round_trip(void) {
  uint8_t header[NFC_HEADER_LEN];
  nfc_record_t rec;

  TEST("build accepts a normal payload");
  CHECK(nfc_record_build(header, PAYLOAD_LEN, CAPACITY, NFC_RECORD_KEF) ==
            NFC_RECORD_OK,
        "build refused a valid payload");

  TEST("parse accepts what build produced");
  CHECK(nfc_record_parse(header, CAPACITY, ANY, &rec) == NFC_RECORD_OK,
        "parse refused its own header");

  TEST("parsed fields survive the round trip");
  CHECK(rec.type == NFC_RECORD_KEF && rec.payload_len == PAYLOAD_LEN,
        "fields do not match");

  TEST("maximum-size payload round trips");
  CHECK(nfc_record_build(header, NFC_RECORD_MAX_PAYLOAD, CAPACITY,
                         NFC_RECORD_KEF) == NFC_RECORD_OK &&
            nfc_record_parse(header, CAPACITY, ANY, &rec) == NFC_RECORD_OK &&
            rec.payload_len == NFC_RECORD_MAX_PAYLOAD,
        "largest allowed payload was refused");
}

/* ---------- Hostile headers ---------- */

static void test_bad_magic(void) {
  uint8_t header[NFC_HEADER_LEN];
  nfc_record_t rec;

  TEST("blank card is not a record");
  memset(header, 0, sizeof(header));
  CHECK(nfc_record_parse(header, CAPACITY, ANY, &rec) == NFC_RECORD_ERR_MAGIC,
        "accepted an all-zero header");

  TEST("erased card is not a record");
  memset(header, 0xFF, sizeof(header));
  CHECK(nfc_record_parse(header, CAPACITY, ANY, &rec) == NFC_RECORD_ERR_MAGIC,
        "accepted an all-ones header");

  TEST("near-miss magic is refused");
  good_header(header);
  header[3] = '2';
  CHECK(nfc_record_parse(header, CAPACITY, ANY, &rec) == NFC_RECORD_ERR_MAGIC,
        "accepted KRN2");
}

static void test_bad_type(void) {
  uint8_t header[NFC_HEADER_LEN];
  nfc_record_t rec;

  TEST("first unassigned record type is refused");
  good_header(header);
  header[4] = 5;
  CHECK(nfc_record_parse(header, CAPACITY, ANY, &rec) == NFC_RECORD_ERR_TYPE,
        "accepted an unassigned type");

  TEST("record type zero is refused");
  good_header(header);
  header[4] = 0;
  CHECK(nfc_record_parse(header, CAPACITY, ANY, &rec) == NFC_RECORD_ERR_TYPE,
        "accepted type 0");

  TEST("record type 0xFF is refused");
  good_header(header);
  header[4] = 0xFF;
  CHECK(nfc_record_parse(header, CAPACITY, ANY, &rec) == NFC_RECORD_ERR_TYPE,
        "accepted type 0xFF");
}

/* ---------- The type system ---------- */

static const uint8_t known_types[] = {NFC_RECORD_KEF, NFC_RECORD_DESCRIPTOR,
                                      NFC_RECORD_DATUM, NFC_RECORD_XPUB};

static void test_all_types_round_trip(void) {
  for (size_t i = 0; i < sizeof(known_types) / sizeof(known_types[0]); i++) {
    uint8_t header[NFC_HEADER_LEN];
    nfc_record_t rec;
    char name[64];

    snprintf(name, sizeof(name), "record type %u round trips",
             (unsigned)known_types[i]);
    TEST(name);
    CHECK(nfc_record_build(header, PAYLOAD_LEN, CAPACITY, known_types[i]) ==
                  NFC_RECORD_OK &&
              nfc_record_parse(header, CAPACITY, ANY, &rec) == NFC_RECORD_OK &&
              rec.type == known_types[i],
          "a known type did not survive build then parse");
  }
}

/*
 * The security property the type byte exists for: a card of the wrong kind is
 * refused at the header, so it never reaches a parser that was not written for
 * it — and never reaches a password prompt either.
 */
static void test_type_filtering(void) {
  uint8_t header[NFC_HEADER_LEN];
  nfc_record_t rec;

  TEST("a descriptor card cannot enter the mnemonic loader");
  nfc_record_build(header, PAYLOAD_LEN, CAPACITY, NFC_RECORD_DESCRIPTOR);
  CHECK(nfc_record_parse(header, CAPACITY, NFC_RECORD_BIT(NFC_RECORD_KEF),
                         &rec) == NFC_RECORD_ERR_TYPE,
        "a descriptor record passed a KEF-only reader");

  TEST("a seed card cannot enter the wallet");
  nfc_record_build(header, PAYLOAD_LEN, CAPACITY, NFC_RECORD_KEF);
  CHECK(nfc_record_parse(header, CAPACITY,
                         NFC_RECORD_BIT(NFC_RECORD_DESCRIPTOR),
                         &rec) == NFC_RECORD_ERR_TYPE,
        "a KEF record passed a descriptor-only reader");

  TEST("a reader naming two types accepts both");
  const uint32_t both =
      NFC_RECORD_BIT(NFC_RECORD_KEF) | NFC_RECORD_BIT(NFC_RECORD_DESCRIPTOR);
  nfc_record_build(header, PAYLOAD_LEN, CAPACITY, NFC_RECORD_KEF);
  int ok = nfc_record_parse(header, CAPACITY, both, &rec) == NFC_RECORD_OK;
  nfc_record_build(header, PAYLOAD_LEN, CAPACITY, NFC_RECORD_DESCRIPTOR);
  ok = ok && nfc_record_parse(header, CAPACITY, both, &rec) == NFC_RECORD_OK;
  CHECK(ok, "a two-type mask refused one of its own types");

  TEST("an empty mask accepts nothing");
  nfc_record_build(header, PAYLOAD_LEN, CAPACITY, NFC_RECORD_KEF);
  CHECK(nfc_record_parse(header, CAPACITY, 0, &rec) == NFC_RECORD_ERR_TYPE,
        "an empty mask accepted a record");
}

/* A caller cannot widen the allowlist past what this build knows, however
   enthusiastic its mask. */
static void test_mask_cannot_widen(void) {
  uint8_t header[NFC_HEADER_LEN];
  nfc_record_t rec;

  TEST("an all-ones mask does not legitimize an unassigned type");
  good_header(header);
  header[4] = 5;
  CHECK(nfc_record_parse(header, CAPACITY, 0xFFFFFFFFu, &rec) ==
            NFC_RECORD_ERR_TYPE,
        "0xFFFFFFFF accepted type 5");

  TEST("an all-ones mask does not legitimize a type above the mask width");
  good_header(header);
  header[4] = 200;
  CHECK(nfc_record_parse(header, CAPACITY, 0xFFFFFFFFu, &rec) ==
            NFC_RECORD_ERR_TYPE,
        "0xFFFFFFFF accepted type 200");
}

static void test_build_refuses_unknown_type(void) {
  uint8_t header[NFC_HEADER_LEN];
  const uint8_t bad[] = {0, 5, 31, 0xFF};

  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    char name[64];
    snprintf(name, sizeof(name), "build refuses record type %u",
             (unsigned)bad[i]);
    TEST(name);
    CHECK(nfc_record_build(header, PAYLOAD_LEN, CAPACITY, bad[i]) ==
              NFC_RECORD_ERR_TYPE,
          "built a record with an unknown type");
  }
}

/* The header comment promises magic, then type, then reserved, then length,
   stopping at the first divergence. A header wrong in two ways must report the
   earlier one. */
static void test_check_ordering(void) {
  uint8_t header[NFC_HEADER_LEN];
  nfc_record_t rec;

  TEST("type is checked before length");
  good_header(header);
  header[4] = 5;
  set_len(header, 0);
  CHECK(nfc_record_parse(header, CAPACITY, ANY, &rec) == NFC_RECORD_ERR_TYPE,
        "reported a length error ahead of a type error");

  TEST("magic is checked before type");
  good_header(header);
  header[0] = 'X';
  header[4] = 5;
  CHECK(nfc_record_parse(header, CAPACITY, ANY, &rec) == NFC_RECORD_ERR_MAGIC,
        "reported a type error ahead of a magic error");

  TEST("type is checked before reserved bytes");
  good_header(header);
  header[4] = 5;
  header[5] = 1;
  CHECK(nfc_record_parse(header, CAPACITY, ANY, &rec) == NFC_RECORD_ERR_TYPE,
        "reported a reserved error ahead of a type error");
}

/*
 * Byte-compatibility with the Krux fork that writes the same cards.
 *
 * These are exactly what Krux's build_header(length, capacity, record_type)
 * (src/krux/nfc.py) emits for the same arguments. Changing any byte below
 * breaks cards written by either firmware, so a diff here is a deliberate
 * format change and nothing less.
 *
 * DATUM and XPUB are build-only: Kern reserves those numbers and never writes
 * them from a page, which makes this the only thing in the tree keeping the
 * numbering aligned.
 */
static void test_golden_headers(void) {
  static const struct {
    const char *name;
    uint8_t type;
    size_t len;
    uint8_t bytes[NFC_HEADER_LEN];
  } vectors[] = {
      {"KEF/96",
       NFC_RECORD_KEF,
       96,
       {0x4B, 0x52, 0x4E, 0x31, 0x01, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00}},
      {"DESCRIPTOR/450",
       NFC_RECORD_DESCRIPTOR,
       450,
       {0x4B, 0x52, 0x4E, 0x31, 0x02, 0x00, 0x01, 0xC2, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00}},
      {"DATUM/704",
       NFC_RECORD_DATUM,
       704,
       {0x4B, 0x52, 0x4E, 0x31, 0x03, 0x00, 0x02, 0xC0, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00}},
      {"XPUB/111",
       NFC_RECORD_XPUB,
       111,
       {0x4B, 0x52, 0x4E, 0x31, 0x04, 0x00, 0x00, 0x6F, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00}},
  };

  for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
    uint8_t header[NFC_HEADER_LEN];
    nfc_record_t rec;
    char name[80];

    snprintf(name, sizeof(name), "golden header %s matches Krux",
             vectors[i].name);
    TEST(name);
    CHECK(nfc_record_build(header, vectors[i].len, CAPACITY, vectors[i].type) ==
                  NFC_RECORD_OK &&
              memcmp(header, vectors[i].bytes, NFC_HEADER_LEN) == 0,
          "built header diverges from the pinned bytes");

    snprintf(name, sizeof(name), "golden header %s parses back",
             vectors[i].name);
    TEST(name);
    CHECK(nfc_record_parse(vectors[i].bytes, CAPACITY, ANY, &rec) ==
                  NFC_RECORD_OK &&
              rec.type == vectors[i].type && rec.payload_len == vectors[i].len,
          "a pinned header did not parse back to its own fields");
  }
}

/* The wire format in constants. A silent divergence here would only otherwise
   show up in a card handed between two firmwares. */
static void test_format_constants(void) {
  TEST("header length is 16");
  CHECK(NFC_HEADER_LEN == 16, "header length changed");

  TEST("payload ceiling is 704");
  CHECK(NFC_RECORD_MAX_PAYLOAD == 704, "payload ceiling changed");

  TEST("magic is KRN1");
  CHECK(memcmp(NFC_RECORD_MAGIC, "KRN1", NFC_RECORD_MAGIC_LEN) == 0,
        "magic changed");

  TEST("the known-type allowlist is types 1 through 4");
  CHECK(NFC_RECORD_MASK_ANY_KNOWN == 0x1Eu, "the type allowlist changed");
}

static void test_reserved_bytes(void) {
  const int reserved[] = {5, 8, 9, 10, 11, 12, 13, 14, 15};

  for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
    uint8_t header[NFC_HEADER_LEN];
    nfc_record_t rec;
    char name[64];

    snprintf(name, sizeof(name), "reserved byte %d must be zero", reserved[i]);
    TEST(name);
    good_header(header);
    header[reserved[i]] = 0x01;
    CHECK(nfc_record_parse(header, CAPACITY, ANY, &rec) ==
              NFC_RECORD_ERR_RESERVED,
          "accepted a non-zero reserved byte");
  }
}

static void test_bad_length(void) {
  uint8_t header[NFC_HEADER_LEN];
  nfc_record_t rec;

  TEST("zero length is refused");
  good_header(header);
  set_len(header, 0);
  CHECK(nfc_record_parse(header, CAPACITY, ANY, &rec) == NFC_RECORD_ERR_LENGTH,
        "accepted length 0");

  TEST("0xFFFF length is refused");
  good_header(header);
  set_len(header, 0xFFFF);
  CHECK(nfc_record_parse(header, CAPACITY, ANY, &rec) == NFC_RECORD_ERR_LENGTH,
        "accepted length 0xFFFF");

  TEST("length past the compile-time ceiling is refused");
  good_header(header);
  set_len(header, NFC_RECORD_MAX_PAYLOAD + 1);
  CHECK(nfc_record_parse(header, CAPACITY, ANY, &rec) == NFC_RECORD_ERR_LENGTH,
        "accepted a payload over the ceiling");

  TEST("length past what the tag holds is refused");
  good_header(header);
  set_len(header, 200);
  /* A small NTAG: 144 bytes total, so 200 bytes of payload cannot be there
     even though it is under the ceiling. */
  CHECK(nfc_record_parse(header, 144, ANY, &rec) == NFC_RECORD_ERR_LENGTH,
        "accepted a payload larger than the tag");

  TEST("payload exactly filling the tag is accepted");
  good_header(header);
  set_len(header, 144 - NFC_HEADER_LEN);
  CHECK(nfc_record_parse(header, 144, ANY, &rec) == NFC_RECORD_OK,
        "refused a payload that fits exactly");

  TEST("one byte past a full tag is refused");
  good_header(header);
  set_len(header, 144 - NFC_HEADER_LEN + 1);
  CHECK(nfc_record_parse(header, 144, ANY, &rec) == NFC_RECORD_ERR_LENGTH,
        "off-by-one accepted");

  TEST("capacity smaller than the header is refused");
  good_header(header);
  CHECK(nfc_record_parse(header, 8, ANY, &rec) == NFC_RECORD_ERR_LENGTH,
        "accepted a capacity below the header size");

  TEST("zero capacity is refused");
  good_header(header);
  CHECK(nfc_record_parse(header, 0, ANY, &rec) == NFC_RECORD_ERR_LENGTH,
        "accepted zero capacity");
}

static void test_build_limits(void) {
  uint8_t header[NFC_HEADER_LEN];

  TEST("build refuses an oversize payload");
  CHECK(nfc_record_build(header, NFC_RECORD_MAX_PAYLOAD + 1, CAPACITY,
                         NFC_RECORD_KEF) == NFC_RECORD_ERR_LENGTH,
        "built a record over the ceiling");

  TEST("build refuses a payload the tag cannot hold");
  CHECK(nfc_record_build(header, PAYLOAD_LEN, 20, NFC_RECORD_KEF) ==
            NFC_RECORD_ERR_LENGTH,
        "built a record larger than the tag");

  TEST("build refuses an empty payload");
  CHECK(nfc_record_build(header, 0, CAPACITY, NFC_RECORD_KEF) ==
            NFC_RECORD_ERR_LENGTH,
        "built an empty record");
}

static void test_null_args(void) {
  uint8_t header[NFC_HEADER_LEN];
  nfc_record_t rec;

  TEST("parse rejects NULL arguments");
  CHECK(nfc_record_parse(NULL, CAPACITY, ANY, &rec) == NFC_RECORD_ERR_ARG &&
            nfc_record_parse(header, CAPACITY, ANY, NULL) == NFC_RECORD_ERR_ARG,
        "NULL accepted");

  TEST("build rejects a NULL header");
  CHECK(nfc_record_build(NULL, PAYLOAD_LEN, CAPACITY, NFC_RECORD_KEF) ==
            NFC_RECORD_ERR_ARG,
        "NULL accepted");
}

int main(void) {
  printf("=== NFC record parser tests ===\n\n");

  test_round_trip();
  test_bad_magic();
  test_bad_type();
  test_all_types_round_trip();
  test_type_filtering();
  test_mask_cannot_widen();
  test_build_refuses_unknown_type();
  test_check_ordering();
  test_golden_headers();
  test_format_constants();
  test_reserved_bytes();
  test_bad_length();
  test_build_limits();
  test_null_args();

  printf("\nPassed: %d, Failed: %d\n", tests_passed, tests_failed);
  return tests_failed == 0 ? 0 : 1;
}
