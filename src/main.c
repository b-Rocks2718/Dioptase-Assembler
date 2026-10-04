#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assembler.h"
#include "instruction_array.h"
#include "label_list.h"
#include "preprocessor.h"
#include "elf.h"
#include "debug.h"

// CRT files to prepend when -crt is used. Order matters so _start is emitted first.
enum { kCrtFileCount = 2 };
static const char* const kCrtFileNames[kCrtFileCount] = {
  "crt0.s",
  "arithmetic.s",
};

// Parsed command line. inputs lists every source in assembly order, with
// the CRT files first when -crt is given; CRT paths are owned in crt_paths.
struct Options {
  const char* target;
  bool pre_only;
  bool is_kernel;
  bool debug_labels;
  bool output_binary;
  const char** defines;
  int num_defines;
  const char** inputs;
  int num_inputs;
  char* crt_paths[kCrtFileCount];
};

// Join two path components with a '/' separator when needed.
// Returns a heap-allocated joined path or NULL on allocation failure.
static char* join_paths(const char* left, const char* right) {
  size_t left_len = strlen(left);
  size_t right_len = strlen(right);
  int needs_sep = (left_len > 0 && left[left_len - 1] != '/');
  size_t total_len = left_len + (needs_sep ? 1 : 0) + right_len + 1;
  char* path = malloc(total_len);
  if (path == NULL) return NULL;
  snprintf(path, total_len, needs_sep ? "%s/%s" : "%s%s", left, right);
  return path;
}

// Parse argv into opts. Returns false after printing the problem.
// opts must be zeroed; free_options releases it whether or not parsing succeeded.
static bool parse_args(int argc, const char* const* argv, struct Options* opts) {
  // Leave room for the CRT inputs ahead of the user's files.
  opts->inputs = malloc((size_t)(argc + kCrtFileCount) * sizeof(*opts->inputs));
  opts->defines = malloc((size_t)argc * sizeof(*opts->defines));
  if (opts->inputs == NULL || opts->defines == NULL) {
    fprintf(stderr, "Assembler Error: failed to allocate argument tables for %d arguments\n", argc);
    return false;
  }

  const char* crt_dir = NULL;
  const char* target = NULL;
  int first_user_input = kCrtFileCount;  // CRT slots are filled in below
  opts->num_inputs = first_user_input;

  for (int i = 1; i < argc; ++i){
    const char* arg = argv[i];
    if (strcmp(arg, "-pre") == 0){
      opts->pre_only = true;
    } else if (strcmp(arg, "-o") == 0){
      if (i + 1 == argc){
        fprintf(stderr, "Must specify a target name after -o flag\n");
        return false;
      }
      target = argv[++i];
    } else if (strcmp(arg, "-bin") == 0){
      opts->output_binary = true;
    } else if (strcmp(arg, "-kernel") == 0){
      opts->is_kernel = true;
    } else if (strcmp(arg, "-g") == 0){
      opts->debug_labels = true;
    } else if (strcmp(arg, "-crt") == 0){
      if (i + 1 == argc){
        fprintf(stderr, "Must specify a CRT directory after -crt\n");
        return false;
      }
      crt_dir = argv[++i];
    } else if (strncmp(arg, "-D", 2) == 0){
      opts->defines[opts->num_defines++] = arg + 2;
    } else if (arg[0] == '-'){
      fprintf(stderr, "Unrecognized flag %s. Allowed flags are -pre, -o, -bin, -kernel, -g, -crt <dir>, or -DNAME=value\n", arg);
      return false;
    } else {
      opts->inputs[opts->num_inputs++] = arg;
    }
  }

  if (opts->num_inputs == first_user_input) {
    fprintf(stderr, "Must pass at least one source file\n");
    return false;
  }
  if (opts->output_binary && opts->debug_labels){
    fprintf(stderr, "Assembler Error: -bin output does not support -g debug labels\n");
    return false;
  }
  opts->target = target != NULL ? target : (opts->output_binary ? "./a.bin" : "./a.hex");

  if (crt_dir != NULL) {
    // Prepend CRT sources so _start is emitted first in the output image.
    for (int i = 0; i < kCrtFileCount; ++i) {
      opts->crt_paths[i] = join_paths(crt_dir, kCrtFileNames[i]);
      if (opts->crt_paths[i] == NULL) {
        fprintf(stderr, "Assembler Error: failed to allocate CRT path for %s\n", kCrtFileNames[i]);
        return false;
      }
      opts->inputs[i] = opts->crt_paths[i];
    }
  } else {
    // No CRT: close the gap left by the reserved slots.
    opts->num_inputs -= kCrtFileCount;
    memmove(opts->inputs, opts->inputs + kCrtFileCount, (size_t)opts->num_inputs * sizeof(*opts->inputs));
  }
  return true;
}

// Release everything parse_args allocated.
static void free_options(struct Options* opts) {
  for (int i = 0; i < kCrtFileCount; ++i) free(opts->crt_paths[i]);
  free(opts->inputs);
  free(opts->defines);
}

