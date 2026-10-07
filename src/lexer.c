#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "lexer.h"
#include "charclass.h"

/*
  Lexer shared by the preprocessor and both assembler passes.

  All scanning goes through one cursor, `current`, into a NUL-terminated
  source buffer. Every consume_* function either consumes a complete token
  (advancing `current`) or leaves `current` where it was, so callers can try
  alternatives in sequence. skip() never crosses a newline; statement
  boundaries are crossed explicitly with skip_newline()/skip_line(), which
  keep `line_count` in step for diagnostics.
*/

char const * current;
char const * current_buffer_start = NULL;
unsigned line_count = 1;
char const * current_file;

// Print "<kind> in <file>\nline N: "<source line>"" for the line containing
// `current`. The line is trimmed of surrounding whitespace; a blank line prints
// as "". The walk back stops at current_buffer_start or the NUL sentinel that
// precedes every preprocessed buffer.
static void print_source_context(const char* kind) {
  fprintf(stderr, "%s in %s\nline %u: \"", kind, current_file, line_count);

  char const * buffer_start = current_buffer_start != NULL ? current_buffer_start : current;
  char const * start = current;
  while (start > buffer_start && *(start - 1) != '\0' && *(start - 1) != '\n') start--;
  char const * end = current;
  while (*end != '\0' && *end != '\n') end++;

  // Trim inside [start, end) only, so a whitespace-only line cannot make the
  // span run into the neighbouring lines.
  while (start < end && ascii_isspace((unsigned char)*start)) start++;
  while (end > start && ascii_isspace((unsigned char)*(end - 1))) end--;

  struct Slice line = {start, (size_t)(end - start)};
  print_slice_err(&line);
  fprintf(stderr, "\"\n");
}


// print line causing an error
// Only the first error prints its source context; later diagnostics from the
// same failure (e.g. "Preprocesser macro error") are printed bare.
void print_error(void) {
  static bool has_printed = false;
  if (has_printed) return;
  print_source_context("Error");
  has_printed = true;
}


// Print a warning at the current source location.
void print_warning(const char* message) {
  print_source_context("Warning");
  fprintf(stderr, "%s\n", message);
}


// is the rest of the file just whitespace?
bool is_at_end(void) {
  while (ascii_isspace((unsigned char)*current)) {
    if (*current == '\n') line_count++;
    current += 1;
  }
  if (*current != 0) return false;
  else return true;
}


// skip whitespace and commas until end of line or non-whitespace character
void skip(void) {
  while ((ascii_isspace((unsigned char)*current) && *current != '\n') || *current == ',' || *current == ';') {
    current++;
  }
}


// skip until we get to a new nonempty line
void skip_newline(void) {
  while (ascii_isspace((unsigned char)*current)) {
    if (*current == '\n') line_count++;
    current++;
  }
}


// skip an entire line
void skip_line(void){
  while (*current != '\n' && *current != '\0') current++; 
  skip_newline();
}


// attempt to consume a string, has no effect if a match is not found
bool consume(const char* str) {
  skip();
  size_t i = 0;
  while (true) {
    char const expected = str[i];
    char const found = current[i];
    if (expected == 0) {
      /* survived to the end of the expected string */
      current += i;
      return true;
    }
    if (expected != found) {
      return false;
    }
    i += 1;
  } 
}


// attempt to consume a keyword, has no effect if a match is not found
// differs from consume because we ensure token boundaries on both sides
bool consume_keyword(const char* str) {
  // Whitespace is intentionally left to the caller because preprocessing also
  // uses this matcher.
  if (current != current_buffer_start &&
      is_identifier_char(current[-1])) {
    return false;
  }

  size_t i = 0;
  while (true) {
    char const expected = str[i];
    char const found = current[i];
    if (expected == 0) {
      /* survived to the end of the expected string */
      if (ascii_isspace(found) || found == '\0' ||
          found == ',' || found == ';' || found == ':') {
        // word break
        current += i;
        return true;
      } else {
        return false;
      }
    }
    if (expected != found) {
      return false;
    }
    i += 1;
  } 
}


// attempt to consume an identifier, has no effect if a match is not found
// On success *out views the identifier in the source buffer.
bool consume_identifier(struct Slice* out) {
  skip();
  if (!is_identifier_start(current[0])) return false;
  // identifiers begin with a letter or underscore, then letters, digits,
  // underscores, and periods
  size_t i = 1;
  while (is_identifier_char(current[i])) i++;
  out->start = current;
  out->len = i;
  current += i;
  return true;
}


// Return whether c ends an unquoted .line filename.
static bool ends_filename(char c) {
  return c == '\0' || ascii_isspace((unsigned char)c) || c == ',' || c == ';';
}


// attempt to consume a filename, has no effect if a match is not found
// Debug file names may be relative or absolute and are emitted without
// quotes. Consume until the next whitespace or statement separator.
bool consume_filename(struct Slice* out) {
  skip();
  if (ends_filename(current[0])) return false;
  size_t i = 1;
  while (!ends_filename(current[i])) i++;
  out->start = current;
  out->len = i;
  current += i;
  return true;
}


