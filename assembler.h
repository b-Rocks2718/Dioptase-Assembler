#pragma once

struct InstructionArray* assemble(char const* const file, char const* prog);

enum ConsumeResult {
  ERROR,
  NOT_FOUND,
  FOUND
};