// Read a whole source file into a heap buffer with a terminating NUL, which
// the preprocessor relies on to find the end of input. Uses only stdio so the
// assembler can later be hosted on Dioptase without an mmap equivalent.
// Returns NULL after printing a diagnostic naming the file and the failing step.
static char* read_source_file(const char* path) {
  FILE* f = fopen(path, "rb");
  if (f == NULL) {
    fprintf(stderr, "Failed to open source file %s: %s\n", path, strerror(errno));
    return NULL;
  }
  size_t cap = 4096;
  size_t len = 0;
  char* buf = malloc(cap);
  while (buf != NULL) {
    len += fread(buf + len, 1, cap - len - 1, f);
    if (ferror(f)) {
      fprintf(stderr, "Failed to read source file %s: %s\n", path, strerror(errno));
      free(buf);
      fclose(f);
      return NULL;
    }
    if (feof(f)) break;
    char* grown = realloc(buf, cap * 2);
    if (grown == NULL) free(buf);
    buf = grown;
    cap *= 2;
  }
  fclose(f);
  if (buf == NULL) {
    fprintf(stderr, "Assembler Error: failed to buffer source file %s\n", path);
    return NULL;
  }
  buf[len] = '\0';
  return buf;
}

// Free `count` NUL-terminated buffers and the array holding them.
static void free_buffers(char** buffers, int count) {
  if (buffers == NULL) return;
  for (int i = 0; i < count; ++i) free(buffers[i]);
  free(buffers);
}

// Read and preprocess every input. Returns NULL after reporting an error.
static char** load_sources(const struct Options* opts) {
  char** sources = calloc((size_t)opts->num_inputs, sizeof(*sources));
  if (sources == NULL) {
    fprintf(stderr, "Assembler Error: failed to allocate source file table for %d files\n", opts->num_inputs);
    return NULL;
  }
  for (int i = 0; i < opts->num_inputs; ++i){
    sources[i] = read_source_file(opts->inputs[i]);
    if (sources[i] == NULL) {
      free_buffers(sources, i);
      return NULL;
    }
  }
  // The preprocessed buffers own the text assemble() reads, so the raw
  // sources can be released before the two assembly passes.
  char** preprocessed = preprocess(opts->num_inputs, opts->inputs, (const char* const*)sources);
  free_buffers(sources, opts->num_inputs);
  return preprocessed;
}

// Write the assembled program in the selected format:
//   -kernel: raw image (hex with "@word-address" origin markers, or bytes
//            padded to each origin with -bin);
//   user:    ELF header, program header table, then the segments.
// With -g (hex only), label and debug records are appended as "#..." lines.
static void write_program(FILE* out, const struct Options* opts, struct ProgramDescriptor* program,
                          const struct LabelList* labels, struct DebugInfoList* debug) {
  if (!opts->is_kernel) {
    struct ElfHeader header = create_elf_header(program);
    struct ElfProgramHeader* pht = create_PHT(program);
    if (opts->output_binary) {
      fwrite_elf_header(out, &header);
      fwrite_pht(out, pht);
    } else {
      fprint_elf_header(out, &header);
      fprint_pht(out, pht);
    }
    free(pht);
  }

  // Kernel images carry their own origins; ELF segments are placed by the PHT.
  if (opts->output_binary) {
    fwrite_instruction_array_list(out, program->sections, opts->is_kernel);
  } else {
    fprint_instruction_array_list(out, program->sections, opts->is_kernel);
  }

  if (opts->debug_labels) {
    if (opts->is_kernel) fprint_label_list_kernel(out, labels);
    else fprint_label_list(out, labels);
    fprint_debug_info_list(out, debug);
  }
}

// Assemble the requested source files and write the image (and -g records).
int main(int argc, const char *const *const argv){
  int status = 1;
  struct Options opts;
  memset(&opts, 0, sizeof(opts));
  char** preprocessed = NULL;
  struct ProgramDescriptor* program = NULL;
  struct LabelList* labels = NULL;
  struct DebugInfoList* debug = NULL;

  if (!parse_args(argc, argv, &opts)) goto done;
  if (!set_cli_defines(opts.num_defines, opts.defines)) goto done;

  preprocessed = load_sources(&opts);
  if (preprocessed == NULL) goto done;

  if (opts.pre_only){
    // Skip each buffer's leading NUL sentinel.
    for (int i = 0; i < opts.num_inputs; ++i) printf("%s\n", preprocessed[i] + 1);
    status = 0;
    goto done;
  }

  program = assemble(opts.num_inputs, opts.inputs, preprocessed, opts.is_kernel,
                     opts.debug_labels ? &labels : NULL, opts.debug_labels ? &debug : NULL);
  if (program == NULL) goto done;

  FILE* out = fopen(opts.target, opts.output_binary ? "wb" : "w");
  if (out == NULL){
    fprintf(stderr, "Assembler Error: could not open output file %s: %s\n", opts.target, strerror(errno));
    goto done;
  }
  write_program(out, &opts, program, labels, debug);
  if (fclose(out) != 0) {
    fprintf(stderr, "Assembler Error: failed to write output file %s: %s\n", opts.target, strerror(errno));
    goto done;
  }
  status = 0;

done:
  if (program != NULL) destroy_program_descriptor(program);
  destroy_label_list(labels);
  destroy_debug_info_list(debug);
  free_buffers(preprocessed, opts.num_inputs);
  free_options(&opts);
  return status;
}
