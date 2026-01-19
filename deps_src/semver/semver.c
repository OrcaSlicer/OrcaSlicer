/*
 * semver.c
 *
 * Copyright (c) 2015-2017 Tomas Aparicio
 * MIT licensed
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "semver.h"

#define SLICE_SIZE   50
#define DELIMITER    "."
#define PR_DELIMITER "-"
#define MT_DELIMITER "+"
#define NUMBERS      "0123456789"
#define ALPHA        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
#define DELIMITERS   DELIMITER PR_DELIMITER MT_DELIMITER
#define VALID_CHARS  NUMBERS ALPHA DELIMITERS

static const size_t MAX_SIZE     = sizeof(char) * 255;
static const int MAX_SAFE_INT = (unsigned int) -1 >> 1;

#ifdef _WIN32
  #define strdup _strdup
#endif

/**
 * Define comparison operators, storing the
 * ASCII code per each symbol in hexadecimal notation.
 */

enum operators {
  SYMBOL_GT = 0x3e,
  SYMBOL_LT = 0x3c,
  SYMBOL_EQ = 0x3d,
  SYMBOL_TF = 0x7e,
  SYMBOL_CF = 0x5e
};

/**
 * Private helpers
 */

/*
 * Remove [begin:len-begin] from str by moving len data from begin+len to begin.
 * If len is negative cut out to the end of the string.
 */
static int
strcut (char *str, int begin, int len) {
  size_t l;
  l = strlen(str);

  if((int)l < 0 || (int)l > MAX_SAFE_INT) return -1;

  if (len < 0) len = (int)l - begin + 1;
  if (begin + len > (int)l) len = (int)l - begin;
  memmove(str + begin, str + begin + len, l - len + 1 - begin);

  return len;
}

static int
contains (const char c, const char *matrix, size_t len) {
  size_t x;
  for (x = 0; x < len; x++)
    if ((char) matrix[x] == c) return 1;
  return 0;
}

static int
has_valid_chars (const char *str, const char *matrix) {
  size_t i, len, mlen;
  len = strlen(str);
  mlen = strlen(matrix);

  for (i = 0; i < len; i++)
    if (contains(str[i], matrix, mlen) == 0)
      return 0;

  return 1;
}

static int
binary_comparison (int x, int y) {
  if (x == y) return 0;
  if (x > y) return 1;
  return -1;
}

static int
parse_int (const char *s) {
  int valid, num;
  valid = has_valid_chars(s, NUMBERS);
  if (valid == 0) return -1;

  num = strtol(s, NULL, 10);
  if (num > MAX_SAFE_INT) return -1;

  return num;
}

/*
 * Return a string allocated on the heap with the content from sep to end and
 * terminate buf at sep.
 */
static char *
parse_slice (char *buf, char sep) {
  char *pr, *part;
  size_t plen;

  /* Find separator in buf */
  pr = strchr(buf, sep);
  if (pr == NULL) return NULL;
  /* Length from separator to end of buf */
  plen = strlen(pr);

  /* Copy from buf into new string */
  part = calloc(plen + 1, sizeof(*part));
  if (part == NULL) return NULL;
  memcpy(part, pr + 1, plen);
  /* Null terminate new string */
  part[plen] = '\0';

  /* Terminate buf where separator was */
  *pr = '\0';

  return part;
}

/**
 * Parses a string as semver expression.
 *
 * Returns:
 *
 * `0` - Parsed successfully
 * `-1` - In case of error
 */

int
semver_parse (const char *str, semver_t *ver) {
  int valid, res;
  size_t len;
  char *buf;
  valid = semver_is_valid(str);
  if (!valid) return -1;

  len = strlen(str);
  buf = calloc(len + 1, sizeof(*buf));
  if (buf == NULL) return -1;
  strcpy(buf, str);

  ver->metadata = parse_slice(buf, MT_DELIMITER[0]);
  ver->prerelease = parse_slice(buf, PR_DELIMITER[0]);

  res = semver_parse_version(buf, ver);
  free(buf);
#if DEBUG > 0
  printf("[debug] semver.c %s = %d.%d.%d, %s %s\n", str, ver->major, ver->minor, ver->patch, ver->prerelease, ver->metadata);
#endif
  return res;
}

