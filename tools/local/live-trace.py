#!/usr/bin/env python3
"""Reports on a live `trace` / `tracew` capture (src/local/live-trace.h).

    live-trace.py frames TRACE [--detail N]      per-frame GPU busy, render idle, GPU idle gaps
    live-trace.py frame TRACE INDEX [--span MS]  one frame as an event timeline
    live-trace.py producers TRACE [--frames A:B] async guest readbacks and their GPU producers
                                                 (needs `tracew`)
    live-trace.py gpu TRACE INDEX [--span MS]    GPU command buffers of one frame: when they ran,
                                                 when the render thread submitted them, from which
                                                 guest queue slice

The TSC rate is read from TRACE.hz when present (the LIVE_TRACE line prints it), else --hz.
Memory stays bounded: ranges are matched with sorted intervals, never expanded per page.
"""
import argparse
import collections
import resource
import statistics
import struct
from pathlib import Path

SUBMIT, GUEST_DONE, EQUEUE_WAIT, FLIP_SUBMIT, FLIP_COMPLETE, FRONT_SUSPEND = 1, 2, 7, 8, 9, 10
GPU_SUBMIT, GPU_DONE, GUEST_READBACK, RENDER_IDLE, READBACK_TICKS = 11, 12, 13, 14, 19
TICK_DONE, RENDER_SLICE, GPU_SPAN, GPU_SPAN_NS, GPU_WRITE, SYNC_READBACK = 20, 21, 22, 23, 26, 28


class Trace:
    def __init__(self, path, hz):
        data = Path(path).read_bytes()
        hz_file = Path(str(path) + '.hz')
        self.hz = float(hz_file.read_text()) if hz_file.exists() else hz
        records = sorted(struct.iter_unpack('<QIIQQ', data))
        self.t0 = records[0][0]
        self.records = records
        self.done = {}
        spans, last = {}, {}
        for tsc, tid, kind, a, b in records:
            if kind == TICK_DONE:
                self.done.setdefault(a, self.ms(tsc))
            elif kind == GPU_SPAN:
                last[tid] = a
            elif kind == GPU_SPAN_NS and tid in last:
                spans[last.pop(tid)] = (a, b)
        # GPU clock -> TSC: the completion a monitor sees is never earlier than the GPU end.
        offsets = [self.done[t] - e / 1e6 for t, (bg, e) in spans.items() if t in self.done]
        offset = min(offsets) if offsets else 0.0
        self.gpu = sorted((bg / 1e6 + offset, e / 1e6 + offset, t) for t, (bg, e) in spans.items())
        self.render = collections.Counter(r[1] for r in records if r[2] == RENDER_SLICE).most_common(1)[0][0]
        self.flips = [self.ms(r[0]) for r in records if r[2] == FLIP_SUBMIT and r[1] == self.render]
        self.render_idle = []
        start = None
        for tsc, tid, kind, a, b in records:
            if tid == self.render and kind == RENDER_IDLE:
                if a == 1:
                    start = (self.ms(tsc), b)
                elif start is not None:
                    self.render_idle.append((start[0], self.ms(tsc), start[1]))
                    start = None
        self.readback_waits = []  # (begin, end, tid, address)
        open_waits = {}
        for tsc, tid, kind, a, b in records:
            if kind == GUEST_READBACK:
                if a == 1:
                    open_waits[tid] = (self.ms(tsc), b)
                elif tid in open_waits:
                    begin, address = open_waits.pop(tid)
                    self.readback_waits.append((begin, self.ms(tsc), tid, address))

    def ms(self, tsc):
        return (tsc - self.t0) / self.hz * 1e3


def union(intervals, lo, hi):
    total, current = 0.0, None
    for s, e in sorted(intervals):
        s, e = max(s, lo), min(e, hi)
        if e <= s:
            continue
        if current is None or s > current[1]:
            if current:
                total += current[1] - current[0]
            current = [s, e]
        else:
            current[1] = max(current[1], e)
    if current:
        total += current[1] - current[0]
    return total


