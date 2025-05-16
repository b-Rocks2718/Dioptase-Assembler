#pragma once

struct InstructionArray* assemble(char* prog);

enum ConsumeResult {
  ERROR,
  NOT_FOUND,
  FOUND
};