// Consume an identifier followed by ':' into *out. On failure nothing is
// consumed beyond leading separators.
bool consume_label(struct Slice* out){
  skip();
  char const * old_current = current;
  if (consume_identifier(out) && consume(":")) return true;

  // undo side effects
  current = old_current;
  return false;
}


// Consume name when it denotes a complete register token.
static bool consume_named_register(const char* name) {
  size_t len = strlen(name);
  if (strncmp(current, name, len) == 0 && !is_identifier_char(current[len])) {
    current += len;
    return true;
  }
  return false;
}


// attempt to consume a register
int consume_register(void) {
  skip();

  if (consume_named_register("sp")) return 31;
  else if (consume_named_register("bp")) return 30;
  else if (consume_named_register("ra")) return 29;

  // registers begin with an r
  else if (current[0] == 'r' && ascii_isdigit((unsigned char)current[1])) {
    int v = 0;
    size_t i = 1;
    while(ascii_isdigit((unsigned char)current[i])) {
      // then followed by numbers
      v = 10 * v + current[i] - '0';
      i += 1;
    }

    if (v > 31 || is_identifier_char(current[i])) return -1;
    current += i;
    return v;
  }
  else return -1;
}


// attempt to consume a control register
int consume_control_register(void) {
  skip();
  // registers begin with an r
  if (current[0] == 'c' && current[1] == 'r' && ascii_isdigit((unsigned char)current[2])) {
    int v = 0;
    size_t i = 2;
    while(ascii_isdigit((unsigned char)current[i])) {
      // then followed by numbers
      v = 10 * v + current[i] - '0';
      i += 1;
    }

    // cr10 and cr11 are reserved (formerly the IPI mailboxes; see
    // docs/ISA.md "Control Registers"), so only cr0 - cr9 and cr12 exist.
    if (v > 12 || v == 10 || v == 11 || is_identifier_char(current[i])) return -1;
    current += i;
    return v;
  } else {
    if (consume_named_register("psr")) return 0;
    else if (consume_named_register("pid")) return 1;
    else if (consume_named_register("isr")) return 2;
    else if (consume_named_register("imr")) return 3;
    else if (consume_named_register("epc")) return 4;
    else if (consume_named_register("flg")) return 5;
    else if (consume_named_register("efg")) return 6;
    else if (consume_named_register("tlba")) return 7;
    else if (consume_named_register("ksp")) return 8;
    else if (consume_named_register("cid")) return 9;
    else if (consume_named_register("tlbf")) return 12;
    else return -1;
  }
}


// Return the value of c as a hexadecimal digit, or -1.
static int digit_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}


// Radix prefixes accepted after a leading 0 (either letter case).
static const struct {
  char prefix;
  int radix;
  const char* name;       // used mid-sentence: "Invalid hex literal"
  const char* name_cap;   // used to start a sentence: "Hex literal requires..."
} kRadixPrefixes[] = {
  {'b', 2, "binary", "Binary"},
  {'o', 8, "octal", "Octal"},
  {'x', 16, "hex", "Hex"},
};


// attempt to consume an integer literal
// Accepts an optional '-', then decimal (no leading zero), a lone 0, or a
// 0b/0o/0x prefixed literal. Binary and octal literals run over decimal
// digits and hex over alphanumerics, so an out-of-range digit is an ERROR
// rather than the silent end of the literal.
long consume_literal(enum ConsumeResult* result) {
  skip();
  bool negate = false;
  char const * old_current = current;
  if (*current == '-') {
    negate = true;
    current++;
    skip();
  }

  // edge case for zero literal
  // (only time leading 0 is allowed)
  char next = current[1];
  if (*current == '0' &&
      (ascii_isspace((unsigned char)next) || next == '\0' || next == ']' || next == '#')){
    *result = FOUND;
    current++;
    return 0;
  }

  long v = 0;
  if (ascii_isdigit((unsigned char)*current) && *current != '0') {
    // decimal literal
    do {
      v = 10*v + ((*current) - '0');
      current += 1;
    } while (ascii_isdigit((unsigned char)*current));
    *result = FOUND;
    return negate ? -v : v;
  }

  if (*current == '0') {
    for (size_t p = 0; p < sizeof(kRadixPrefixes) / sizeof(kRadixPrefixes[0]); ++p) {
      int radix = kRadixPrefixes[p].radix;
      if ((next | 0x20) != kRadixPrefixes[p].prefix) continue;  // ASCII lower-case

      current += 2;
      bool saw_digit = false;
      while (radix == 16 ? ascii_isalnum((unsigned char)*current)
                         : ascii_isdigit((unsigned char)*current)) {
        int d = digit_value(*current);
        if (d < 0 || d >= radix) {
          print_error();
          fprintf(stderr, "Invalid %s literal\n", kRadixPrefixes[p].name);
          *result = ERROR;
          return 0;
        }
        saw_digit = true;
        v = radix * v + d;
        current += 1;
      }
      if (!saw_digit) {
        print_error();
        fprintf(stderr, "%s literal requires at least one digit\n", kRadixPrefixes[p].name_cap);
        *result = ERROR;
        return 0;
      }
      *result = FOUND;
      return negate ? -v : v;
    }
  }

  current = old_current;
  *result = NOT_FOUND;
  return 0;
}