/**
 * Parses a given string as semver expression.
 *
 * Returns:
 *
 * `0` - Parsed successfully
 * `-1` - Parse error or invalid
 */

int
semver_parse_version (const char *str, semver_t *ver) {
  size_t len;
  int index, value;
  char *slice, *next, *endptr;
  slice = (char *) str;
  index = 0;

  // non mandatory
  ver->patch = 0;

  while (slice != NULL && index++ < 4) {
    next = strchr(slice, DELIMITER[0]);
    if (next == NULL)
      len = strlen(slice);
    else
      len = next - slice;
    if (len > SLICE_SIZE) return -1;

    /* Cast to integer and store */
    value = strtol(slice, &endptr, 10);
    if (endptr != next && *endptr != '\0') return -1;

    switch (index) {
      case 1: ver->major = value; break;
      case 2: ver->minor = value; break;
      case 3: ver->patch = value; break;
      //BBS: add convert for AA.BB.CC.DD
      case 4: ver->patch = (ver->patch*100)+value; break;
    }

    /* Continue with the next slice */
    if (next == NULL)
      slice = NULL;
    else
      slice = next + 1;
  }

  // Major and minor versions are mandatory, patch version is not mandatory.
  return (index == 2 || index == 3 || index == 4) ? 0 : -1;
}

static int
compare_prerelease (char *x, char *y) {
  char *lastx, *lasty, *xptr, *yptr, *endptr;
  size_t xlen, ylen, xn, yn, min;
  int xisnum, yisnum, xnum, ynum;
  int res;
  if (x == NULL && y == NULL) return 0;
  if (y == NULL && x) return -1;
  if (x == NULL && y) return 1;

  // Orca: Special handling for 'dev'
  if (strcmp(x, "dev") == 0 && strcmp(y, "dev") == 0)
      return 0;
  if (strcmp(x, "dev") == 0 && strcmp(y, "dev") != 0)
      return -1;
  if (strcmp(x, "dev") != 0 && strcmp(y, "dev") == 0)
      return 1;

  lastx = x;
  lasty = y;
  xlen = strlen(x);
  ylen = strlen(y);

  while (1) {
    if ((xptr = strchr(lastx, DELIMITER[0])) == NULL)
      xptr = x + xlen;
    if ((yptr = strchr(lasty, DELIMITER[0])) == NULL)
      yptr = y + ylen;

    xnum = strtol(lastx, &endptr, 10);
    xisnum = endptr == xptr ? 1 : 0;
    ynum = strtol(lasty, &endptr, 10);
    yisnum = endptr == yptr ? 1 : 0;

    if (xisnum && !yisnum) return -1;
    if (!xisnum && yisnum) return 1;

    if (xisnum && yisnum) {
      /* Numerical comparison */
      if (xnum != ynum) return xnum < ynum ? -1 : 1;
    } else {
      /* String comparison */
      xn = xptr - lastx;
      yn = yptr - lasty;
      min = xn < yn ? xn : yn;
      if ((res = strncmp(lastx, lasty, min))) return res < 0 ? -1 : 1;
      if (xn != yn) return xn < yn ? -1 : 1;
    }

    lastx = xptr + 1;
    lasty = yptr + 1;
    if (lastx == x + xlen + 1 && lasty == y + ylen + 1) break;
    if (lastx == x + xlen + 1) return -1;
    if (lasty == y + ylen + 1) return 1;
  }

  return 0;
}

int
semver_compare_prerelease (semver_t x, semver_t y) {
  return compare_prerelease(x.prerelease, y.prerelease);
}

/**
 * Performs a major, minor and patch binary comparison (x, y).
 * This function is mostly used internally
 *
 * Returns:
 *
 * `0` - If versiona are equal
 * `1` - If x is higher than y
 * `-1` - If x is lower than y
 */

int
semver_compare_version (semver_t x, semver_t y) {
  int res;

  if ((res = binary_comparison(x.major, y.major)) == 0) {
    if ((res = binary_comparison(x.minor, y.minor)) == 0) {
      return binary_comparison(x.patch, y.patch);
    }
  }

  return res;
}

