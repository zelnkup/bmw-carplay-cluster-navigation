/* rgi_mem.c — memset/memcpy and the ARM EABI helpers the freestanding build needs (hudshapes.h + struct copies), defined
 * here so the binary imports nothing new from the car's libc. Byte-wise (safe at any alignment, -mno-unaligned-access). */
typedef unsigned long usize;
void *memset(void *d, int c, usize n) { volatile unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, usize n) { volatile unsigned char *a = d; const unsigned char *b = s; while (n--) *a++ = *b++; return d; }
void __aeabi_memcpy(void *d, const void *s, usize n) { memcpy(d, s, n); }
void __aeabi_memcpy4(void *d, const void *s, usize n) { memcpy(d, s, n); }
void __aeabi_memset(void *d, usize n, int c) { memset(d, c, n); }
void __aeabi_memset4(void *d, usize n, int c) { memset(d, c, n); }
void __aeabi_memset8(void *d, usize n, int c) { memset(d, c, n); }
void __aeabi_memclr(void *d, usize n) { memset(d, 0, n); }
void __aeabi_memclr4(void *d, usize n) { memset(d, 0, n); }
void __aeabi_memclr8(void *d, usize n) { memset(d, 0, n); }
