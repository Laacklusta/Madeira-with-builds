/*
 * ml734 Theorafile call tracer (x86-64 PE, opt-in diagnostic).
 *
 * The intro video decodes and plays, the file reaches a clean
 * STATUS_END_OF_FILE after 37,530 reads, the decoder then stops reading --
 * and the game never leaves VideoContext. The filesystem layer is therefore
 * exonerated, but file EOF is NOT decoder EOS: we still cannot see whether
 * tf_eos() is even called, what it returns, or whether tf_close() happens.
 *
 * A counter-only tail-jump trampoline cannot settle that. "tf_eos was called
 * 900 times" is equally consistent with it returning false forever AND with
 * it returning true while the managed side ignores the result. Only the
 * RETURN VALUE splits those, so these are call-and-return wrappers.
 *
 * Written in C and compiled, not hand-encoded: the ABI details (shadow space,
 * alignment, which registers survive the inner call) are exactly the kind of
 * thing that is easy to get subtly wrong by hand and expensive to debug on a
 * device.
 *
 * The loader fills tft_real[] with the original export addresses and rewrites
 * libtheorafile's export address table to point at these wrappers. Records go
 * into a plain ring buffer that native code reads; nothing here calls back
 * into the host.
 */
#include <stdint.h>

#define TFT_SLOTS 5
enum { TF_FOPEN = 0, TF_READVIDEO, TF_READAUDIO, TF_EOS, TF_CLOSE };

/* Filled in by the loader before any wrapper can run. */
__declspec(dllexport) void *tft_real[TFT_SLOTS];

/* Reporting goes out through OutputDebugStringA, which lands in the host log
 * directly. The alternative -- a ring buffer read by native code -- needs the
 * reader to learn this module's base, and that tracking lives in a third build
 * (the emulator), so it would have meant touching three build chains to print
 * five numbers. Bounded and state-change-only, so a per-frame decode call
 * cannot turn logging into the thing being measured. */
__declspec(dllimport) void __stdcall OutputDebugStringA(const char *);

static char *tft_hex(char *o, unsigned long long v)
{
    static const char *H = "0123456789abcdef";
    int i, lead = 0;
    *o++ = '0'; *o++ = 'x';
    for (i = 60; i >= 0; i -= 4)
    {
        int d = (int)((v >> i) & 0xf);
        if (d || lead || i == 0) { *o++ = H[d]; lead = 1; }
    }
    return o;
}

static char *tft_str(char *o, const char *s)
{
    while (*s) *o++ = *s++;
    return o;
}

/* Ring buffer retained so a native reader can be added later without changing
 * the wrappers. */
typedef struct {
    uint32_t idx;
    uint32_t reserved;
    uint64_t handle;
    uint64_t result;
    uint64_t seq;
} tft_rec;

__declspec(dllexport) volatile uint64_t tft_seq;
__declspec(dllexport) tft_rec tft_ring[256];
__declspec(dllexport) volatile uint64_t tft_calls[TFT_SLOTS];
/* LIVE state, deliberately outside the capped event log. The cap exists so a
 * per-frame decode call cannot drown the log, but it must never be able to
 * hide a late tf_eos 0->1 or a terminal read result -- so the last value of
 * every function is always current here, however many calls were capped. */
__declspec(dllexport) volatile uint64_t tft_last[TFT_SLOTS];
__declspec(dllexport) volatile uint64_t tft_last_handle[TFT_SLOTS];
__declspec(dllexport) volatile uint64_t tft_magic = 0x5446545241434531ULL; /* "TFTRACE1" */

/* Record first call, every state change, and every terminal result -- not
 * every call. tf_readvideo runs per frame; logging all of it would drown the
 * log and change the timing we are measuring. */
