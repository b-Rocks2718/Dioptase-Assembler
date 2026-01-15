#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assembler.h"
#include "instruction_array.h"
#include "label_list.h"
#include "preprocessor.h"
#include "elf.h"
#include "debug.h"

// Purpose: Environment variable pointing to the CRT source directory.
// Inputs/Outputs: Read via getenv when -crt is requested.
// Invariants/Assumptions: Directory contains crt0.s and arithmetic.s.
static const char* kCrtDirEnvVar = "DIOPTASE_CRT_DIR";

// Purpose: Environment variable pointing to the repo root for CRT lookup.
// Inputs/Outputs: Read via getenv when DIOPTASE_CRT_DIR is unset.
// Invariants/Assumptions: Repo root contains Dioptase-OS/crt.
static const char* kRepoRootEnvVar = "DIOPTASE_ROOT";

// Purpose: Relative CRT directory under the repo root.
// Inputs/Outputs: Joined with DIOPTASE_ROOT when DIOPTASE_CRT_DIR is unset.
// Invariants/Assumptions: Uses '/' as the host path separator.
static const char* kDefaultCrtRelDir = "Dioptase-OS/crt";

// Purpose: CRT files to prepend when -crt is used.
// Inputs/Outputs: Joined with the CRT directory to form full paths.
// Invariants/Assumptions: Order matters so _start is emitted first.
enum { kCrtFileCount = 2 };
static const char* const kCrtFileNames[kCrtFileCount] = {
  "crt0.s",
  "arithmetic.s",
};

// Purpose: Copy a string into heap storage.
// Inputs: src is a NUL-terminated string.
// Outputs: Returns a heap-allocated copy or NULL on allocation failure.
// Invariants/Assumptions: Caller must free the returned string.
static char* duplicate_string(const char* src) {
  if (src == NULL) return NULL;
  size_t len = strlen(src);
  char* copy = malloc(len + 1);
  if (copy == NULL) return NULL;
  memcpy(copy, src, len + 1);
  return copy;
}

// Purpose: Join two path components with a '/' separator when needed.
// Inputs: left and right are path components.
// Outputs: Returns a heap-allocated joined path or NULL on allocation failure.
// Invariants/Assumptions: Uses '/' as the host path separator.
static char* join_paths(const char* left, const char* right) {
  if (left == NULL || right == NULL) return NULL;
  size_t left_len = strlen(left);
  size_t right_len = strlen(right);
  int needs_sep = (left_len > 0 && left[left_len - 1] != '/');
  size_t total_len = left_len + (needs_sep ? 1 : 0) + right_len + 1;
  char* path = malloc(total_len);
  if (path == NULL) return NULL;
  if (needs_sep) {
    snprintf(path, total_len, "%s/%s", left, right);
  } else {
    snprintf(path, total_len, "%s%s", left, right);
  }
  return path;
}

// Purpose: Resolve the CRT directory using environment variables.
// Inputs: None.
// Outputs: Returns a heap-allocated directory path or NULL if unset/unavailable.
// Invariants/Assumptions: Prefers DIOPTASE_CRT_DIR, falls back to DIOPTASE_ROOT.
static char* resolve_crt_dir(void) {
  const char* dir = getenv(kCrtDirEnvVar);
  if (dir != NULL && dir[0] != '\0') {
    return duplicate_string(dir);
  }

  const char* root = getenv(kRepoRootEnvVar);
  if (root == NULL || root[0] == '\0') {
    return NULL;
  }

  return join_paths(root, kDefaultCrtRelDir);
}

// Purpose: Free an array of CRT path strings.
// Inputs: paths is the array to free, count is its length.
// Outputs: None.
// Invariants/Assumptions: Safe to call with NULL paths.
static void free_crt_paths(char** paths, int count) {
  if (paths == NULL) return;
  for (int i = 0; i < count; ++i) {
    free(paths[i]);
  }
  free(paths);
}

