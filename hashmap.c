#include <stdlib.h>
#include <stdbool.h>

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

struct HashEntry* create_hash_entry(struct Slice* key, long value){
  struct HashEntry* entry = malloc(sizeof(struct HashEntry));

  entry->key = key;
  entry->value = value;
  entry->next = NULL;

  return entry;
}

void hash_entry_insert(struct HashEntry* entry, struct Slice* key, long value){
  if (compare_slice_to_slice(entry->key, key)){
    entry->value = value;
    free(key);
  } else if (entry->next == NULL){
    entry->next = create_hash_entry(key, value);
  } else {
    hash_entry_insert(entry->next, key, value);
  }
}

void hash_map_insert(struct HashMap* hmap, struct Slice* key, long value){
  size_t hash = hash_slice(key) % hmap->size;
  
  if ((hmap->arr[hash]) == NULL){
    hmap->arr[hash] = create_hash_entry(key, value);
  } else {
    hash_entry_insert(hmap->arr[hash], key, value);
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

bool hash_map_contains(struct HashMap* hmap, struct Slice* key){
  size_t hash = hash_slice(key) % hmap->size;

  if (hmap->arr[hash] == NULL){
    return false;
  } else {
    return hash_entry_contains(hmap->arr[hash], key);
  }
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