static void tft_emit(const char *tag, uint32_t idx, uint64_t handle, uint64_t result, uint64_t n)
{
    static const char * const nm[TFT_SLOTS] =
        { "tf_fopen", "tf_readvideo", "tf_readaudio", "tf_eos", "tf_close" };
    char buf[200], *o = buf;
    o = tft_str( o, "[tf-trace] ml735 " );
    o = tft_str( o, tag );
    o = tft_str( o, " " );
    o = tft_str( o, nm[idx] );
    o = tft_str( o, " call#" );   o = tft_hex( o, n );
    o = tft_str( o, " handle=" ); o = tft_hex( o, handle );
    o = tft_str( o, " -> " );     o = tft_hex( o, result );
    if (idx == TF_EOS && result) o = tft_str( o, "   <== EOS TRUE" );
    if (idx == TF_CLOSE)         o = tft_str( o, "   <== CLOSED" );
    *o++ = '\n'; *o = 0;
    OutputDebugStringA( buf );
}

static void tft_note(uint32_t idx, uint64_t handle, uint64_t result)
{
    static uint64_t seen[TFT_SLOTS];
    uint64_t prev = tft_last[idx];
    uint64_t n = ++tft_calls[idx];

    /* live state first, always */
    tft_last[idx] = result;
    tft_last_handle[idx] = handle;

    /* ml735: the previous build capped at 64 records per function and treated
     * every zero return as interesting. tf_readvideo returns 0 whenever no
     * frame is ready, which is most calls, so the budget was spent within
     * seconds and the log stopped at call #65 of many thousands -- exactly the
     * "a cap must never hide a late tf_eos 0->1" failure. Three rules now:
     *
     *   1. a TRANSITION (result != previous) is always reported, uncapped.
     *      This is the event we are hunting and it is rare by construction.
     *   2. the first call of each function is reported once.
     *   3. a periodic heartbeat proves the function is still being called and
     *      carries the live totals, so silence is never ambiguous.
     */
    if (result != prev)
    {
        tft_emit( "CHANGE", idx, handle, result, n );
        return;
    }
    if (n == 1) { tft_emit( "FIRST ", idx, handle, result, n ); return; }
    if (!(n % 4096)) { tft_emit( "ALIVE ", idx, handle, result, n ); return; }
    (void)seen;
}

typedef int  (*fn_fopen)(const char *, void **);
typedef int  (*fn_rv)(void *, void *, int);
typedef int  (*fn_ra)(void *, void *, int);
typedef int  (*fn_eos)(void *);
typedef void (*fn_close)(void **);

__declspec(dllexport) int tft_tf_fopen(const char *path, void **out)
{
    int r = ((fn_fopen)tft_real[TF_FOPEN])(path, out);
    tft_note(TF_FOPEN, (uint64_t)(uintptr_t)(out ? *out : 0), (uint64_t)(uint32_t)r);
    return r;
}

__declspec(dllexport) int tft_tf_readvideo(void *f, void *buf, int n)
{
    int r = ((fn_rv)tft_real[TF_READVIDEO])(f, buf, n);
    tft_note(TF_READVIDEO, (uint64_t)(uintptr_t)f, (uint64_t)(uint32_t)r);
    return r;
}

__declspec(dllexport) int tft_tf_readaudio(void *f, void *buf, int n)
{
    int r = ((fn_ra)tft_real[TF_READAUDIO])(f, buf, n);
    tft_note(TF_READAUDIO, (uint64_t)(uintptr_t)f, (uint64_t)(uint32_t)r);
    return r;
}

__declspec(dllexport) int tft_tf_eos(void *f)
{
    int r = ((fn_eos)tft_real[TF_EOS])(f);
    tft_note(TF_EOS, (uint64_t)(uintptr_t)f, (uint64_t)(uint32_t)r);
    return r;
}

__declspec(dllexport) void tft_tf_close(void **f)
{
    tft_note(TF_CLOSE, (uint64_t)(uintptr_t)(f ? *f : 0), 0);
    ((fn_close)tft_real[TF_CLOSE])(f);
}

int __stdcall DllMainCRTStartup(void *h, unsigned r, void *x) { (void)h;(void)r;(void)x; return 1; }
