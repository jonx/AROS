/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: arc4random_buf()/arc4random()/arc4random_uniform() for AROS.

    AROS has no entropy source of its own, so on a *hosted* build the real,
    unpredictable bytes are borrowed from the host libc's arc4random_buf(), bound
    through <aros/hostbind.h> (which wraps hostlib.resource). On a native build (or
    if the host lacks the symbol) this degrades to a non-cryptographic mix of
    address-space + task entropy -- a documented stopgap until AROS grows a real
    entropy source.

    Independent work: the CSPRNG bytes come entirely from the host; nothing here
    reimplements the RC4/ChaCha algorithm. The unbiased-modulo helper is the
    standard rejection technique, written from the arithmetic.
*/

#include <proto/exec.h>
#include <aros/hostbind.h>

#include <stddef.h>
#include <stdint.h>

typedef void (*arc4_buf_fn)(void *buf, size_t n);

static arc4_buf_fn host_arc4random_buf = NULL;
static int resolved = 0;

/* Non-cryptographic fallback (native, or host without arc4random_buf): SplitMix64
   over address-space + task entropy. Enough for HashMap keys to differ per run;
   NOT secure. */
static void weak_fill(void *buf, size_t n)
{
    static uint64_t x = 0;
    uint8_t *p = (uint8_t *)buf;

    x ^= (uint64_t)(uintptr_t)&buf;
    x ^= (uint64_t)(uintptr_t)FindTask(NULL);

    while (n)
    {
        uint64_t z;
        size_t k, i;

        x += 0x9E3779B97F4A7C15ULL;
        z = x;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        z ^= z >> 31;

        k = n < 8 ? n : 8;
        for (i = 0; i < k; i++)
            *p++ = (uint8_t)(z >> (i * 8));
        n -= k;
    }
}

void arc4random_buf(void *buf, size_t n)
{
    if (buf == NULL || n == 0)
        return;

    if (!resolved)
    {
        host_arc4random_buf = (arc4_buf_fn)HostBind_LibcSym("arc4random_buf");
        resolved = 1;
    }

    if (host_arc4random_buf)
        host_arc4random_buf(buf, n);
    else
        weak_fill(buf, n);
}

uint32_t arc4random(void)
{
    uint32_t v;
    arc4random_buf(&v, sizeof v);
    return v;
}

uint32_t arc4random_uniform(uint32_t upper_bound)
{
    uint32_t r, min;

    if (upper_bound < 2)
        return 0;

    /* 2^32 mod upper_bound -- reject the low, modulo-biased slice for uniformity. */
    min = (uint32_t)(-upper_bound) % upper_bound;
    do {
        r = arc4random();
    } while (r < min);

    return r % upper_bound;
}
