#include <stdlib.h>
#include <stdio.h>

#include "preprocessor.h"

// copy the program into a new string, but without the comments
// expand macros into real instructions
char * preprocess(char const* prog){
  size_t prog_index = 0;
  size_t result_index = 0;

  size_t capacity = 5; // using a dynamic array here

  char * result = malloc(sizeof(char) * capacity);

  while (prog[prog_index] != 0){
    if (result_index == capacity - 1) { // leave room for null terminator
      // resize
      result = realloc(result, 2 * capacity);
      capacity = 2 * capacity;
    }

    // remove single line # comments
    if (prog[prog_index] == '#'){
      while (prog[prog_index] != '\n') {
        if (prog[prog_index] == '\0') goto end;
        prog_index++;
      }
    }

    // remove multi line /* */ comments
    if (prog[prog_index] == '/' && prog[prog_index + 1] == '*'){
      while (!(prog[prog_index] == '*' && prog[prog_index + 1] == '/')) {
        if (prog[prog_index] == '\0') goto end;
        prog_index++;
      }
      prog_index += 2;
    }

    result[result_index] = prog[prog_index];
    result_index++;
    prog_index++;
  }

  // include null terminator
  end:  result[result_index] = 0;

  return result;
}