#pragma once

// Minimal ZIP reader for EPUB (deflate / store). Uses ROM tinfl on ESP32-C5.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Extract one named member into *out (caller frees). Returns byte count, or 0.
size_t ebook_zip_extract(FILE *f, const char *name, uint8_t **out);

// Call fn(name, local_off, method, comp, uncomp, ctx) for each file.
// Returns number of files visited, or -1 on a bad archive.
typedef void (*ebook_zip_walk_fn)(const char *name, uint32_t local_off, uint16_t method,
                                  uint32_t comp, uint32_t uncomp, void *ctx);
int ebook_zip_walk(FILE *f, ebook_zip_walk_fn fn, void *ctx);

// Inflate raw deflate (ZIP method 8) into dst. Returns bytes written, or 0.
size_t ebook_inflate(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap);
