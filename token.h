#pragma once

enum TokenType {
  // tokens with data
  INT_LIT, // CONTAINS INT
  LONG_LIT, // CONTAINS LONG
  U_INT_LIT, // CONTAINS UNSIGNED
  U_LONG_LIT, // CONTINS UNSIGNED LONG
  IDENT, // CONTAINS SLICE

  // TOKENS WITHOUT DATA
  VOID_TOK,
  INT_TOK,
  RETURN_TOK,
  OPEN_P,
  CLOSE_P,
  OPEN_B,
  CLOSE_B,
  SEMI,
  TILDE,
  INC_TOK,
  DEC_TOK,
  PLUS,
  ASTERISK,
  SLASH,
  PERCENT,
  MINUS,
  AMPERSAND,
  PIPE,
  CARAT,
  SHIFT_L_TOK,
  SHIFT_R_TOK,
  EXCLAMATION,
  DOUBLE_AMPERSAND,
  DOUBLE_PIPE,
  DOUBLE_EQUALS,
  EQUALS,
  NOT_EQUAL,
  LESS_THAN,
  GREATER_THAN,
  LESS_THAN_EQ,
  GREATER_THAN_EQ,
  PLUS_EQ,
  MINUS_EQ,
  TIMES_EQ,
  DIV_EQ,
  MOD_EQ,
  AND_EQ,
  OR_EQ,
  XOR_EQ,
  SHL_EQ,
  SHR_EQ,
  IF_TOK,
  ELSE_TOK,
  QUESTION,
  COLON,
  GOTO_TOK,
  DO_TOK,
  WHILE_TOK,
  FOR_TOK,
  BREAK_TOK,
  CONTINUE_TOK,
  COMMA,
  STATIC_TOK,
  EXTERN_TOK,
  SWITCH_TOK,
  CASE_TOK,
  DEFAULT_TOK,
  UNSIGNED_TOK,
  SIGNED_TOK,
  LONG_TOK,
};

union TokenVariant {
  int int_val;
  unsigned uint_val;
  long long_val;
  unsigned long ulong_val;
  struct Slice* ident_name;
};

struct Token {
  enum TokenType type;
  union TokenVariant data;
};

void free_token(struct Token* token);

void print_token(struct Token token);