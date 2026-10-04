#ifndef CHARCLASS_H
#define CHARCLASS_H

#include <stdbool.h>

// ASCII character classes used by the lexer and keyword matcher.
// Assembler source is ASCII, so these avoid ctype.h's locale lookup on the
// lexing hot path and behave the same on every host.

// Return whether c is ASCII whitespace (space, \t, \n, \v, \f, \r).
static inline bool ascii_isspace(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

// Return whether c is an ASCII decimal digit.
static inline bool ascii_isdigit(unsigned char c) {
  return c >= '0' && c <= '9';
}

// Return whether c is an ASCII letter.
static inline bool ascii_isalpha(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// Return whether c is an ASCII letter or digit.
static inline bool ascii_isalnum(unsigned char c) {
  return ascii_isalpha(c) || ascii_isdigit(c);
}

// Return whether c can begin an identifier (letter or underscore).
static inline bool is_identifier_start(char c) {
  return ascii_isalpha((unsigned char)c) || c == '_';
}

// Return whether c can continue an identifier (letter, digit, '_' or '.').
// Directives are matched with the same rule, so ".text" is one token.
static inline bool is_identifier_char(char c) {
  return ascii_isalnum((unsigned char)c) || c == '_' || c == '.';
}

#endif  // CHARCLASS_H
