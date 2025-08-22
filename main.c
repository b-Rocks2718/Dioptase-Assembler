#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assembler.h"
#include "instruction_array.h"
#include "preprocessor.h"

int main(int argc, const char *const *const argv){
  if (argc <= 0) {
    fprintf(stderr,"usage: %s <file name>\n",argv[0]);
    exit(1);
  }

  int* file_names = malloc(argc * sizeof(int));
  int num_files = 0;

  const char* target_name = "./a.hex";

  // look for flags
  bool pre_only = false;
  bool has_start = true;
  for (int i = 1; i < argc; ++i){
    if (strcmp(argv[i], "-pre") == 0){
      pre_only = true;
    } else if (strcmp(argv[i], "-o") == 0){
      // the next argument should be a file name
      if (i + 1 == argc){
        fprintf(stderr, "Must specify a target name after -o flag\n");
        free(file_names);
        exit(1);
      }
      target_name = argv[i + 1];
      ++i;
    } else if (strcmp(argv[i], "-nostart") == 0){
      has_start = false;
    } else if (argv[i][0] == '-'){
      fprintf(stderr, "Unrecognized flag %s. Allowed flags are -pre or -o\n", argv[i]);
      free(file_names);
      exit(1);
    } else {
      // this is a file, record the index
      file_names[num_files] = i;
      num_files++;
    }
  }

  if (num_files <= 0) {
    fprintf(stderr,"Must pass at least one source file\n");
    exit(1);
  }

  char const** const files = malloc(num_files * sizeof(char**));

  for (int i = 0; i < num_files; ++i){
    // open the files
    int fd = open(argv[file_names[i]],O_RDONLY);
    if (fd < 0) {
        perror("open");
        exit(1);
    }

    // determine its size (std::filesystem::get_size?)
    struct stat file_stats;
    int rc = fstat(fd,&file_stats);
    if (rc != 0) {
        perror("fstat");
        exit(1);
    }

    // map the file in my address space
    char const* const src = (char const * const)mmap(
        0,
        file_stats.st_size,
        PROT_READ,
        MAP_PRIVATE,
        fd,
        0);
    if (src == MAP_FAILED) {
      perror("mmap");
      free(file_names);
      free(files);
      exit(1);
    }
    files[i] = src;
  }

  char** preprocessed = preprocess(num_files, file_names, has_start, argv, files);
  if (preprocessed == NULL) {
    free(file_names);
    free(files);
    return 1;
  }

  if (pre_only){
    for (int i = 0; i < num_files; ++i) printf("%s\n", preprocessed[i] + 1);
    
    free(file_names);
    free(files);
    for (int i = 0; i < num_files; ++i) free(preprocessed[i]);
    free(preprocessed);
    return 0;
  }

  struct InstructionArrayList* instructions = assemble(num_files, file_names, argv, preprocessed);
  
  for (int i = 0; i < num_files; ++i) free(preprocessed[i]);
  free(preprocessed);
  free(file_names);
  free(files);

  if (instructions == NULL) return 1;

  // write output
  FILE* fptr = fopen(target_name, "w");

  if(fptr == NULL){
    fprintf(stderr, "Could not open output file\n");   
    exit(1);             
  }

  fprint_instruction_array_list(fptr, instructions);

  fclose(fptr);
  destroy_instruction_array_list(instructions);

  return 0;
}