/**
 * Compare two semantic versions (x, y).
 *
 * Returns:
 * - `1` if x is higher than y
 * - `0` if x is equal to y
 * - `-1` if x is lower than y
 */

int
semver_compare (semver_t x, semver_t y) {
  int res;

  if ((res = semver_compare_version(x, y)) == 0) {
    return semver_compare_prerelease(x, y);
  }

  return res;
}

/**
 * Performs a `greater than` comparison
 */

int
semver_gt (semver_t x, semver_t y) {
  return semver_compare(x, y) == 1;
}

/**
 * Performs a `lower than` comparison
 */

int
semver_lt (semver_t x, semver_t y) {
  return semver_compare(x, y) == -1;
}

/**
 * Performs a `equality` comparison
 */

int
semver_eq (semver_t x, semver_t y) {
  return semver_compare(x, y) == 0;
}

/**
 * Performs a `non equal to` comparison
 */

int
semver_neq (semver_t x, semver_t y) {
  return semver_compare(x, y) != 0;
}

/**
 * Performs a `greater than or equal` comparison
 */

int
semver_gte (semver_t x, semver_t y) {
  return semver_compare(x, y) >= 0;
}

/**
 * Performs a `lower than or equal` comparison
 */

int
semver_lte (semver_t x, semver_t y) {
  return semver_compare(x, y) <= 0;
}

/**
 * Checks if version `x` can be satisfied by `y`
 * performing a comparison with caret operator.
 *
 * See: https://docs.npmjs.com/misc/semver#caret-ranges-1-2-3-0-2-5-0-0-4
 *
 * Returns:
 *
 * `1` - Can be satisfied
 * `0` - Cannot be satisfied
 */

int
semver_satisfies_caret (semver_t x, semver_t y) {
  if (x.major == y.major) {
    if (x.major == 0) {
      return x.minor >= y.minor;
    }
    return 1;
  }
  return 0;
}

/**
 * Checks if version `x` can be satisfied by `y`
 * performing a comparison with tilde operator.
 *
 * See: https://docs.npmjs.com/misc/semver#tilde-ranges-1-2-3-1-2-1
 *
 * Returns:
 *
 * `1` - Can be satisfied
 * `0` - Cannot be satisfied
 */

int
semver_satisfies_patch (semver_t x, semver_t y) {
  return x.major == y.major
      && x.minor == y.minor;
}

/**
 * Checks if both versions can be satisfied
 * based on the given comparison operator.
 *
 * Allowed operators:
 *
 * - `=`  - Equality
 * - `>=` - Higher or equal to
 * - `<=` - Lower or equal to
 * - `<`  - Lower than
 * - `>`  - Higher than
 * - `^`  - Caret comparison (see https://docs.npmjs.com/misc/semver#caret-ranges-1-2-3-0-2-5-0-0-4)
 * - `~`  - Tilde comparison (see https://docs.npmjs.com/misc/semver#tilde-ranges-1-2-3-1-2-1)
 *
 * Returns:
 *
 * `1` - Can be satisfied
 * `0` - Cannot be satisfied
 */

int
semver_satisfies (semver_t x, semver_t y, const char *op) {
  int first, second;
  /* Extract the comparison operator */
  first = op[0];
  second = op[1];

  /* Caret operator */
  if (first == SYMBOL_CF)
    return semver_satisfies_caret(x, y);

  /* Tilde operator */
  if (first == SYMBOL_TF)
    return semver_satisfies_patch(x, y);

  /* Strict equality */
  if (first == SYMBOL_EQ)
    return semver_eq(x, y);

  /* Greater than or equal comparison */
  if (first == SYMBOL_GT) {
    if (second == SYMBOL_EQ) {
      return semver_gte(x, y);
    }
    return semver_gt(x, y);
  }

  /* Lower than or equal comparison */
  if (first == SYMBOL_LT) {
    if (second == SYMBOL_EQ) {
      return semver_lte(x, y);
    }
    return semver_lt(x, y);
  }

  return 0;
}

