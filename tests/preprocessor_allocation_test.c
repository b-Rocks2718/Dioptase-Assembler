/*
 * Verifies that preprocessing releases its result table and every completed
 * file when any allocation fails. Allocation interception is limited to the
 * included preprocessor translation unit so linked assembler helpers retain
 * their normal host allocation behavior.
 */

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

static size_t live_allocations;
static size_t allocation_calls;
static size_t fail_on_call;

static void* tracked_malloc(size_t size) {
  if (++allocation_calls == fail_on_call) return NULL;
  void* ptr = malloc(size);
  if (ptr != NULL) live_allocations++;
  return ptr;
}

static void tracked_free(void* ptr) {
  if (ptr != NULL) {
    assert(live_allocations > 0 && "free must match a tracked allocation");
    live_allocations--;
  }
  free(ptr);
}

#define malloc tracked_malloc
#define free tracked_free
#include "preprocessor.c"
#undef malloc
#undef free

int main(void) {
  const size_t allocation_count = 3;
  int file_names[] = {0, 1};
  const char* argv[] = {"first.s", "second.s"};
  const char* files[] = {"", ""};

  for (size_t failure = 1; failure <= allocation_count; failure++) {
    allocation_calls = 0;
    fail_on_call = failure;
    char** outputs = preprocess(2, file_names, true, argv, files);
    assert(outputs == NULL && "preprocessing must report allocation failure");
    assert(live_allocations == 0 && "failure must release all partial output");
  }

  fail_on_call = 0;
  char** outputs = preprocess(2, file_names, true, argv, files);
  assert(outputs != NULL && "successful preprocessing must still work");
  tracked_free(outputs[0]);
  tracked_free(outputs[1]);
  tracked_free(outputs);
  assert(live_allocations == 0 && "success transfers output ownership to the caller");
  return 0;
}