def merged(intervals, lo, hi, join=0.02):
    out = []
    for s, e in sorted(intervals):
        if e < lo or s > hi:
            continue
        if out and s <= out[-1][1] + join:
            out[-1][1] = max(out[-1][1], e)
        else:
            out.append([s, e])
    return out


def cmd_frames(t, args):
    gpu = [(s, e) for s, e, _ in t.gpu]
    idle = [(s, e) for s, e, _ in t.render_idle]
    frames, busy, ridle = [], [], []
    for i in range(1, len(t.flips) - 1):
        lo, hi = t.flips[i], t.flips[i + 1]
        frames.append(hi - lo)
        busy.append(union(gpu, lo, hi))
        ridle.append(union(idle, lo, hi))
        if i <= args.detail:
            print(f'frame {i}: {hi - lo:6.2f} ms  GPU busy {busy[-1]:6.2f}  render idle {ridle[-1]:5.2f}')
            cursor = lo
            for s, e in merged(gpu, lo, hi):
                if s - cursor > 0.4:
                    print(f'   GPU idle {cursor - lo:6.2f}-{s - lo:6.2f} ({s - cursor:5.2f} ms), render idle '
                          f'{union(idle, cursor, s):5.2f}')
                cursor = max(cursor, e)
    print(f'{len(frames)} frames: frame {statistics.mean(frames):.2f} ms, GPU busy {statistics.mean(busy):.2f} ms, '
          f'render idle {statistics.mean(ridle):.2f} ms')


def cmd_frame(t, args):
    lo = t.flips[args.index]
    hi = lo + args.span
    events = []
    for s, e, blocked in t.render_idle:
        if s < hi and e > lo and e - s > 0.05:
            events.append((s, f'RENDER idle {e - s:.2f} ms ({"blocked" if blocked else "empty"})'))
    for tsc, tid, kind, a, b in t.records:
        x = t.ms(tsc)
        if not lo <= x < hi:
            continue
        if kind == SUBMIT:
            events.append((x, f'guest submit tid {tid} q{a & 0xff} type{(a >> 8) & 0xff} {b} dw'))
        elif kind == FRONT_SUSPEND and tid == t.render:
            events.append((x, f'render WAIT_REG_MEM {a:#x} == {b:#x}'))
        elif kind == FLIP_SUBMIT and tid == t.render:
            events.append((x, 'FLIP submit (render)'))
    for s, e, tid, address in t.readback_waits:
        if s < hi and e > lo and e - s > 0.05:
            events.append((s, f'guest {tid} readback wait {e - s:.2f} ms {address:#x}'))
    for s, e in merged([(s, e) for s, e, _ in t.gpu], lo, hi):
        if e - s > 0.1:
            events.append((s, f'GPU busy {e - s:.2f} ms (until {e - lo:.2f})'))
    for x, text in sorted(events):
        print(f'{x - lo:7.2f}  {text}')


