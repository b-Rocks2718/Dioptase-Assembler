#ifndef HASHMAP_H
#define HASHMAP_H

#include <stdbool.h>
#include <stddef.h>

#include "slice.h"

// Store one symbol key, value, definition state, and section metadata.
// The key is a borrowed view into source text (or argv for -D names), which
// must outlive the map.
struct HashEntry{
  struct Slice key;
  long value;
  bool is_defined;
  bool is_data;
  struct HashEntry* next;
};

// Own the bucket array used for assembler symbol lookup.
struct HashMap{
  size_t size;
  struct HashEntry** arr;
};

// Allocate an empty map; returns NULL after reporting an allocation failure.
struct HashMap* create_hash_map(size_t num_buckets);

// Return the entry for key, or NULL when the key is absent.
struct HashEntry* hash_map_find(const struct HashMap* hmap, const struct Slice* key);

// Insert key with the given state. If key is already present only its value
// is replaced; the definition and data flags of the existing entry are kept.
void hash_map_insert(struct HashMap* hmap, const struct Slice* key, long value, bool is_def, bool is_data);

// Return the value stored for key, or 0 when the key is absent.
long hash_map_get(const struct HashMap* hmap, const struct Slice* key);

bool hash_map_contains(const struct HashMap* hmap, const struct Slice* key);

// Return whether key is present and has been defined rather than only declared.
bool label_has_definition(const struct HashMap* hmap, const struct Slice* key);

// Mark an existing key as defined with the given value. The key must be present.
void make_defined(struct HashMap* hmap, const struct Slice* key, long value);

// Free every entry and the map. Keys are borrowed and are not freed. NULL is allowed.
void destroy_hash_map(struct HashMap* hmap);

#endif  // HASHMAP_H
