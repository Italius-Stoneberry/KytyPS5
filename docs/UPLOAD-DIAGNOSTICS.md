# Upload and staging diagnostics

This patch adds opt-in diagnostics only. It does not change dirty tracking, uploads,
GPU waits, memory budgets, or default settings.

Set these before starting the emulator:

```
KYTY_STAGING_DIAGNOSTICS=1
KYTY_IMAGE_UPLOAD_DIAGNOSTICS=1
```

`STAGING MAP` reports waits of at least 10 ms around ring reservations, including
requested/normalized size, capacity, wrap, usage and watch ticks. Ordinary lines
are limited to one per second per thread; stalls of 100 ms or longer are always
reported. This duration covers the existing pending-operation wait path, not GPU
execution time. When disabled, this diagnostic performs no clock reads.

`IMAGE-UPLOAD-DIAG` reports images of at least 16 MiB: first observed whole-dirty
invalidations, full upload decisions and successful partial uploads. Serial plus
address distinguish image incarnations. `reason` describes the upload decision;
`eligibility` describes current partial-invalidation eligibility, which may already
reflect a prior invalidation. A partial line's `bytes` is the full image size, not
the actual transferred bytes; use existing partial-upload counters for transfer
volume. The global cap is 32 lines per second, so absence of a line is not proof
that an event did not happen.

Optionally restrict image diagnostics to one exact guest image base address:

```
KYTY_IMAGE_UPLOAD_DIAGNOSTICS_ADDR=0x102a400000
```

Addresses may change between sessions. First capture without a filter. Both log
families include the same TSC clock as existing `SLOW` lines and flush on output.
Logging can affect performance; compare throughput with diagnostics disabled.

For an initial capture, warm the same save/scene, collect a 60-second stationary
window, then a short repeatable traversal. Use `KYTY_LIVE_FILE` / `measure` to
record whole-window flip statistics and counters. `SLOW Frame` entries alone are
threshold-selected and cannot establish average FPS. Keep shader-cache state and
all launch settings identical between comparisons.

Validation requires a Windows build and gameplay. No claim of improved FPS is
made by this instrumentation patch.
