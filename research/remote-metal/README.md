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

## Next

Offscreen triangle returning a pixel checksum — isolates pipeline creation,
buffers, command serialisation, GPU execution and readback without dragging in
windowing. Then the host window and drawable presentation.
