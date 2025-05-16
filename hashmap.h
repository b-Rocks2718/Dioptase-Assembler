#pragma once

#include <stdint.h>

#include "slice.h"

struct HashEntry{
  struct Slice* key;
  long value;
  struct HashEntry* next;
};

struct HashMap{
	size_t size;
  struct HashEntry** arr;
};

struct HashMap* create_hash_map(size_t numBuckets);

void hash_map_insert(struct HashMap* hmap, struct Slice* key, long value);

long hash_map_get(struct HashMap* hmap, struct Slice* key);

bool hash_map_contains(struct HashMap* hmap, struct Slice* key);

void destroy_hash_map(struct HashMap* hmap);