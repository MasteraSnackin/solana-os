// Tiny ZIP central-directory walker + tinfl inflate for EPUB members.

#include "ebook_zip.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"

#include "core/os_mem.h"

#ifndef SOLANA_OS_EMULATOR
#include "miniz.h"
#endif

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

size_t ebook_inflate(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap)
{
    if (!src || !dst || !src_len || !dst_cap) {
        return 0;
    }
#ifdef SOLANA_OS_EMULATOR
    (void)src;
    (void)src_len;
    (void)dst;
    (void)dst_cap;
    return 0;   // deflate only on the badge (ROM tinfl)
#else
    size_t n = tinfl_decompress_mem_to_mem(dst, dst_cap, src, src_len,
                                           TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (n == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) {
        return 0;
    }
    return n;
#endif
}

static bool find_eocd(FILE *f, uint32_t *cd_off, uint16_t *cd_entries)
{
    if (fseek(f, 0, SEEK_END) != 0) {
        return false;
    }
    long sz = ftell(f);
    if (sz < 22) {
        return false;
    }
    // EOCD is 22 bytes + optional comment (up to 64 KB). Scan the last 66 KB.
    long scan = sz < 65557L ? sz : 65557L;
    uint8_t *buf = heap_caps_malloc((size_t)scan, os_mem_bulk_caps());
    if (!buf) {
        return false;
    }
    if (fseek(f, sz - scan, SEEK_SET) != 0 || fread(buf, 1, (size_t)scan, f) != (size_t)scan) {
        free(buf);
        return false;
    }
    bool ok = false;
    for (long i = scan - 22; i >= 0; i--) {
        if (rd32(buf + i) == 0x06054b50u) {
            *cd_entries = rd16(buf + i + 10);
            *cd_off = rd32(buf + i + 16);
            ok = true;
            break;
        }
    }
    free(buf);
    return ok;
}

int ebook_zip_walk(FILE *f, ebook_zip_walk_fn fn, void *ctx)
{
    uint32_t cd_off = 0;
    uint16_t n = 0;
    if (!find_eocd(f, &cd_off, &n) || !fn) {
        return -1;
    }
    if (fseek(f, (long)cd_off, SEEK_SET) != 0) {
        return -1;
    }
    int count = 0;
    for (uint16_t i = 0; i < n; i++) {
        uint8_t hdr[46];
        if (fread(hdr, 1, 46, f) != 46 || rd32(hdr) != 0x02014b50u) {
            return -1;
        }
        uint16_t method = rd16(hdr + 10);
        uint32_t comp = rd32(hdr + 20);
        uint32_t uncomp = rd32(hdr + 24);
        uint16_t namelen = rd16(hdr + 28);
        uint16_t extralen = rd16(hdr + 30);
        uint16_t commentlen = rd16(hdr + 32);
        uint32_t local_off = rd32(hdr + 42);
        char name[256];
        if (namelen >= sizeof(name) || fread(name, 1, namelen, f) != namelen) {
            return -1;
        }
        name[namelen] = 0;
        if (fseek(f, extralen + commentlen, SEEK_CUR) != 0) {
            return -1;
        }
        fn(name, local_off, method, comp, uncomp, ctx);
        count++;
    }
    return count;
}

typedef struct {
    const char *want;
    uint32_t local_off;
    uint16_t method;
    uint32_t comp, uncomp;
    bool found;
} locate_t;

static void locate_cb(const char *name, uint32_t local_off, uint16_t method, uint32_t comp,
                      uint32_t uncomp, void *ctx)
{
    locate_t *L = ctx;
    if (!L->found && strcmp(name, L->want) == 0) {
        L->local_off = local_off;
        L->method = method;
        L->comp = comp;
        L->uncomp = uncomp;
        L->found = true;
    }
}

size_t ebook_zip_extract(FILE *f, const char *name, uint8_t **out)
{
    *out = NULL;
    locate_t L = { .want = name };
    if (ebook_zip_walk(f, locate_cb, &L) < 0 || !L.found || L.uncomp == 0 || L.uncomp > 2u * 1024u * 1024u) {
        return 0;
    }
    if (fseek(f, (long)L.local_off, SEEK_SET) != 0) {
        return 0;
    }
    uint8_t lhdr[30];
    if (fread(lhdr, 1, 30, f) != 30 || rd32(lhdr) != 0x04034b50u) {
        return 0;
    }
    uint16_t namelen = rd16(lhdr + 26);
    uint16_t extralen = rd16(lhdr + 28);
    if (fseek(f, namelen + extralen, SEEK_CUR) != 0) {
        return 0;
    }

    uint8_t *dst = heap_caps_malloc(L.uncomp + 1, os_mem_bulk_caps());
    if (!dst) {
        return 0;
    }
    if (L.method == 0) {
        if (fread(dst, 1, L.uncomp, f) != L.uncomp) {
            free(dst);
            return 0;
        }
        dst[L.uncomp] = 0;
        *out = dst;
        return L.uncomp;
    }
    if (L.method != 8) {
        free(dst);
        return 0;
    }
    uint8_t *comp = heap_caps_malloc(L.comp ? L.comp : 1, os_mem_bulk_caps());
    if (!comp) {
        free(dst);
        return 0;
    }
    if (fread(comp, 1, L.comp, f) != L.comp) {
        free(comp);
        free(dst);
        return 0;
    }
    size_t n = ebook_inflate(comp, L.comp, dst, L.uncomp);
    free(comp);
    if (n != L.uncomp) {
        free(dst);
        return 0;
    }
    dst[L.uncomp] = 0;
    *out = dst;
    return L.uncomp;
}
