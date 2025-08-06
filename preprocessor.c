#include <stdlib.h>
#include <stdio.h>

#include "preprocessor.h"
#include "assembler.h"

static size_t prog_index = 0;
static size_t result_index = 0;

static char* result = NULL;
static size_t capacity = 50; // using a dynamic array here

// expand dynamic array if necessary
bool adjust_capacity(void){
  if (result_index == capacity - 2) { // leave room for null terminator
    // resize
    result = realloc(result, 2 * capacity);
    if (result == NULL) return false;
    capacity = 2 * capacity;
  }
  return true;
}

// remove single line # comments
bool skip_comments(char const* const prog){
  if (prog[prog_index] == '#'){
    while (prog[prog_index] != '\n') {
      if (prog[prog_index] == '\0') return false;
      prog_index++;
    }
  }
  return true;
}

void expand_macros(void){
  
}

// copy the program into a new string, but without the comments
// expand macros into real instructions
char * preprocess(char const* const prog){

  result = malloc(sizeof(char) * capacity);
  if (result == NULL) return NULL;

  // initial null used to detect start of program
  // used when printing errors
  result[result_index] = '\0';
  result_index++; 

  while (prog[prog_index] != '\0'){
    // expand dynamic array if necessary, exit if realloc fails 
    if (!adjust_capacity()) goto end;

    // skip comments, exit if EOF is reached
    if (!skip_comments(prog)) goto end;

    expand_macros();

    // write one character, then repeat loop
    result[result_index] = prog[prog_index];
    result_index++;
    prog_index++;
  }

  // include null terminator, realloc should ensure there's always room
  end:  result[result_index] = 0;

  return result;
}