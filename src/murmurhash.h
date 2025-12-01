#include <stddef.h>
#include <stdint.h>

/* Our hash function is MurmurHash2, 64 bit version.
 * It was modified for Redis in order to provide the same result in
 * big and little endian archs (endian neutral). */
uint64_t MurmurHash64A(const void * key, size_t len, unsigned int seed);
