// Yaz0 decompression for shader_split.py (loaded through ctypes; built on first use by yaz0_native). The same format as its Python yaz0, about a hundred times faster:
// the pure-Python decoder took 39 of 59 s of a sample of Mario Kart 8 Deluxe's RomFS, where nearly every file is a Yaz0 .szs.
// Returns 0 with dst filled, or -1 on input that runs out or refers before the start of the output (the caller then falls back to the Python decoder and its error).
#include <stddef.h>
#include <stdint.h>

int yaz0_decode(const uint8_t *src, size_t slen, uint8_t *dst, size_t n) {
    size_t si = 16, o = 0;
    if (slen < 16) return -1;
    while (o < n) {
        if (si >= slen) return -1;
        unsigned h = src[si++];
        for (int bit = 0; bit < 8 && o < n; bit++, h <<= 1) {
            if (h & 0x80) {
                if (si >= slen) return -1;
                dst[o++] = src[si++];
            } else {
                if (si + 2 > slen) return -1;
                unsigned b1 = src[si], b2 = src[si + 1]; si += 2;
                size_t dist = (((size_t)(b1 & 15) << 8) | b2) + 1, ln = b1 >> 4;
                if (ln) ln += 2;
                else { if (si >= slen) return -1; ln = (size_t)src[si++] + 0x12; }
                if (dist > o) return -1;
                if (ln > n - o) ln = n - o;   // the Python decoder also stops at n
                for (size_t k = 0; k < ln; k++, o++) dst[o] = dst[o - dist];   // byte by byte: an overlapping copy repeats its period
            }
        }
    }
    return 0;
}