/**
 * Free heep allocated memory of a given semver.
 * This is just a convenient function that you
 * should call when you're done.
 */

void
semver_free (semver_t *x) {
  if (x->metadata) {
    free(x->metadata);
    x->metadata = NULL;
  }
  if (x->prerelease) {
    free(x->prerelease);
    x->prerelease = NULL;
  }
}

/**
 * Renders
 */

static void
concat_num (char * str, int x, char * sep) {
  char buf[SLICE_SIZE] = {0};
  if (sep == NULL) sprintf(buf, "%d", x);
  else sprintf(buf, "%s%d", sep, x);
  strcat(str, buf);
}

static void
concat_char (char * str, char * x, char * sep) {
  char buf[SLICE_SIZE] = {0};
  sprintf(buf, "%s%s", sep, x);
  strcat(str, buf);
}

/**
 * Render a given semver as string
 */

void
semver_render (semver_t *x, char *dest) {
  if (x->major) concat_num(dest, x->major, NULL);
  if (x->minor) concat_num(dest, x->minor, DELIMITER);
  if (x->patch) concat_num(dest, x->patch, DELIMITER);
  if (x->prerelease) concat_char(dest, x->prerelease, PR_DELIMITER);
  if (x->metadata) concat_char(dest, x->metadata, MT_DELIMITER);
}

/**
 * Version bump helpers
 */

void
semver_bump (semver_t *x) {
  x->major++;
}

void
semver_bump_minor (semver_t *x) {
  x->minor++;
}

void
semver_bump_patch (semver_t *x) {
  x->patch++;
}

/**
 * Helpers
 */

static int
has_valid_length (const char *s) {
  return strlen(s) <= MAX_SIZE;
}

/**
 * Checks if a given semver string is valid
 *
 * Returns:
 *
 * `1` - Valid expression
 * `0` - Invalid
 */

int
semver_is_valid (const char *s) {
  return has_valid_length(s)
      && has_valid_chars(s, VALID_CHARS);
}

/**
 * Removes non-valid characters in the given string.
 *
 * Returns:
 *
 * `0`  - Valid
 * `-1` - Invalid input
 */

int
semver_clean (char *s) {
  size_t i, len, mlen;
  int res;
  if (has_valid_length(s) == 0) return -1;

  len = strlen(s);
  mlen = strlen(VALID_CHARS);

  for (i = 0; i < len; i++) {
    if (contains(s[i], VALID_CHARS, mlen) == 0) {
      res = strcut(s, (int)i, 1);
      if(res == -1) return -1;
      --len; --i;
    }
  }

  return 0;
}

static int
char_to_int (const char * str) {
  int buf;
  size_t i,len, mlen;
  buf = 0;
  len = strlen(str);
  mlen = strlen(VALID_CHARS);

  for (i = 0; i < len; i++)
    if (contains(str[i], VALID_CHARS, mlen))
      buf += (int) str[i];

  return buf;
}

/**
 * Render a given semver as numeric value.
 * Useful for ordering and filtering.
 */

int
semver_numeric (semver_t *x) {
  int num;
  char buf[SLICE_SIZE * 3];
  memset(&buf, 0, SLICE_SIZE * 3);

  if (x->major) concat_num(buf, x->major, NULL);
  if (x->minor) concat_num(buf, x->minor, NULL);
  if (x->patch) concat_num(buf, x->patch, NULL);

  num = parse_int(buf);
  if(num == -1) return -1;

  if (x->prerelease) num += char_to_int(x->prerelease);
  if (x->metadata) num += char_to_int(x->metadata);

  return num;
}

char *semver_strdup(const char *src) {
  if (src == NULL) return NULL;
  size_t len = strlen(src) + 1;
  char *res = malloc(len);
  return res != NULL ? (char *) memcpy(res, src, len) : NULL;
}

semver_t
semver_copy(const semver_t *ver) {
  semver_t res = *ver;
  if (ver->metadata != NULL) {
    res.metadata = strdup(ver->metadata);
  }
  if (ver->prerelease != NULL) {
    res.prerelease = strdup(ver->prerelease);
  }
  return res;
}
