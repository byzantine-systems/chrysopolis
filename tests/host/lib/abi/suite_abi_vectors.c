/* Shared golden vectors. tools/abi builds each image from the typed model and
 * states the verdict; the generated C checkers must reach it at an even and an
 * odd address. apps/chryso_abi runs the same manifest through the Erlang
 * codec, so both languages are held to one set of bytes.
 *
 * argv[1] is the vector directory: manifest.txt plus <name>.bin per vector.
 * The manifest is read by a small state machine (header, then rows), and the
 * row count must equal the header's, so a vector cannot be skipped silently. */
#include "abi_shim.h"
#include "check.h"

#include <chrysopolis/orchestrator_abi.h>
#include <chrysopolis/root_control.h>

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Room for the largest record, one extra byte for the oversize vectors, and
 * one leading byte so the same image can be checked at an odd address. */
static constexpr size_t vector_capacity = sizeof(chryso_root_status_page) + 1;
static uint8_t vector_storage[vector_capacity + 1];

static constexpr size_t line_capacity = 256;
static constexpr size_t path_capacity = 4096;
static constexpr uint64_t manifest_version = 1;

typedef struct {
  const char *data;
  size_t len;
} text_view;

typedef enum {
  vectors_ok,
  vectors_malformed,
  vectors_io_error,
} vectors_status;

typedef enum {
  manifest_expect_header,
  manifest_expect_rows,
} manifest_phase;

typedef struct {
  manifest_phase phase;
  uint64_t declared;
  uint64_t seen;
  const char *dir;
} manifest;

static bool view_is(text_view view, const char *expected) {
  size_t len = strlen(expected);
  return view.len == len && memcmp(view.data, expected, len) == 0;
}

/* The next space-separated field of `line` from `*at`. Fields are separated
 * by exactly one space; an empty field is returned as such, not skipped. */
static text_view next_field(text_view line, size_t *at) {
  size_t start = *at;
  size_t end = start;
  while (end < line.len && line.data[end] != ' ') {
    ++end;
  }
  *at = end < line.len ? end + 1 : end;
  return (text_view){.data = line.data + start, .len = end - start};
}

/* Strict unsigned parse of a whole field: no sign, no junk, no overflow. */
static bool parse_u64(text_view field, int base, uint64_t *out) {
  char digits[24] = {};
  if (field.len == 0 || field.len >= sizeof digits || field.data[0] == '-' ||
      field.data[0] == '+') {
    return false;
  }
  memcpy(digits, field.data, field.len);
  errno = 0;
  char *end = nullptr;
  unsigned long long value = strtoull(digits, &end, base);
  if (end != digits + field.len || errno == ERANGE || value > UINT64_MAX) {
    return false;
  }
  *out = (uint64_t)value; /* range checked above */
  return true;
}

/* Vector names become file names, so only [a-z0-9_] is accepted. */
static bool valid_name(text_view name) {
  static const char allowed[] = "abcdefghijklmnopqrstuvwxyz0123456789_";
  if (name.len == 0) {
    return false;
  }
  for (size_t i = 0; i < name.len; ++i) {
    if (name.data[i] == '\0' || !strchr(allowed, name.data[i])) {
      return false;
    }
  }
  return true;
}

static const struct {
  const char *name;
  chryso_abi_reject verdict;
} reject_names[] = {
    {"ok", chryso_abi_reject_ok},
    {"magic", chryso_abi_reject_magic},
    {"version", chryso_abi_reject_version},
    {"size", chryso_abi_reject_size},
    {"checksum", chryso_abi_reject_checksum},
    {"reserved", chryso_abi_reject_reserved},
    {"unknown_kind", chryso_abi_reject_unknown_kind},
    {"range", chryso_abi_reject_range},
    {"torn", chryso_abi_reject_torn},
};

static bool parse_reject(text_view field, chryso_abi_reject *out) {
  for (size_t i = 0; i < sizeof reject_names / sizeof reject_names[0]; ++i) {
    if (view_is(field, reject_names[i].name)) {
      *out = reject_names[i].verdict;
      return true;
    }
  }
  return false;
}

/* Reads <dir>/<name>.bin into vector_storage + 1. */
static vectors_status read_vector(const char *dir, text_view name,
                                  size_t *len) {
  char path[path_capacity] = {};
  if (name.len > INT_MAX) {
    return vectors_malformed;
  }
  int needed =
      snprintf(path, sizeof path, "%s/%.*s.bin", dir, (int)name.len, name.data);
  if (needed < 0 || (size_t)needed >= sizeof path) {
    return vectors_malformed;
  }
  FILE *file = fopen(path, "rb");
  if (!file) {
    fprintf(stderr, "%s: %s\n", path, strerror(errno));
    return vectors_io_error;
  }
  vectors_status status = vectors_ok;
  uint8_t *bytes = vector_storage + 1;
  size_t got = fread(bytes, 1, vector_capacity, file);
  if (ferror(file)) {
    status = vectors_io_error;
  } else if (got == vector_capacity && fgetc(file) != EOF) {
    fprintf(stderr, "%s: larger than any record\n", path);
    status = vectors_malformed;
  }
  if (fclose(file) == EOF && status == vectors_ok) {
    status = vectors_io_error;
  }
  *len = got;
  return status;
}

