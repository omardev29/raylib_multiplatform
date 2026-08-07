#pragma once
// Minimal MD5 (RFC 1321) for the rres packing tool (standalone, no raylib).
// Returns a pointer to a static unsigned int[4] (16 bytes) with the digest.
#ifdef __cplusplus
extern "C" {
#endif
unsigned int *ComputeMD5(const void *data, unsigned int size);
#ifdef __cplusplus
}
#endif
