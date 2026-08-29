/* Remote Metal guest spike -- runs natively on the iOS guest, no FEX involved.
 *
 * Answers the questions that must be settled before committing weeks to a
 * remote Metal backend: does the round trip work at all from inside the VM,
 * what does a call cost, and does the generation-tagged handle table actually
 * reject a stale handle instead of quietly addressing a recycled object.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include "../protocol.h"

static int g_fd;
static uint32_t g_seq;

static int rd(int fd, void *p, size_t n) {
    uint8_t *b = p;
    while (n) { ssize_t r = read(fd, b, n); if (r <= 0) return -1; b += r; n -= (size_t)r; }
    return 0;
}
static int wr(int fd, const void *p, size_t n) {
    const uint8_t *b = p;
    while (n) { ssize_t r = write(fd, b, n); if (r <= 0) return -1; b += r; n -= (size_t)r; }
    return 0;
}

/* One synchronous call. Returns the status; response payload copied to out. */
static uint32_t call(uint16_t op, const void *arg, uint32_t arglen,
                     void *out, uint32_t outcap, uint32_t *outlen) {
    struct rm_hdr h = { RM_MAGIC, RM_VERSION, op, ++g_seq, 0, arglen, 0 };
    if (wr(g_fd, &h, sizeof h)) return 0xffffffff;
    if (arglen && wr(g_fd, arg, arglen)) return 0xffffffff;
    struct rm_hdr r;
    if (rd(g_fd, &r, sizeof r)) return 0xffffffff;
    if (r.magic != RM_MAGIC || r.seq != h.seq) {
        fprintf(stderr, "  PROTOCOL DESYNC: magic=%08x seq=%u expected %u\n", r.magic, r.seq, h.seq);
        return 0xffffffff;
    }
    uint8_t scratch[65536];
    uint32_t n = r.payload_len;
    if (n) { if (rd(g_fd, scratch, n)) return 0xffffffff; }
    if (out && n) { uint32_t c = n < outcap ? n : outcap; memcpy(out, scratch, c); }
    if (outlen) *outlen = n;
    return r.status;
}

static double now_ms(void) {
    struct timeval t; gettimeofday(&t, NULL);
    return t.tv_sec * 1000.0 + t.tv_usec / 1000.0;
}

static const char *statname(uint32_t s) {
    switch (s) {
    case RM_OK: return "OK";
    case RM_ERR_STALE_HANDLE: return "STALE_HANDLE";
    case RM_ERR_BAD_HANDLE: return "BAD_HANDLE";
    case RM_ERR_WRONG_CLASS: return "WRONG_CLASS";
    default: return "err";
    }
}

