#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>

#include "hashmap.h"
#include "slice.h"

struct HashMap* create_hash_map(size_t num_buckets){
  struct HashEntry** arr = malloc(num_buckets * sizeof(struct HashEntry*));
  struct HashMap* hmap = malloc(sizeof(struct HashMap));

  for (int i = 0; i < num_buckets; ++i){
    arr[i] = NULL;
  }

  hmap->size = num_buckets;
  hmap->arr = arr;

  return hmap;
}

struct HashEntry* create_hash_entry(struct Slice* key, long value, bool is_def){
  struct HashEntry* entry = malloc(sizeof(struct HashEntry));

  entry->key = key;
  entry->value = value;
  entry->is_defined = is_def;
  entry->next = NULL;

  return entry;
}

void hash_entry_insert(struct HashEntry* entry, struct Slice* key, long value, bool is_def){
  if (compare_slice_to_slice(entry->key, key)){
    entry->value = value;
    free(key);
  } else if (entry->next == NULL){
    entry->next = create_hash_entry(key, value, is_def);
  } else {
    hash_entry_insert(entry->next, key, value, is_def);
  }
}

void hash_map_insert(struct HashMap* hmap, struct Slice* key, long value, bool is_def){
  size_t hash = hash_slice(key) % hmap->size;
  
  if ((hmap->arr[hash]) == NULL){
    hmap->arr[hash] = create_hash_entry(key, value, is_def);
  } else {
    hash_entry_insert(hmap->arr[hash], key, value, is_def);
  }
}

long hash_entry_get(struct HashEntry* entry, struct Slice* key){
  if (compare_slice_to_slice(entry->key, key)){
    return entry->value;
  } else if (entry->next == NULL){
    return 0;
  } else {
    return hash_entry_get(entry->next, key);
  }
}

long hash_map_get(struct HashMap* hmap, struct Slice* key){
  size_t hash = hash_slice(key) % hmap->size;

  if (hmap->arr[hash] == NULL){
    return 0;
  } else {
    return hash_entry_get(hmap->arr[hash], key);
  }
}

bool hash_entry_contains(struct HashEntry* entry, struct Slice* key){
  if (compare_slice_to_slice(entry->key, key)){
    return true;
  } else if (entry->next == NULL){
    return false;
  } else {
    return hash_entry_contains(entry->next, key);
  }
}

bool hash_entry_contains_def(struct HashEntry* entry, struct Slice* key){
  if (compare_slice_to_slice(entry->key, key) && entry->is_defined){
    return true;
  } else if (entry->next == NULL){
    return false;
  } else {
    return hash_entry_contains(entry->next, key);
  }
}

bool hash_map_contains(struct HashMap* hmap, struct Slice* key){
  size_t hash = hash_slice(key) % hmap->size;

  if (hmap->arr[hash] == NULL){
    return false;
  } else {
    return hash_entry_contains(hmap->arr[hash], key);
  }
}

bool label_has_definition(struct HashMap* hmap, struct Slice* key){
  size_t hash = hash_slice(key) % hmap->size;

  if (hmap->arr[hash] == NULL){
    return false;
  } else {
    return hash_entry_contains_def(hmap->arr[hash], key);
  }
}

void make_entry_defined(struct HashEntry* entry, struct Slice* key, long value){
  if (compare_slice_to_slice(entry->key, key)){
    entry->is_defined = true;
    entry->value = value;
  } else {
    assert(entry->next != NULL);
    make_entry_defined(entry->next, key, value);
  }
}

void make_defined(struct HashMap* hmap, struct Slice* key, long value){
  size_t hash = hash_slice(key) % hmap->size;

  assert(hmap->arr[hash] != NULL);

  make_entry_defined(hmap->arr[hash], key, value);
}

void destroy_hash_entry(struct HashEntry* entry){
  if (entry->next !=  NULL) destroy_hash_entry(entry->next);
  free(entry->key);
  free(entry);
}

void destroy_hash_map(struct HashMap* hmap){
  for (int i = 0; i < hmap->size; ++i){
    if (hmap->arr[i] != NULL) destroy_hash_entry(hmap->arr[i]);
  }
  free(hmap->arr);
  free(hmap);
}
