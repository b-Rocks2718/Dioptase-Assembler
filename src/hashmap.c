#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "hashmap.h"

// Allocate an empty hash map with the requested bucket count.
struct HashMap* create_hash_map(size_t num_buckets){
  struct HashEntry** arr = calloc(num_buckets, sizeof(struct HashEntry*));
  struct HashMap* hmap = malloc(sizeof(struct HashMap));
  if (arr == NULL || hmap == NULL) {
    fprintf(stderr, "Assembler hashmap: failed to allocate %zu buckets\n", num_buckets);
    free(arr);
    free(hmap);
    return NULL;
  }

  hmap->size = num_buckets;
  hmap->arr = arr;

  return hmap;
}

// Return the bucket head slot that key hashes to.
static struct HashEntry** bucket_for(const struct HashMap* hmap, const struct Slice* key){
  return &hmap->arr[hash_slice(key) % hmap->size];
}

// Walk key's collision chain and return its entry, or NULL.
struct HashEntry* hash_map_find(const struct HashMap* hmap, const struct Slice* key){
  for (struct HashEntry* entry = *bucket_for(hmap, key); entry != NULL; entry = entry->next){
    if (compare_slice_to_slice(&entry->key, key)) return entry;
  }
  return NULL;
}

// Insert or replace a key in the map. New entries go to the end of the chain
// so iteration order matches insertion order within a bucket.
void hash_map_insert(struct HashMap* hmap, const struct Slice* key, long value, bool is_def, bool is_data){
  struct HashEntry** link = bucket_for(hmap, key);
  for (; *link != NULL; link = &(*link)->next){
    if (compare_slice_to_slice(&(*link)->key, key)){
      (*link)->value = value;
      return;
    }
  }

  struct HashEntry* entry = malloc(sizeof(struct HashEntry));
  if (entry == NULL) {
    fprintf(stderr, "Assembler hashmap: failed to allocate entry for symbol \"%.*s\"\n",
            (int)key->len, key->start);
    exit(1);
  }
  entry->key = *key;
  entry->value = value;
  entry->is_defined = is_def;
  entry->is_data = is_data;
  entry->next = NULL;
  *link = entry;
}

// Look up a key in the map, returning its stored value.
long hash_map_get(const struct HashMap* hmap, const struct Slice* key){
  struct HashEntry* entry = hash_map_find(hmap, key);
  return entry == NULL ? 0 : entry->value;
}

// Return whether the map contains a key.
bool hash_map_contains(const struct HashMap* hmap, const struct Slice* key){
  return hash_map_find(hmap, key) != NULL;
}

// Return whether a label has been defined rather than only referenced.
bool label_has_definition(const struct HashMap* hmap, const struct Slice* key){
  struct HashEntry* entry = hash_map_find(hmap, key);
  return entry != NULL && entry->is_defined;
}

// Mark a map entry as defined and update its value.
void make_defined(struct HashMap* hmap, const struct Slice* key, long value){
  struct HashEntry* entry = hash_map_find(hmap, key);
  assert(entry != NULL);
  entry->is_defined = true;
  entry->value = value;
}

// Free every collision chain and the hash map's bucket storage.
void destroy_hash_map(struct HashMap* hmap){
  if (hmap == NULL) return;
  for (size_t i = 0; i < hmap->size; ++i){
    struct HashEntry* entry = hmap->arr[i];
    while (entry != NULL){
      struct HashEntry* next = entry->next;
      free(entry);
      entry = next;
    }
  }
  free(hmap->arr);
  free(hmap);
}
