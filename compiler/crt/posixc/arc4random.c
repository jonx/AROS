/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: arc4random_buf()/arc4random()/arc4random_uniform() for AROS.

    AROS has no entropy source of its own, so on a *hosted* build the real,
    unpredictable bytes are borrowed from the host libc's arc4random_buf(),
    reached through hostlib.resource -- the same way arch/all-unix/battclock
    borrows the host clock. On a native build (or if the host lacks the symbol)
    this degrades to a non-cryptographic mix of address-space + task entropy,
    which is a documented stopgap until AROS grows a real entropy source.

    Independent work: the CSPRNG bytes come entirely from the host; nothing here
    reimplements the RC4/ChaCha algorithm. The unbiased-modulo helper is the
    standard rejection technique, written from the arithmetic.
*/

#include <proto/exec.h>
#include <proto/hostlib.h>

#include <stddef.h>
#include <stdint.h>

/* We don't get a HOST_OS_* define in generic posixc (only arch/all-unix modules
   pass it), so probe the likely host C libraries by name at runtime. First that
   opens AND exports arc4random_buf wins; on native AROS none open -> weak fallback. */
static const char *LibcNames[] = { "libSystem.dylib", "libc.so.6", "libc.so", NULL };
static const char *Symbols[] = { "arc4random_buf", NULL };

struct RandIFace
{
    void (*arc4random_buf)(void *buf, size_t n);
};

static struct RandIFace *RandIFace_host = NULL;
static int resolved = 0;

static void resolve_host(void)
{
    APTR HostLibBase;
    int i;

    /* Set first so a preempting caller falls back to weak_fill for one call
       instead of racing us into a second resolve. */
    resolved = 1;

    HostLibBase = OpenResource("hostlib.resource");
    if (!HostLibBase)
        return;                         /* native build: no host to borrow from */

    for (i = 0; LibcNames[i]; i++)
    {
        APTR lib = HostLib_Open(LibcNames[i], NULL);
        if (!lib)
            continue;                   /* wrong name for this host */
        ULONG unresolved = 0;
        RandIFace_host = (struct RandIFace *)HostLib_GetInterface(lib, Symbols, &unresolved);
        if (RandIFace_host && !unresolved)
            return;                     /* got arc4random_buf; keep `lib` open */
        /* opened but no arc4random_buf -- release and try the next candidate */
        if (RandIFace_host)
        {
            HostLib_DropInterface((APTR)RandIFace_host);
            RandIFace_host = NULL;
        }
        HostLib_Close(lib, NULL);
    }
}

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
        resolve_host();

    if (RandIFace_host)
        RandIFace_host->arc4random_buf(buf, n);
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
