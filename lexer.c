#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <ctype.h>

#include "slice.h"
#include "token.h"
#include "token_array.h"
#include "lexer.h"

static char const * program;
static char const * current;

static void print_error() {
  printf("lexer failed at offset %ld\n", (size_t)(current-program));
  printf("%s\n",current);
}

static bool is_at_end() {
  while (isspace(*current)) {
    current += 1;
  }
  if (*current != 0) return false;
  else return true;
}

static void skip() {
  while (isspace(*current)) {
    current += 1;
  }
}

static bool consume(const char* str) {
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
    // assertion: found != 0
    i += 1;
  } 
}

static bool consume_keyword(const char* str) {
  skip();
  size_t i = 0;
  while (true) {
    char const expected = str[i];
    char const found = current[i];
    if (expected == 0) {
      /* survived to the end of the expected string */
      if (!isalnum(found) && found != '_') {
        // word break
        current += i;
        return true;
      } else {
        // this is actually an identifier
        return false;
      }
    }
    if (expected != found) {
      return false;
    }
    // assertion: found != 0
    i += 1;
  } 
}

static bool consume_identifier(struct Token* token) {
  skip();
  if (isalpha(*current)) {
    char const * start = current;
    do {
      current += 1;
    } while(isalnum(*current) || *current == '_');

    struct Slice* slice = malloc(sizeof(struct Slice));
    slice->start = start;
    slice->len = (current - start);

    token->type = IDENT;
    token->data.ident_name = slice;
    return true;
  } else {
    return false;
  }
}

static bool consume_literal(struct Token* token) {
  skip();
  if (isdigit(*current)) {
    uint64_t v = 0;
    do {
      v = 10*v + ((*current) - '0');
      current += 1;
    } while (isdigit(*current));

    token->type = INT_LIT;
    token->data.int_val = v;
    return true;
  } else {
    return NULL;
  }
}

// consumes any token
static struct Token* consume_any(){
  struct Token* token = malloc(sizeof(struct Token));

  if (consume_keyword("return")) { token->type = RETURN_TOK; return token; }
  if (consume_keyword("void")) { token->type = VOID_TOK; return token; }
  if (consume_keyword("if")) { token->type = IF_TOK; return token; }
  if (consume_keyword("else")) { token->type = ELSE_TOK; return token; }
  if (consume_keyword("do")) { token->type = DO_TOK; return token; }
  if (consume_keyword("while")) { token->type = WHILE_TOK; return token; }
  if (consume_keyword("for")) { token->type = FOR_TOK; return token; }
  if (consume_keyword("goto")) { token->type = GOTO_TOK; return token; }
  if (consume_keyword("break")) { token->type = BREAK_TOK; return token; }
  if (consume_keyword("continue")) { token->type = CONTINUE_TOK; return token; }
  if (consume_keyword("static")) { token->type = STATIC_TOK; return token; }
  if (consume_keyword("extern")) { token->type = EXTERN_TOK; return token; }
  if (consume_keyword("switch")) { token->type = SWITCH_TOK; return token; }
  if (consume_keyword("case")) { token->type = CASE_TOK; return token; }
  if (consume_keyword("default")) { token->type = DEFAULT_TOK; return token; }
  if (consume_keyword("int")) { token->type = INT_TOK; return token; }
  if (consume_keyword("unsigned")) { token->type = UNSIGNED_TOK; return token; }
  if (consume_keyword("signed")) { token->type = SIGNED_TOK; return token; }
  if (consume_keyword("long")) { token->type = LONG_TOK; return token; }

  if (consume(",")) { token->type = COMMA; return token; }
  if (consume("?")) { token->type = QUESTION; return token; }
  if (consume(":")) { token->type = COLON; return token; }
  if (consume(";")) { token->type = SEMI; return token; }
  if (consume("(")) { token->type = OPEN_P; return token; }
  if (consume(")")) { token->type = CLOSE_P; return token; }
  if (consume("{")) { token->type = OPEN_B; return token; }
  if (consume("}")) { token->type = CLOSE_B; return token; }
  if (consume("~")) { token->type = TILDE; return token; }
  if (consume("++")) { token->type = INC_TOK; return token; }
  if (consume("--")) { token->type = DEC_TOK; return token; }
  if (consume("+=")) { token->type = PLUS_EQ; return token; }
  if (consume("-=")) { token->type = MINUS_EQ; return token; }
  if (consume("*=")) { token->type = TIMES_EQ; return token; }
  if (consume("/=")) { token->type = DIV_EQ; return token; }
  if (consume("%=")) { token->type = MOD_EQ; return token; }
  if (consume("+")) { token->type = PLUS; return token; }
  if (consume("-")) { token->type = MINUS; return token; }
  if (consume("*")) { token->type = ASTERISK; return token; }
  if (consume("/")) { token->type = SLASH; return token; }
  if (consume("%")) { token->type = PERCENT; return token; }
  if (consume("&&")) { token->type = DOUBLE_AMPERSAND; return token; }
  if (consume("||")) { token->type = DOUBLE_PIPE; return token; }
  if (consume("&=")) { token->type = AND_EQ; return token; }
  if (consume("|=")) { token->type = OR_EQ; return token; }
  if (consume("^=")) { token->type = XOR_EQ; return token; }
  if (consume(">>=")) { token->type = SHR_EQ; return token; }
  if (consume("<<=")) { token->type = SHL_EQ; return token; }
  if (consume("&")) { token->type = AMPERSAND; return token; }
  if (consume("|")) { token->type = PIPE; return token; }
  if (consume("^")) { token->type = CARAT; return token; }
  if (consume(">>")) { token->type = SHIFT_R_TOK; return token; }
  if (consume("<<")) { token->type = SHIFT_L_TOK; return token; }
  if (consume("!=")) { token->type = NOT_EQUAL; return token; }
  if (consume("!")) { token->type = EXCLAMATION; return token; }
  if (consume("==")) { token->type = DOUBLE_EQUALS; return token; }
  if (consume(">=")) { token->type = GREATER_THAN_EQ; return token; }
  if (consume("<=")) { token->type = LESS_THAN_EQ; return token; }
  if (consume("=")) { token->type = EQUALS; return token; }
  if (consume(">")) { token->type = GREATER_THAN; return token; }
  if (consume("<")) { token->type = LESS_THAN; return token; }

  if (consume_identifier(token)) return token;
  if (consume_literal(token)) return token;

  free(token);
  return NULL;
}

struct TokenArray* lex(char* prog){
  program = prog;
  current = prog;

  struct TokenArray* result = create_token_array(1000);

  struct Token* current_token = consume_any();
  while (current_token != NULL){    
    token_array_append(result, current_token);
    current_token = consume_any();
  }

  if (!is_at_end()) {
    destroy_token_array(result);
    print_error();
    return NULL;
  }

  return result;
}