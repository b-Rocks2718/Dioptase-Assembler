#ifndef PREPROCESSOR_H
#define PREPROCESSOR_H

#include <stdbool.h>

char** preprocess(int num_files, int* file_names, bool has_start,
  const char *const *const argv, const char * const * const files);

#endif  // PREPROCESSOR_H
