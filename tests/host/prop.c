#include "prop.h"

#include <string.h>

prop_rng prop_seed(uint64_t seed) {
  return (prop_rng){.state = seed != 0 ? seed : UINT64_C(0x9e3779b97f4a7c15)};
}

uint64_t prop_next(prop_rng *rng) {
  rng->state ^= rng->state << 13;
  rng->state ^= rng->state >> 7;
  rng->state ^= rng->state << 17;
  return rng->state;
}

size_t prop_below(prop_rng *rng, size_t bound) {
  return bound == 0 ? 0 : (size_t)(prop_next(rng) % bound);
}

bool prop_chance(prop_rng *rng, unsigned int percent) {
  return prop_below(rng, 100) < percent;
}

/* Each pass either removes a chunk, which shortens the sequence, or halves
 * the chunk size, so the loop ends. */
size_t prop_shrink(void *items, size_t count, size_t size, void *scratch,
                   prop_holds_fn *holds, void *ctx) {
  unsigned char *base = items;
  unsigned char *work = scratch;
  size_t chunk = count / 2 != 0 ? count / 2 : 1;
  while (chunk != 0 && count != 0) {
    bool removed = false;
    size_t at = 0;
    while (at < count) {
      const size_t len = chunk < count - at ? chunk : count - at;
      memcpy(work, base, at * size);
      memcpy(work + at * size, base + (at + len) * size,
             (count - at - len) * size);
      if (!holds(work, count - len, ctx)) {
        count -= len;
        memcpy(base, work, count * size);
        removed = true;
      } else {
        at += len;
      }
    }
    if (!removed) {
      chunk /= 2;
    } else if (chunk > count) {
      chunk = count;
    }
  }
  return count;
}