int main(int argc, const char *const *const argv){
  if (argc <= 0) {
    fprintf(stderr,"usage: %s <file name>\n",argv[0]);
    exit(1);
  }

  // Leave room for optional CRT inputs when -crt is used.
  int* file_names = malloc((argc + kCrtFileCount) * sizeof(int));
  int num_files = 0;

  const char* target_name = "./a.hex";
  char* target_name_alloc = NULL;

  // look for flags
  bool pre_only = false;
  bool is_kernel = false;
  bool debug_labels = false;
  bool include_crt = false;
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
    } else if (strcmp(argv[i], "-g") == 0){
      debug_labels = true;
    } else if (strcmp(argv[i], "-crt") == 0){
      include_crt = true;
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
      fprintf(stderr, "Unrecognized flag %s. Allowed flags are -pre, -o, -kernel, -g, -crt, or -DNAME=value\n", argv[i]);
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

  const char* const* input_args = argv;
  const char** input_args_alloc = NULL;
  char** crt_paths = NULL;

  if (include_crt) {
    char* crt_dir = resolve_crt_dir();
    if (crt_dir == NULL) {
      fprintf(stderr,
              "Assembler Error: -crt requires %s or %s to be set. "
              "%s should point to Dioptase-OS/crt; %s should point to the repo root.\n",
              kCrtDirEnvVar,
              kRepoRootEnvVar,
              kCrtDirEnvVar,
              kRepoRootEnvVar);
      free(file_names);
      free(cli_defines);
      exit(1);
    }

    // Prepend CRT sources so _start is emitted first in the output image.
    crt_paths = malloc(kCrtFileCount * sizeof(char*));
    if (crt_paths == NULL) {
      fprintf(stderr, "Assembler Error: failed to allocate CRT path list\n");
      free(crt_dir);
      free(file_names);
      free(cli_defines);
      exit(1);
    }
    for (int i = 0; i < kCrtFileCount; ++i) crt_paths[i] = NULL;
    for (int i = 0; i < kCrtFileCount; ++i) {
      crt_paths[i] = join_paths(crt_dir, kCrtFileNames[i]);
      if (crt_paths[i] == NULL) {
        fprintf(stderr, "Assembler Error: failed to allocate CRT path for %s\n",
                kCrtFileNames[i]);
        free(crt_dir);
        free(file_names);
        free(cli_defines);
        free_crt_paths(crt_paths, kCrtFileCount);
        exit(1);
      }
    }
    free(crt_dir);

    input_args_alloc = malloc((argc + kCrtFileCount) * sizeof(char*));
    for (int i = 0; i < argc; ++i) input_args_alloc[i] = argv[i];
    for (int i = 0; i < kCrtFileCount; ++i) {
      input_args_alloc[argc + i] = crt_paths[i];
    }
    input_args = input_args_alloc;

    for (int i = num_files - 1; i >= 0; --i) {
      file_names[i + kCrtFileCount] = file_names[i];
    }
    for (int i = 0; i < kCrtFileCount; ++i) {
      file_names[i] = argc + i;
    }
    num_files += kCrtFileCount;
  }

  char const** const files = malloc(num_files * sizeof(char**));

  for (int i = 0; i < num_files; ++i){
    // open the files
    const char* file_path = input_args[file_names[i]];
    int fd = open(file_path,O_RDONLY);
    if (fd < 0) {
      fprintf(stderr, "Failed to open source file %s: %s\n", file_path, strerror(errno));
      exit(1);
    }

    // determine its size (std::filesystem::get_size?)
    struct stat file_stats;
    int rc = fstat(fd,&file_stats);
    if (rc != 0) {
      fprintf(stderr, "Failed to stat source file %s: %s\n", file_path, strerror(errno));
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
      fprintf(stderr, "Failed to map source file %s: %s\n", file_path, strerror(errno));
      free(file_names);
      free(files);
      exit(1);
    }
    files[i] = src;
  }

  char** preprocessed = preprocess(num_files, file_names, is_kernel, input_args, files);
  if (preprocessed == NULL) {
    free(file_names);
    free(files);
    free(cli_defines);
    free(input_args_alloc);
    free_crt_paths(crt_paths, kCrtFileCount);
    return 1;
  }

  if (pre_only){
    for (int i = 0; i < num_files; ++i) printf("%s\n", preprocessed[i] + 1);
    
    free(file_names);
    free(files);
    for (int i = 0; i < num_files; ++i) free(preprocessed[i]);
    free(preprocessed);
    free(cli_defines);
    free(input_args_alloc);
    free_crt_paths(crt_paths, kCrtFileCount);
    return 0;
  }

  set_cli_defines(num_defines, cli_defines);
  struct LabelList* labels = NULL;
  struct DebugInfoList* labels_c = NULL;
  struct ProgramDescriptor* program = assemble(
    num_files,
    file_names,
    is_kernel,
    input_args,
    preprocessed,
    debug_labels ? &labels : NULL,
    debug_labels ? &labels_c : NULL
  );
  
  for (int i = 0; i < num_files; ++i) free(preprocessed[i]);
  free(preprocessed);
  free(file_names);
  free(files);
  free(cli_defines);
  free(input_args_alloc);
  free_crt_paths(crt_paths, kCrtFileCount);

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
    fprint_debug_info_list(fptr, labels_c);
    destroy_debug_info_list(labels_c);
  }

  fclose(fptr);
  if (target_name_alloc != NULL) {
    free(target_name_alloc);
  }

  return 0;
}
