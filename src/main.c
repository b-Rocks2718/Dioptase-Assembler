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
#include "elf.h"

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
  bool is_kernel = false;
  bool debug_labels = false;
  const char** cli_defines = malloc(argc * sizeof(char*));
  int num_defines = 0;
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
    } else if (strcmp(argv[i], "-kernel") == 0){
      is_kernel = true;
    } else if (strcmp(argv[i], "-debug") == 0){
      debug_labels = true;
    } else if (strncmp(argv[i], "-D", 2) == 0){
      const char* def = argv[i] + 2;
      if (def[0] == '\0' || strchr(def, '=') == NULL){
        fprintf(stderr, "Invalid -D definition (expected -DNAME=value)\n");
        free(file_names);
        free(cli_defines);
        exit(1);
      }
      cli_defines[num_defines++] = def;
    } else if (argv[i][0] == '-'){
      fprintf(stderr, "Unrecognized flag %s. Allowed flags are -pre, -o, -kernel, -debug, or -DNAME=value\n", argv[i]);
      free(file_names);
      free(cli_defines);
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

  char** preprocessed = preprocess(num_files, file_names, is_kernel, argv, files);
  if (preprocessed == NULL) {
    free(file_names);
    free(files);
    free(cli_defines);
    return 1;
  }

  if (pre_only){
    for (int i = 0; i < num_files; ++i) printf("%s\n", preprocessed[i] + 1);
    
    free(file_names);
    free(files);
    for (int i = 0; i < num_files; ++i) free(preprocessed[i]);
    free(preprocessed);
    free(cli_defines);
    return 0;
  }

  set_cli_defines(num_defines, cli_defines);
  struct LabelList* labels = NULL;
  struct ProgramDescriptor* program = assemble(
    num_files,
    file_names,
    is_kernel,
    argv,
    preprocessed,
    debug_labels ? &labels : NULL
  );
  
  for (int i = 0; i < num_files; ++i) free(preprocessed[i]);
  free(preprocessed);
  free(file_names);
  free(files);
  free(cli_defines);

  if (program == NULL) {
    if (target_name_alloc != NULL) free(target_name_alloc);
    return 1;
  }

  // write output
  FILE* fptr = fopen(target_name, "w");

  if(fptr == NULL){
    fprintf(stderr, "Could not open output file\n");   
    exit(1);             
  }

  if (is_kernel) {
    // write raw instructions without ELF structure
    fprint_instruction_array_list(fptr, program->sections, true);

    destroy_program_descriptor(program);
  } else {
    // write elf header
    struct ElfHeader header = create_elf_header(program);
    fprint_elf_header(fptr, &header);

    // write program header table
    struct ElfProgramHeader* pht = create_PHT(program);
    fprint_pht(fptr, pht);
    free(pht);

    // write program data
    fprint_instruction_array_list(fptr, program->sections, false);

    destroy_program_descriptor(program);
  }

  
  // Append label metadata for the debugger.
  if (debug_labels){
    fprint_label_list(fptr, labels);
    destroy_label_list(labels);
  }

  fclose(fptr);
  if (target_name_alloc != NULL) {
    free(target_name_alloc);
  }

  return 0;
}
