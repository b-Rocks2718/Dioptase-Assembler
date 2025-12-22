#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assembler.h"
#include "instruction_array.h"
#include "label_list.h"
#include "preprocessor.h"

static bool ends_with(const char* str, const char* suffix) {
  size_t str_len = strlen(str);
  size_t suffix_len = strlen(suffix);
  if (str_len < suffix_len) return false;
  return strncmp(str + (str_len - suffix_len), suffix, suffix_len) == 0;
}

// Build output filename for debug mode: replace/append .debug, or return NULL if already .debug.
static char* build_debug_name(const char* name) {
  const char* debug_ext = ".debug";
  const char* hex_ext = ".hex";
  size_t name_len = strlen(name);
  if (ends_with(name, debug_ext)) {
    return NULL;
  }
  if (ends_with(name, hex_ext)) {
    size_t base_len = name_len - strlen(hex_ext);
    char* out = malloc(base_len + strlen(debug_ext) + 1);
    memcpy(out, name, base_len);
    memcpy(out + base_len, debug_ext, strlen(debug_ext) + 1);
    return out;
  }

  char* out = malloc(name_len + strlen(debug_ext) + 1);
  memcpy(out, name, name_len);
  memcpy(out + name_len, debug_ext, strlen(debug_ext) + 1);
  return out;
}

int main(int argc, const char *const *const argv){
  if (argc <= 0) {
    fprintf(stderr,"usage: %s <file name>\n",argv[0]);
    exit(1);
  }

  int* file_names = malloc(argc * sizeof(int));
  int num_files = 0;

  const char* target_name = "./a.hex";
  char* target_name_alloc = NULL;

  // look for flags
  bool pre_only = false;
  bool has_start = true;
  bool debug_labels = false;
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
    } else if (strcmp(argv[i], "-debug") == 0){
      debug_labels = true;
    } else if (argv[i][0] == '-'){
      fprintf(stderr, "Unrecognized flag %s. Allowed flags are -pre, -o, -nostart, or -debug\n", argv[i]);
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

  // Debug output uses a .debug extension so the emulator can parse labels.
  if (debug_labels) {
    target_name_alloc = build_debug_name(target_name);
    if (target_name_alloc != NULL) {
      target_name = target_name_alloc;
    }
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

  struct LabelList* labels = NULL;
  struct InstructionArrayList* instructions = assemble(
    num_files,
    file_names,
    argv,
    preprocessed,
    debug_labels ? &labels : NULL
  );
  
  for (int i = 0; i < num_files; ++i) free(preprocessed[i]);
  free(preprocessed);
  free(file_names);
  free(files);

  if (instructions == NULL) {
    if (target_name_alloc != NULL) free(target_name_alloc);
    return 1;
  }

  // write output
  FILE* fptr = fopen(target_name, "w");

  if(fptr == NULL){
    fprintf(stderr, "Could not open output file\n");   
    exit(1);             
  }

  fprint_instruction_array_list(fptr, instructions);
  // Append label metadata for the debugger.
  if (debug_labels){
    fprint_label_list(fptr, labels);
    destroy_label_list(labels);
  }

  fclose(fptr);
  destroy_instruction_array_list(instructions);
  if (target_name_alloc != NULL) {
    free(target_name_alloc);
  }

  return 0;
}