/* One row: `name checker bank class canonical size`. */
static vectors_status check_row(const manifest *state, text_view line) {
  size_t at = 0;
  text_view name = next_field(line, &at);
  text_view checker = next_field(line, &at);
  text_view bank_field = next_field(line, &at);
  text_view class_field = next_field(line, &at);
  text_view canonical = next_field(line, &at);
  text_view size_field = next_field(line, &at);
  uint64_t bank = 0;
  uint64_t size = 0;
  chryso_abi_reject expected = chryso_abi_reject_ok;
  if (at != line.len || !valid_name(name) || checker.len == 0 ||
      !parse_u64(bank_field, 10, &bank) ||
      !parse_reject(class_field, &expected) ||
      !(view_is(canonical, "0") || view_is(canonical, "1")) ||
      !parse_u64(size_field, 10, &size) || bank > SIZE_MAX) {
    return vectors_malformed;
  }

  size_t len = 0;
  vectors_status status = read_vector(state->dir, name, &len);
  if (status != vectors_ok) {
    return status;
  }
  CHECK_EQ_U64(len, size);

  /* The file was read one byte in: an odd address first, then even. */
  uint8_t odd = abi_shim_check(checker.data, checker.len, vector_storage + 1,
                               len, (size_t)bank);
  memmove(vector_storage, vector_storage + 1, len);
  uint8_t even = abi_shim_check(checker.data, checker.len, vector_storage, len,
                                (size_t)bank);
  if (even == UINT8_MAX) {
    fprintf(stderr, "%.*s: unknown checker %.*s\n", (int)name.len, name.data,
            (int)checker.len, checker.data);
  }
  if (even != expected || odd != expected) {
    fprintf(stderr, "%.*s: expected %u, got %u even and %u odd\n",
            (int)name.len, name.data, (unsigned)expected, (unsigned)even,
            (unsigned)odd);
  }
  CHECK_EQ_U64(even, expected);
  CHECK_EQ_U64(odd, expected);
  return vectors_ok;
}

/* `chryso-abi-vectors <version> <count> <layout digest>`. */
static vectors_status check_header(manifest *state, text_view line) {
  size_t at = 0;
  text_view magic = next_field(line, &at);
  text_view version = next_field(line, &at);
  text_view count = next_field(line, &at);
  text_view digest_field = next_field(line, &at);
  uint64_t format = 0;
  uint64_t digest = 0;
  if (at != line.len || !view_is(magic, "chryso-abi-vectors") ||
      !parse_u64(version, 10, &format) ||
      !parse_u64(count, 10, &state->declared) || digest_field.len != 8 ||
      !parse_u64(digest_field, 16, &digest)) {
    return vectors_malformed;
  }
  CHECK_EQ_U64(format, manifest_version);
  /* Vectors built from another model must not pass against these headers. */
  CHECK_EQ_U64(digest, chryso_abi_layout_digest);
  CHECK(state->declared > 0);
  return vectors_ok;
}

static vectors_status manifest_step(manifest *state, text_view line) {
  switch (state->phase) {
  case manifest_expect_header: {
    vectors_status status = check_header(state, line);
    if (status == vectors_ok) {
      state->phase = manifest_expect_rows;
    }
    return status;
  }
  case manifest_expect_rows:
    if (state->seen == state->declared) {
      return vectors_malformed; /* more rows than the header declared */
    }
    ++state->seen;
    return check_row(state, line);
  }
  return vectors_malformed;
}

static vectors_status run_manifest(manifest *state) {
  char path[path_capacity] = {};
  int needed = snprintf(path, sizeof path, "%s/manifest.txt", state->dir);
  if (needed < 0 || (size_t)needed >= sizeof path) {
    return vectors_malformed;
  }
  FILE *file = fopen(path, "rb");
  if (!file) {
    fprintf(stderr, "%s: %s\n", path, strerror(errno));
    return vectors_io_error;
  }
  vectors_status status = vectors_ok;
  char line[line_capacity] = {};
  while (status == vectors_ok && fgets(line, (int)sizeof line, file)) {
    size_t len = strlen(line);
    if (len == 0 || line[len - 1] != '\n') {
      status = vectors_malformed; /* overlong or unterminated line */
      break;
    }
    status = manifest_step(state, (text_view){.data = line, .len = len - 1});
  }
  if (status == vectors_ok && ferror(file)) {
    status = vectors_io_error;
  }
  if (fclose(file) == EOF && status == vectors_ok) {
    status = vectors_io_error;
  }
  return status;
}

int main(int argc, char *argv[]) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <vector-dir>\n", argc > 0 ? argv[0] : "suite");
    return EXIT_FAILURE;
  }
  CHECK_EQ_U64(chryso_abi_crc32((const uint8_t *)"123456789", 9),
               UINT32_C(0xCBF43926));

  manifest state = {.phase = manifest_expect_header, .dir = argv[1]};
  vectors_status status = run_manifest(&state);
  CHECK(status == vectors_ok);
  CHECK(state.phase == manifest_expect_rows);
  CHECK_EQ_U64(state.seen, state.declared);
  printf("abi_vectors: %" PRIu64 " vectors\n", state.seen);
  return check_finish("abi_vectors");
}
