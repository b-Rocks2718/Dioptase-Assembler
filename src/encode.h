#ifndef ENCODE_H
#define ENCODE_H

#include "lexer.h"

// Parse the instruction at `current` and return its 32-bit encoding.
// Sets *result to NOT_FOUND (cursor unchanged) when the next token is not a
// mnemonic, or to ERROR after reporting a malformed instruction; leaves it
// untouched on success. Labels encode as 0 during pass 1.
int consume_instruction(enum ConsumeResult* result);

#endif  // ENCODE_H