int main(int argc, char **argv) {
    const char *host = argc > 1 ? argv[1] : "10.0.1.53";
    g_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(RM_PORT) };
    inet_pton(AF_INET, host, &a.sin_addr);
    if (connect(g_fd, (struct sockaddr *)&a, sizeof a)) { perror("connect"); return 1; }
    int one = 1; setsockopt(g_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    printf("[rmtest] connected to %s:%d\n\n", host, RM_PORT);

    /* --- device enumeration through the handle table --- */
    struct rm_ret_handle rh; uint32_t st;
    st = call(RM_OP_COPY_ALL_DEVICES, NULL, 0, &rh, sizeof rh, NULL);
    printf("  CopyAllDevices      -> %s handle=%llu (gen=%u slot=%u)\n",
           statname(st), (unsigned long long)rh.handle,
           RM_HANDLE_GEN(rh.handle), RM_HANDLE_SLOT(rh.handle));
    uint64_t arr = rh.handle;

    struct rm_ret_u64 ru; struct rm_arg_handle ah = { arr };
    call(RM_OP_ARRAY_COUNT, &ah, sizeof ah, &ru, sizeof ru, NULL);
    printf("  ArrayCount          -> %llu\n", (unsigned long long)ru.value);

    struct rm_arg_handle_u64 a2 = { arr, 0 };
    call(RM_OP_ARRAY_OBJECT, &a2, sizeof a2, &rh, sizeof rh, NULL);
    uint64_t dev = rh.handle;
    printf("  ArrayObject[0]      -> handle=%llu\n", (unsigned long long)dev);

    char name[256] = {0}; uint32_t nl = 0;
    struct rm_arg_handle ad = { dev };
    call(RM_OP_DEVICE_NAME, &ad, sizeof ad, name, sizeof name - 1, &nl);
    name[nl < sizeof name ? nl : sizeof name - 1] = 0;
    printf("  DeviceName          -> \"%s\"\n", name);

    /* the capabilities the paravirtual device could not provide */
    for (int fam = 1001; fam <= 1009; fam += 2) {
        struct rm_arg_handle_u64 af = { dev, (uint64_t)fam };
        call(RM_OP_SUPPORTS_FAMILY, &af, sizeof af, &ru, sizeof ru, NULL);
        printf("  supportsFamily(%d) -> %llu\n", fam, (unsigned long long)ru.value);
    }
    call(RM_OP_SUPPORTS_BC, &ad, sizeof ad, &ru, sizeof ru, NULL);
    printf("  supportsBC          -> %llu\n", (unsigned long long)ru.value);
    call(RM_OP_ALLOCATED_SIZE, &ad, sizeof ad, &ru, sizeof ru, NULL);
    printf("  allocatedSize       -> %llu\n", (unsigned long long)ru.value);

    /* --- stale handle must be REJECTED, not silently reused --- */
    printf("\n  [handle lifetime]\n");
    struct rm_arg_handle rel = { dev };
    st = call(RM_OP_RELEASE, &rel, sizeof rel, NULL, 0, NULL);
    printf("    release(dev)      -> %s\n", statname(st));
    st = call(RM_OP_ALLOCATED_SIZE, &ad, sizeof ad, &ru, sizeof ru, NULL);
    printf("    use after release -> %s  %s\n", statname(st),
           st == RM_ERR_BAD_HANDLE || st == RM_ERR_STALE_HANDLE ? "(correctly rejected)" : "*** LEAKED ***");
    /* re-intern something so the slot is recycled, then retry the OLD handle */
    call(RM_OP_COPY_ALL_DEVICES, NULL, 0, &rh, sizeof rh, NULL);
    st = call(RM_OP_ALLOCATED_SIZE, &ad, sizeof ad, &ru, sizeof ru, NULL);
    printf("    stale after reuse -> %s  %s\n", statname(st),
           st == RM_ERR_STALE_HANDLE || st == RM_ERR_BAD_HANDLE || st == RM_ERR_WRONG_CLASS
               ? "(correctly rejected)" : "*** ADDRESSED A DIFFERENT OBJECT ***");

    /* --- latency --- */
    printf("\n  [latency, %d calls each]\n", 2000);
    double t0 = now_ms();
    for (int i = 0; i < 2000; i++) call(RM_OP_PING, NULL, 0, NULL, 0, NULL);
    double ping = (now_ms() - t0) / 2000.0;
    struct rm_arg_handle a3 = { arr };
    t0 = now_ms();
    for (int i = 0; i < 2000; i++) call(RM_OP_ARRAY_COUNT, &a3, sizeof a3, &ru, sizeof ru, NULL);
    double work = (now_ms() - t0) / 2000.0;
    printf("    ping (no Metal)   %.4f ms  -> %.0f calls/sec\n", ping, 1000.0 / ping);
    printf("    ArrayCount        %.4f ms  -> %.0f calls/sec\n", work, 1000.0 / work);

    call(RM_OP_STATS, NULL, 0, &ru, sizeof ru, NULL);
    printf("\n  live handles on host: %llu\n", (unsigned long long)ru.value);
    close(g_fd);
    return 0;
}
