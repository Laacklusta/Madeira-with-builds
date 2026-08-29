# Remote Metal transport (spike)

Forwards Metal from the virtualised iOS guest to a real GPU on the macOS host,
so the VM stops being limited by its paravirtual Metal device.

## Why

`AppleParavirtDevice` reports **no GPU family at all** (`Apple7=0 Apple8=0
Apple9=0 Mac2=0`), has **no BC texture compression**, and implements **no mesh
pipeline or encoder** — the APV transport carries no mesh packets, so
`setObjectBuffer:` reaches Metal's abstract selector and aborts the process.
Forwarding to the host removes APV from the GPU path entirely.

Measured from inside the guest against an M4 Max host:

```
DeviceName            "Apple M4 Max"
supportsFamily 1001..1009   all 1     (paravirtual device: all 0)
supportsBC                  1         (paravirtual device: 0)
ping (no Metal)       0.0606 ms   16,514 calls/sec
ArrayCount            0.0643 ms   15,551 calls/sec
```

## Design notes

winemetal is an excellent **semantic seam** — 117 flat C functions — but it is
**not a wire protocol**, and two of its properties are only legal because guest
and host currently share one address space:

1. `obj_handle_t` is a raw Objective-C pointer **cast** to `uint64_t`
   (`params->ret = (obj_handle_t)[array objectAtIndex:i]`). Meaningless across
   a machine boundary. This protocol never puts a host pointer on the wire:
   handles are **generation-tagged table indices**, so a stale handle is
   rejected rather than silently addressing a recycled object. Verified —
   `use after release -> BAD_HANDLE`, `stale after reuse -> STALE_HANDLE`.
2. `wmtcmd_*` command lists are **linked through guest pointers** and carry
   further pointers for inline bytes, viewports and scissors. Those must become
   contiguous records with offsets before command encoding can be forwarded.
   **This spike deliberately stops short of that.**

Synchronous request/response by design. Async completion handlers across a
machine boundary invite non-deterministic hangs; correctness first.

## Layout

    protocol.h      framed wire protocol, shared
    host/rmetald.m  macOS daemon, owns the real MTLDevice and the handle table
    guest/rmtest.c  native ARM64 guest client (no FEX dependency)

    clang -fobjc-arc -O2 -o host/rmetald host/rmetald.m -framework Foundation -framework Metal
    xcrun -sdk iphoneos clang -arch arm64 -O2 -o guest/rmtest guest/rmtest.c

## Offscreen render milestone — done

```
submitRenderPass  OK   4 encoder cmds in ONE round trip, 6.76 ms
getBytes          OK   16384 bytes
fnv1a checksum         0x31d709c5
non-black pixels       1352 / 4096  (33.0%)
```

Coverage is geometrically exact: vertices (0,0.8) (-0.8,-0.8) (0.8,-0.8) give
area 1.28 against a clip-space area of 4, i.e. 32%. That is a real
rasterisation result rather than "some pixels are lit". Shaders are compiled
from source on the host; buffer, texture, library, function, pipeline, encode,
commit, wait and readback all cross the boundary.

## Measured scale

```
upload    1 MB    0.7 ms   1357 MB/s     download 512x512  (1 MB)  1324 MB/s
upload    4 MB    1.6 ms   2522 MB/s     download 1024x1024(4 MB)  2296 MB/s
upload   16 MB    3.6 ms   4419 MB/s
upload   64 MB   12.5 ms   5119 MB/s

render pass, 30 samples each:
       1 draws (   80 B)  median 0.23 ms   p95 0.52 ms
      10 draws (  368 B)  median 0.20 ms   p95 0.50 ms
     100 draws ( 3248 B)  median 0.22 ms   p95 0.50 ms
    1000 draws (32048 B)  median 0.28 ms   p95 0.31 ms
```

**A thousand draws costs the same as one** — 0.23 to 0.28 ms. Command count is
effectively free; the fixed cost is the round trip plus `waitUntilCompleted`.
That is what makes the batched design viable.

⚠️ An earlier single sample read 6.76 ms and was a *cold* first submit
including shader and pipeline warmup. One sample proves nothing; these are
medians and p95 over 30 passes.

Per-call latency is 0.06 ms, but that alone never justified "transport is not
the bottleneck" — bulk transfer and command decoding had to be measured
separately, and are above.

## Presentation — done

2000 frames from the guest into a real macOS window on the host GPU:

```
presented      2000/2000 frames
no-drawable    0        (answered with a status, never hung)
frame submit   mean 0.91 ms   worst 15.68 ms
live handles   11       (unchanged from setup: no per-frame leak)
drawable reuse BAD_HANDLE (consumed by submit, as intended)
```

Design points that matter:

- **AppKit owns the main thread**, RPC runs on a worker. Window creation happens
  directly on main -- `dispatch_sync` to the main queue *from* main, before
  `[NSApp run]`, is an immediate self-deadlock. `dispatch_sync` is used only
  from the RPC thread, which is where it is correct.
- **`presentDrawable` is encoded on the same command buffer as the draw, before
  commit.** Presenting from a separate call after that buffer had committed
  would race the display.
- `allowsNextDrawableTimeout = YES`, and a timeout returns `RM_ERR_NO_DRAWABLE`
  — an answer, not a hung RPC thread.
- A drawable is single-use, so submit consumes **both** its handles. Releasing
  only the drawable leaked one texture handle per frame, which a 600-frame run
  made obvious at 611 live handles.

⚠️ Resize is implemented (the layer is resized to the window each acquisition)
but **not exercised** — the automated run never resizes, so `resizes observed`
was 0. Minimise/close/reopen are likewise untested.

## Next

Convert winemetal's real `wmtcmd_*` lists into validated contiguous records,
then point winemetal at this backend and drive it with the ARM64 D3D11 cube.
