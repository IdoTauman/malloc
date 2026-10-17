# A Thread-Caching malloc in C

A from-scratch memory allocator for Linux/x86-64, built as a shared library. It provides `malloc`, `calloc`, `realloc` and `free`, using size-segregated free lists, batched refills and per-thread caches to cut down on locking.

I built it to see how allocator design affects throughput, scaling and memory use. On a multithreaded benchmark where each thread works on its own objects, it reaches 2.7 to 3.1 times the throughput of glibc `malloc`. Other benchmarks show what that costs: it does badly on large allocations, on workloads that change size distribution over time, and on `realloc` growth.

This README covers the implementation, how I got there, the benchmark setup and results, and the known limitations, including the workloads where glibc is much better.

> Status: educational project. It is not a replacement for glibc `malloc`. See [Limitations](#limitations).

## Contents

1. [Performance at a glance](#performance-at-a-glance)
2. [Design](#design)
3. [Build, test, benchmark](#build-test-benchmark)
4. [How the design evolved](#how-the-design-evolved)
5. [Benchmark 1: thread-private workload](#benchmark-1-thread-private-workload)
6. [Benchmark 2: adversarial workloads](#benchmark-2-adversarial-workloads)
7. [Analysis](#analysis)
8. [Correctness testing](#correctness-testing)
9. [Limitations](#limitations)
10. [Roadmap](#roadmap)
11. [AI assistance](#ai-assistance)

## Performance at a glance

| Workload | Compared to glibc |
|---|---|
| Random alloc/free, sizes up to 8 KB, memory private to each thread | 2.7x to 3.1x higher throughput at 1, 2, 4 and 8 threads |
| Producer/consumer, all frees on a different thread, small blocks | 2.7x to 4.6x faster |
| 20k live objects of 4100 B | 2x faster, 2x the memory |
| 2M live objects of 16 B | 1.7x slower, 2x the memory |
| Fill 32 MB with one size, free it, repeat for 9 sizes | 3.5x slower, 7x the peak memory |
| Large allocations (128 KB to 1 MB) | about 100x to 470x slower |
| `realloc` growing a buffer in 4 KB steps to 4 MB | about 1000x slower |
| Thread churn (many short-lived threads) | even with 1 thread, 1.4x slower with 8 |

The short version: small-object allocation is a pop from a thread-local list, which is cheap. In exchange, freed memory is never merged or reused across size classes, small objects carry a large header, and large blocks go straight to `mmap`.

## Design

### Size classes

| Range | Spacing |
|---|---|
| 16 B to 512 B | 16 B steps (32 classes) |
| 1 KiB and up | powers of two |

A request is rounded up to its class size, so every block in a class has the same size. Payloads are 16-byte aligned.

### Shared state

Each class has a free list protected by its own spinlock. When a list is empty, the allocator maps a chunk (`MMAP_CHUNK_SIZE`), cuts it into fixed-size blocks and pushes them onto the list.

### Thread caches

Each thread has a private free list per class. The cache itself is allocated with `mmap`, so setting it up never calls `malloc`.

- `malloc` pops from the thread's list. No lock, no atomics.
- `free` pushes onto the freeing thread's list. A block freed by a different thread than the one that allocated it just moves to that thread's cache, so no owner pointers are needed.
- On a miss, a thread takes a batch of `CACHE_BATCH_SIZE` (16) blocks from the shared list under one lock. When a list grows past `CACHE_MAX_BLOCKS` (32), a batch goes back.
- A `pthread_key` destructor returns a thread's cached blocks to the shared lists when the thread exits.

### Large allocations

If a request's class is at least `MMAP_CHUNK_SIZE / 2`, it gets its own `mmap`, and `free` calls `munmap` right away.

### Other details

- `realloc` keeps the pointer if the block's capacity already fits. Otherwise it allocates, copies and frees.
- `calloc` is `malloc` plus `memset`, since blocks get recycled.
- An `is_free` flag catches ordinary double frees.
- The public functions are thin wrappers around static `*_impl` functions. This matters because an earlier `calloc` called `malloc` and then `memset`, and GCC at `-O2` turned that pair into a call to `calloc`, which is itself, so every small `calloc` looped forever. The library now builds with `-fno-builtin-malloc`, and the `_impl` split keeps the pattern out of the source.

## Build, test, benchmark

```sh
make                      # build/liballocator.so
make test                 # tests against the shared library
make benchmark            # everything in benchmarks/
make tsan SAN_EXTRA=-DRANDOM_ITERATIONS=50000
make asan SAN_EXTRA=-DRANDOM_ITERATIONS=50000
```

The sanitizer targets compile `src/*.c` directly into each test binary and rename the entry points with `-Dmalloc=my_malloc` (same for `calloc`, `realloc`, `free`). Without this, the sanitizer's own allocator wins symbol resolution and the tests never touch this code.

If TSan aborts at startup with `unexpected memory mapping`, the cause is an incompatibility between the TSan runtime and the kernel's mmap randomization range. Running the binary under `setarch $(uname -m) -R` or lowering `vm.mmap_rnd_bits` gets around it.

For the glibc baseline I build the same benchmark source without `-lallocator`:

```sh
gcc -std=c11 -O2 -pthread -D_DEFAULT_SOURCE benchmarks/bench_adversarial.c -o build/bench_adv_glibc
```

## How the design evolved

I changed the design three times, each time based on the previous numbers.

### Version 1: one global list, first fit, hand-over-hand locking

Every `malloc` walked a single list of free and allocated blocks, locking each block in turn. `free` never merged neighbors, so the list only grew.

| Threads | Runtime (s) | Operations | Ops/sec | Speedup | Efficiency |
|---|---|---|---|---|---|
| 1 | 0.5653 | 81,575 | 144,311 | 1.00x | 100.0% |
| 2 | 6.2275 | 163,072 | 26,186 | 0.18x | 9.1% |
| 4 | 16.9138 | 325,879 | 19,267 | 0.13x | 3.3% |
| 8 | 48.4716 | 651,826 | 13,448 | 0.09x | 1.2% |

These were short runs, about 81k operations per thread. More threads made the run take longer in absolute terms. The causes were two atomic operations per block visited, a list that included every live allocation, and threads queuing behind each other in the locking chain.

### Version 2: size classes, one lock per class

Free lists per class turned allocation into a pop and free into a push.

| Threads | Runtime (s) | Operations | Ops/sec | Speedup | Efficiency |
|---|---|---|---|---|---|
| 1 | 0.0022 | 81,575 | 36,444,695 | 1.00x | 100.0% |
| 2 | 0.0072 | 163,072 | 22,513,107 | 0.62x | 30.9% |
| 4 | 0.0198 | 325,879 | 16,419,210 | 0.45x | 11.3% |
| 8 | 0.0718 | 651,826 | 9,082,290 | 0.25x | 3.1% |

Single-thread throughput went up about 250x (144k to 36.4M ops/sec), but adding threads still made things worse, since every thread takes the same few class locks.

Padding each class struct to a cache line barely helped (8 threads went from 0.25x to 0.28x), so false sharing was not the main problem:

| Threads | Runtime (s) | Ops/sec | Speedup |
|---|---|---|---|
| 1 | 0.0022 | 36,918,046 | 1.00x |
| 2 | 0.0058 | 28,197,820 | 0.76x |
| 4 | 0.0164 | 19,914,383 | 0.54x |
| 8 | 0.0632 | 10,320,857 | 0.28x |

I then made the run 1000x longer (about 81M operations per thread) to rule out thread startup and page faults as the cause. The contention was real:

| Threads | Runtime (s) | Operations | Ops/sec | Speedup | Efficiency |
|---|---|---|---|---|---|
| 1 | 1.4573 | 81,478,657 | 55,909,959 | 1.00x | 100.0% |
| 2 | 4.4484 | 162,960,515 | 36,633,252 | 0.66x | 32.8% |
| 4 | 17.0070 | 325,933,913 | 19,164,721 | 0.34x | 8.6% |
| 8 | 80.3087 | 651,859,977 | 8,116,929 | 0.15x | 1.8% |

### Version 3 (current): size classes plus thread caches

Described in [Design](#design). Results are in the next section.

## Benchmark 1: thread-private workload

Each thread runs a randomized mix of `malloc`, `calloc`, `realloc` and `free` on memory it owns, with sizes up to 8 KB, and checks payload contents on every operation. Work grows with the thread count (weak scaling), so a perfect result is a flat runtime. Each thread does about 81.5M operations. "Speedup" is throughput relative to the same allocator at 1 thread.

This allocator:

| Threads | Runtime (s) | Operations | Ops/sec | Speedup | Efficiency |
|---|---|---|---|---|---|
| 1 | 1.6183 | 81,478,657 | 50,347,704 | 1.00x | 100.0% |
| 2 | 2.2576 | 162,960,515 | 72,182,341 | 1.43x | 71.7% |
| 4 | 3.4508 | 325,933,913 | 94,451,364 | 1.88x | 46.9% |
| 8 | 5.1454 | 651,859,977 | 126,687,795 | 2.52x | 31.5% |

glibc (same benchmark, not linked against this library):

| Threads | Runtime (s) | Operations | Ops/sec | Speedup | Efficiency |
|---|---|---|---|---|---|
| 1 | 4.9513 | 81,478,657 | 16,455,854 | 1.00x | 100.0% |
| 2 | 6.4192 | 162,960,515 | 25,386,394 | 1.54x | 77.1% |
| 4 | 9.6933 | 325,933,913 | 33,624,563 | 2.04x | 51.1% |
| 8 | 13.6768 | 651,859,977 | 47,661,690 | 2.90x | 36.2% |

Side by side:

| Threads | Mine (s) | glibc (s) | Throughput ratio |
|---|---|---|---|
| 1 | 1.62 | 4.95 | 3.06x |
| 2 | 2.26 | 6.42 | 2.84x |
| 4 | 3.45 | 9.69 | 2.81x |
| 8 | 5.15 | 13.68 | 2.66x |

## Benchmark 2: adversarial workloads

`benchmarks/bench_adversarial.c` targets the weak points of this design. Each scenario runs in a forked child process, so peak RSS (`ru_maxrss`, minus the child's starting RSS) is measured per scenario. "RSS / requested" divides peak RSS by the bytes the scenario asked for while holding them live (using decimal MB, so it reads very slightly high).

Scenarios:

- `large_alloc`: 128 KB to 1 MB allocations, touched at both ends and freed, 20,000 per thread.
- `producer_consumer`: producers allocate batches of 512 blocks (16 to 512 B) and pass them through a small queue to a consumer that frees them. Every free happens on a different thread than the allocation.
- `phase_shift`: allocate 32 MB of blocks of one size, free them all, then repeat with the next size (32 B up to 8 KB).
- `footprint_16B`, `footprint_4100B`: hold many objects live and compare RSS to the bytes requested.
- `realloc_growth`: grow one buffer in 4 KB steps to 4 MB, five times.
- `thread_churn`: thousands of short-lived threads, each doing about 100 small alloc/free operations.

| Scenario | Threads | glibc time (s) | Mine time (s) | glibc peak RSS (MB) | Mine peak RSS (MB) |
|---|---|---|---|---|---|
| `large_alloc` | 1 | 0.001 | 0.100 | 1.9 | 1.0 |
| `large_alloc` | 4 | 0.003 | 1.460 | 4.5 | 1.0 |
| `large_alloc` | 8 | 0.005 | 2.344 | 8.1 | 1.0 |
| `producer_consumer` | 2 | 0.156 | 0.041 | 1.6 | 2.9 |
| `producer_consumer` | 4 | 0.210 | 0.046 | 2.2 | 3.9 |
| `producer_consumer` | 8 | 0.218 | 0.080 | 3.3 | 5.5 |
| `phase_shift` | 1 | 0.085 | 0.298 | 56.1 | 396.5 |
| `footprint_16B` | 1 | 0.046 | 0.077 | 61.0 | 122.1 |
| `footprint_4100B` | 1 | 0.082 | 0.040 | 78.4 | 156.2 |
| `realloc_growth` | 1 | 0.003 | 3.091 | 4.4 | 8.6 |
| `thread_churn` | 1 | 0.204 | 0.192 | 1.0 | 2.1 |
| `thread_churn` | 8 | 1.003 | 1.384 | 1.3 | 3.3 |

Throughput in millions of operations per second:

| Scenario | Threads | glibc | Mine |
|---|---|---|---|
| `large_alloc` | 1 / 4 / 8 | 29.06 / 50.85 / 58.33 | 0.40 / 0.11 / 0.14 |
| `producer_consumer` | 2 / 4 / 8 | 13.13 / 19.51 / 37.65 | 50.23 / 88.59 / 102.20 |
| `phase_shift` | 1 | 49.52 | 14.02 |
| `footprint_16B` | 1 | 86.46 | 52.11 |
| `footprint_4100B` | 1 | 0.49 | 0.99 |
| `realloc_growth` | 1 | 1.54 | about 0.002 (roughly 1,650 ops/sec) |
| `thread_churn` | 1 / 8 | 2.45 / 3.99 | 2.60 / 2.89 |

Memory use relative to what was requested:

| Scenario | Requested | glibc | Mine |
|---|---|---|---|
| `footprint_16B` (2M objects) | 32 MB | 2.00x | 4.00x |
| `footprint_4100B` (20k objects) | 82 MB | 1.00x | 2.00x |
| `phase_shift` (32 MB live at a time) | 32 MB | 1.75x | 12.39x |

Caveats on these numbers:

- Every figure is from a single run. I have not yet repeated them and taken medians.
- The glibc binary was built with `-O2` and without `-fno-builtin-malloc`, which the library does use.
- The glibc `large_alloc` and `realloc_growth` runs take only 1 to 5 ms, so noise and thread startup are a large share of them.
- Read the order-of-magnitude gaps as real. Don't read much into small differences.

## Analysis

### Why it beats glibc on the thread-private workload

The fast path is short. A thread-cache `malloc` is a pointer pop and `free` is a push. There are no locks, no atomics, no merging or splitting of neighbors and no boundary-tag updates. glibc does all of that bookkeeping on each operation, and that accounts for most of the 3x lead at one thread.

The thread cache fixed the scaling collapse from version 2. At 8 threads the run dropped from 80.3 s to 5.15 s, and the speedup went from 0.15x to 2.52x. A lock is now taken once per 16 blocks instead of once per operation. A block freed by a thread is also usually reused by that same thread, so its contents stay in that core's cache.

The remaining scaling gap is probably not the allocator. At 8 threads this allocator reaches 31.5% efficiency and glibc, with its per-thread arenas and its own tcache, reaches 36.2%. Both flatten at about the same place. The benchmark writes and verifies up to 8 KB of payload per operation, so a lot of the time goes to `memset` and comparisons competing for shared cache and memory bandwidth. The step from 4 to 8 threads gains about 1.34x for this allocator and 1.40x for glibc, which looks like SMT siblings sharing cores. That is a hypothesis. I haven't profiled the 8-thread run, and `perf` would show how much lock time is left.

The cache has costs too. Single-thread throughput fell about 10% between version 2 (55.9M ops/sec) and version 3 (50.3M), from the thread-local lookup and bookkeeping. Relative scaling at 8 threads is also a bit below glibc's (2.52x against 2.90x), even though absolute throughput is 2.7x higher. A faster baseline leaves less room to scale.

### Where it loses, and why

**No merging, no reuse across classes.** `phase_shift` shows this most clearly. glibc merges freed blocks and reuses the same pages for each new phase, so its peak is 56 MB, 8 MB of which is the benchmark's pointer array. Here, each phase's blocks stay on that class's free list forever and can't serve another size, so the phases pile up. Peak RSS was 396.5 MB, about 7x glibc's. Simple arithmetic roughly agrees: with a stride of `size + header`, the nine phases come to about 80 + 56 + 44 + 38 + 35 + 34 + 33 + 32 + 32, around 350 MB, plus the 8 MB array.

**Per-block overhead.** The block header is 48 bytes, which means a 64-byte stride for a 16-byte payload. Glibc uses a 32-byte chunk for the same object (2.00x). The extra memory costs time as well, since there are twice as many pages to fault in. That is why this scenario is 1.7x slower than glibc even though the allocation path itself is cheaper.

**Power-of-two classes.** A 4100-byte request lands in the 8192-byte class, so `footprint_4100B` uses 2.00x the requested memory against glibc's 1.00x. It is still twice as fast, because popping from a list is cheaper than glibc's search for a fit. This is the tradeoff working as intended.

**Large allocations.** Each large request is an `mmap` and each free is a `munmap`, plus page faults. Four threads do four times the work and take 14.6 times as long as one thread, which suggests the calls are serializing in the kernel, probably on the per-process mmap lock. glibc raises its mmap threshold dynamically after it frees a mapped chunk, and after that it recycles large blocks from its heap without system calls. I think that explains most of the 100x to 470x gap, but I have not confirmed either half with `strace` or `perf`, so treat it as a hypothesis.

**`realloc` growth.** Growing a dedicated mapping means mapping a new region, copying, and unmapping the old one. Growing by 4 KB steps to 4 MB copies about 2 MB per step on average, over about 1,000 steps and five repetitions. That is gigabytes of copying plus a pair of system calls per step. The run took 3.09 s against glibc's 3 ms. glibc can grow mapped chunks with `mremap`, which moves page-table entries and not bytes.

**Thread churn.** With one thread per round the two are about even (0.192 s and 0.204 s), since thread creation dominates. At 8 threads this allocator is 1.4x slower (1.384 s and 1.003 s). Every new thread maps its cache state, registers a `pthread_key` value and later runs the destructor, and threads that start together all refill from the same shared lists at the same time.

### One result that went the other way

I expected cross-thread frees to be a weakness of a thread-caching design, but this allocator was 2.7x to 4.6x faster than glibc on `producer_consumer`. The batched hand-off fits it well: the consumer's cache flushes whole batches to the shared lists and the producer refills from them, so each lock acquisition covers 16 blocks. I suspect glibc has to send each remotely freed chunk back to its owning arena under that arena's lock, but I haven't checked. This allocator also uses more memory in this scenario (5.5 MB against 3.3 MB at 8 threads). I'm leaving it in because it is a real result, even though it doesn't stress the design.

### What the results say about the design

| It does well | It does badly |
|---|---|
| Few instructions on the fast path | A header of about 48 bytes makes small objects expensive (4x for 16 B) |
| Thread-private workloads scale and stay cache-warm | Freed memory is never reused across size classes |
| Cross-thread frees need no owner tracking, and batched hand-offs are fast | Power-of-two classes above 512 B waste up to about 2x |
| Small enough to reason about and test | Large blocks and `realloc` growth enter the kernel on every operation |

It suits programs that allocate lots of small and medium objects with a stable size distribution. It is a poor fit for programs that move between size ranges over time, make many large allocations, or grow buffers with `realloc`.

## Correctness testing

| Test | What it covers |
|---|---|
| `tests/test_basic.c` | Basic semantics, alignment, overflow checks, zero-size cases |
| `tests/test_stress.c` | Size boundaries, the `calloc` threshold, failed `realloc`, many live allocations, randomized operations (single thread) |
| `tests/test_threads.c` | 8 threads on private memory, pattern fills, `realloc` content preservation, randomized operations |
| `tests/test_cross.c` | Producer/consumer with cross-thread frees, and 1000 short-lived threads to exercise the cache destructor |

All four binaries pass under ThreadSanitizer with no reports, and under AddressSanitizer with UBSan. On my machine TSan first aborted at startup with `unexpected memory mapping`, and the workaround described in [Build, test, benchmark](#build-test-benchmark) got it running.

The sanitizer builds compile the allocator source into each test with the `-D` renames, so the sanitizer's own allocator isn't the one under test. ASan doesn't poison blocks this allocator hands out, so it mostly checks the tests' own accesses and undefined behavior in the allocator's code, not overflows within allocator-managed blocks. TSan is the stronger check of the locking and thread-cache logic.

## Limitations

Known gaps as of now:

- No coalescing and no reuse across size classes. Memory in a class stays with that class for the life of the process.
- Large per-block overhead (a header of about 48 bytes, inferred from the measurements).
- Power-of-two classes above 512 B waste up to about 2x.
- Large allocations and `realloc` growth make at least one `mmap`/`munmap` pair per operation, and `realloc` always copies.
- A refill writes a header into every block of a whole chunk, so it touches all of that chunk's pages up front.
- No `posix_memalign`, `aligned_alloc` or `memalign`.
- `errno` isn't set to `ENOMEM` on failure.
- `malloc(0)` returns `NULL`. That is allowed by the standard, but many programs expect a unique pointer.
- Not fork-safe: a spinlock can be held across `fork`.
- The spinlocks have no backoff or yielding.

Because of the missing aligned-allocation functions and `errno` handling, it isn't ready to be used with `LD_PRELOAD` on arbitrary programs.

## Roadmap

In rough order of payoff against effort, based on the benchmark results:

1. Use `mremap` in `realloc` for dedicated mappings. About 15 lines, and it should remove the copying that dominates `realloc_growth`.
2. Keep a few freed large mappings around instead of unmapping right away, to cut the system calls in `large_alloc`.
3. Shrink the block header to 16 bytes: pack `class_index` and `is_free` into 32-bit fields, share one field between `actual_size` and `mapping_size`, and store the free-list pointer in the payload while a block is free. This should about halve `footprint_16B` and reduce `phase_shift`.
4. Carve chunks lazily with a bump pointer so a refill doesn't touch a whole chunk.
5. Use finer size classes (four per power of two above 512 B) to cut internal waste.
6. Reclaim fully free chunks with per-chunk free counts and `munmap`. This is the real fix for `phase_shift` and the most invasive change.
7. Add `posix_memalign`, `aligned_alloc` and `memalign`, set `errno`, add fork handlers and add spin backoff.
8. Profile the 8-thread run with `perf` and try wider thread-cache batches.

After items 1 to 3 I'll rerun Benchmark 2 and add the new numbers next to the current ones.

## AI assistance

I used AI assistants while working on this project, in these areas:

- **Refactoring.** Restructuring the code, for example splitting the public entry points from the static `*_impl` functions.
- **Troubleshooting and debugging.** Diagnosing problems such as the `calloc` self-recursion caused by GCC's optimization, and getting the sanitizer builds to work.
- **Tests and benchmarks.** Writing and extending the test programs (including the cross-thread test) and the benchmarks, including the adversarial one.
- **This README.** Drafting and editing it, including the analysis sections. (I came up with most of the results myself and used the AI to help compile and write them up.)

The benchmark numbers come from runs on my own machine. I ran the tests and sanitizers myself, and I am responsible for what is in this repository. The explanations in the analysis are hypotheses where I say they are, and some of them are still unverified.

## License

MIT.
