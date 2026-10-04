#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#include "instruction_array.h"
#include <stdint.h>

enum {
  kWordBytes = 4,
  kByteMask = 0xFF,
  kByteStride = 1
};

/*
  Linked list for holding instruction arrays
*/

// Allocate an empty list; arrays are added with instruction_array_list_append.
struct InstructionArrayList* create_instruction_array_list(void){
  struct InstructionArrayList* list = malloc(sizeof(struct InstructionArrayList));
  list->head = NULL;
  list->tail = NULL;
  return list;
}

// Append an item to instruction array list.
void instruction_array_list_append(struct InstructionArrayList* list, struct InstructionArray* arr){
  if (list->head == NULL){
    list->head = arr;
    list->tail = arr;
  } else {
    assert(list->tail != NULL);
    list->tail->next = arr;
    list->tail = arr;
  }
}

// Free all instruction-array nodes and their list wrapper.
void destroy_instruction_array_list(struct InstructionArrayList* list){
  destroy_instruction_array(list->head);
  free(list);
}

// Write every instruction array to a selected stream, optionally omitting origins.
void fprint_instruction_array_list(FILE* ptr, struct InstructionArrayList* list, bool raw){
  fprint_instruction_array(ptr, list->head, raw);
}

// Write a 32-bit word in little-endian byte order.
// ptr is open for binary output.
static void write_u32_le(FILE* ptr, uint32_t value){
  uint8_t bytes[kWordBytes];
  bytes[0] = (uint8_t)(value & kByteMask);
  bytes[1] = (uint8_t)((value >> 8) & kByteMask);
  bytes[2] = (uint8_t)((value >> 16) & kByteMask);
  bytes[3] = (uint8_t)((value >> 24) & kByteMask);
  fwrite(bytes, 1, kWordBytes, ptr);
}

// Emit zero padding bytes.
// ptr is open for binary output.
static void write_zero_bytes(FILE* ptr, size_t count){
  enum { kZeroChunkBytes = 256 };
  static const uint8_t zeros[kZeroChunkBytes] = {0};
  while (count > 0){
    size_t chunk = count > kZeroChunkBytes ? kZeroChunkBytes : count;
    fwrite(zeros, 1, chunk, ptr);
    count -= chunk;
  }
}

// Emit one array's words as little-endian bytes.
// ptr is open for binary output.
static void write_instruction_array_words(FILE* ptr, const struct InstructionArray* arr){
  for (size_t i = 0; i < arr->size; ++i){
    write_u32_le(ptr, (uint32_t)arr->instructions[i]);
  }
}

// Emit every array in list order, inserting origin padding when requested.
void fwrite_instruction_array_list(FILE* ptr, struct InstructionArrayList* list, bool include_origin_padding){
  uint32_t cursor = 0;
  for (struct InstructionArray* arr = list->head; arr != NULL; arr = arr->next){
    uint32_t origin = (uint32_t)arr->origin;
    if (include_origin_padding){
      assert(origin >= cursor);
      if (origin > cursor){
        write_zero_bytes(ptr, (size_t)(origin - cursor));
      }
      cursor = origin;
    }
    write_instruction_array_words(ptr, arr);
    cursor += (uint32_t)(arr->size * kWordBytes);
  }
}

/*
  Dynamic array used for holding instructions
*/

// Allocate an empty array of `capacity` words placed at byte address origin.
struct InstructionArray* create_instruction_array(size_t capacity, int origin){
  int* instructions = malloc(sizeof(int) * capacity);

  struct InstructionArray* arr = malloc(sizeof(struct InstructionArray));

  arr->capacity = capacity;
  arr->size = 0;
  arr->instructions = instructions;
  arr->origin = origin;
  arr->next = NULL;

  return arr;
}

// Grow the backing storage when needed and append one encoded word.
void instruction_array_append(struct InstructionArray* arr, int value){
  if (arr->size == arr->capacity){
    arr->instructions = realloc(arr->instructions, arr->capacity * sizeof(int) * 2);
    arr->capacity = arr->capacity * 2;
  } 
    
  arr->instructions[arr->size] = value;
  arr->size++;
}

// Update a byte within an existing 32-bit word.
static void set_word_byte(int* word, int byte_index, uint8_t value){
  uint32_t mask = (uint32_t)kByteMask << (8 * byte_index);
  uint32_t updated = ((uint32_t)(*word) & ~mask) | ((uint32_t)value << (8 * byte_index));
  *word = (int)updated;
}

// Append a 16-bit value at its byte address in little-endian order.
void instruction_array_append_double(struct InstructionArray* arr, uint16_t value, int pc){
  // Little-endian: low byte goes at the lowest address.
  instruction_array_append_byte(arr, (uint8_t)(value & kByteMask), pc);
  instruction_array_append_byte(arr, (uint8_t)((value >> 8) & kByteMask), pc + kByteStride);
}

// Append an 8-bit value at its byte address.
void instruction_array_append_byte(struct InstructionArray* arr, uint8_t value, int pc){
  int byte_index = pc % kWordBytes;
  if (byte_index == 0){
    instruction_array_append(arr, 0);
  }
  assert(arr->size > 0);
  set_word_byte(&arr->instructions[arr->size - 1], byte_index, value);
}

// Free an instruction-array node and every successor in its chain.
void destroy_instruction_array(struct InstructionArray* arr){
  while (arr != NULL){
    struct InstructionArray* next = arr->next;
    free(arr->instructions);
    free(arr);
    arr = next;
  }
}

// Write this array chain as hex words, with word-address origins for raw images.
// Words are buffered so each instruction is not a separate stdio call.
void fprint_instruction_array(FILE* ptr, struct InstructionArray* arr, bool raw){
  enum { kHexChunkBytes = 4096 };
  char chunk[kHexChunkBytes];
  size_t used = 0;
  static const char kHexDigits[] = "0123456789ABCDEF";

  for (; arr != NULL; arr = arr->next) {
    // raw => no ELF structure => put origin markers
    if (raw) {
      if (used > 0) {
        fwrite(chunk, 1, used, ptr);
        used = 0;
      }
      fprintf(ptr, "@%X\n", arr->origin / 4);
    }
    for (size_t i = 0; i < arr->size; ++i) {
      if (used + 9 > kHexChunkBytes) {
        fwrite(chunk, 1, used, ptr);
        used = 0;
      }
      uint32_t word = (uint32_t)arr->instructions[i];
      for (int shift = 28; shift >= 0; shift -= 4) {
        chunk[used++] = kHexDigits[(word >> shift) & 0xF];
      }
      chunk[used++] = '\n';
    }
  }
  if (used > 0) fwrite(chunk, 1, used, ptr);
}

// Return the total number of words in all arrays in the list.
size_t instruction_array_list_size(struct InstructionArrayList* list){
  size_t total_size = 0;
  struct InstructionArray* curr = list->head;
  while (curr != NULL){
    total_size += curr->size;
    curr = curr->next;
  }
  return total_size;
}