def cmd_producers(t, args):
    import numpy as np
    reads = [(t.ms(tsc), a, b & 0xffffffff, b >> 32, 'sync' if kind == SYNC_READBACK else 'async')
             for tsc, tid, kind, a, b in t.records if kind in (READBACK_TICKS, SYNC_READBACK)]
    writes = [(tsc, a, b) for tsc, tid, kind, a, b in t.records if kind == GPU_WRITE]
    w_time = np.array([(tsc - t.t0) / t.hz * 1e3 for tsc, _, _ in writes])
    w_lo = np.array([a for _, a, _ in writes], dtype=np.uint64)
    w_hi = w_lo + np.array([b & 0xffffffff for _, _, b in writes], dtype=np.uint64)
    w_tick = np.array([b >> 32 for _, _, b in writes], dtype=np.uint64)
    # Per distinct read range: the writes that overlap it (in time order), then per read the last
    # of them recorded before the read.
    overlapping = {}
    producer = []
    for x, a, size, _, _ in reads:
        key = (a, size)
        if key not in overlapping:
            overlapping[key] = np.nonzero((w_lo < np.uint64(a + size)) & (np.uint64(a) < w_hi))[0]
        hits = overlapping[key]
        before = np.searchsorted(w_time[hits], x) - 1
        producer.append((float(w_time[hits[before]]), int(w_tick[hits[before]])) if before >= 0 else None)
    ready = sum(1 for (x, _, _, _, _), p in zip(reads, producer) if p and t.done.get(p[1], 1e18) <= x)
    print(f'{len(reads)} readbacks, {sum(1 for p in producer if p)} with a traced producer, '
          f'{ready} of them complete at request time; {len(t.flips)} flips')
    first_frame, last_frame = (int(v) for v in args.frames.split(':'))
    for k in range(first_frame, min(last_frame, len(t.flips) - 1)):
        lo, hi = t.flips[k], t.flips[k + 1]
        print(f'\nframe {k}: {hi - lo:.2f} ms')
        for (x, a, size, tick, tid), p in zip(reads, producer):
            if not lo <= x < hi:
                continue
            copy_done = t.done.get(tick, float('nan')) - lo
            if p:
                done = t.done.get(p[1], float('nan')) - lo
                text = f'producer tick {p[1]} recorded {p[0] - lo:7.2f} done {done:7.2f} ({tick - p[1]} ticks older)'
            else:
                text = 'producer not traced'
            print(f'  {tid:5s} {x - lo:6.2f} {a:#x}+{size:#x} copy tick {tick} done {copy_done:6.2f}  {text}')


def cmd_gpu(t, args):
    lo = t.flips[args.index]
    hi = lo + args.span
    submitted, queue = {}, None
    for tsc, tid, kind, a, b in t.records:
        if tid != t.render:
            continue
        if kind == RENDER_SLICE:
            queue = f'q{a & 0xff}' if (a >> 17) & 1 else None
        elif kind == GPU_SUBMIT and b == 0:
            submitted[a] = (t.ms(tsc), queue)
    busy_by_queue = collections.Counter()
    for s, e, tick in t.gpu:
        if e < lo or s > hi:
            continue
        at, q = submitted.get(tick, (None, None))
        busy_by_queue[q] += min(e, hi) - max(s, lo)
        if e - s >= args.min:
            print(f'GPU {s - lo:7.2f}-{e - lo:7.2f} ({e - s:5.2f} ms) tick {tick} from {q} submitted '
                  f'{"?" if at is None else f"{at - lo:.2f}"}')
    print('busy by submitting slice:', {k: round(v, 2) for k, v in busy_by_queue.most_common()})


def main():
    # A runaway report must never take the machine down with the game running.
    resource.setrlimit(resource.RLIMIT_AS, (16 << 30, 16 << 30))
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--hz', type=float, default=3187145000.0)
    sub = parser.add_subparsers(dest='action', required=True)
    p = sub.add_parser('frames')
    p.add_argument('trace')
    p.add_argument('--detail', type=int, default=3)
    p.set_defaults(func=cmd_frames)
    p = sub.add_parser('frame')
    p.add_argument('trace')
    p.add_argument('index', type=int)
    p.add_argument('--span', type=float, default=34)
    p.set_defaults(func=cmd_frame)
    p = sub.add_parser('gpu')
    p.add_argument('trace')
    p.add_argument('index', type=int)
    p.add_argument('--span', type=float, default=34)
    p.add_argument('--min', type=float, default=0.1)
    p.set_defaults(func=cmd_gpu)
    p = sub.add_parser('producers')
    p.add_argument('trace')
    p.add_argument('--frames', default='5:7')
    p.set_defaults(func=cmd_producers)
    args = parser.parse_args()
    args.func(Trace(args.trace, args.hz), args)


if __name__ == '__main__':
    main()
