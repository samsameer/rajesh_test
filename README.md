# Sensor Fusion Pipeline

Real-time aggregation of four synchronized temperature sensor streams
(96 kHz, 71 kHz, 69.9 kHz, 23 kHz) into a single temporally sorted stream, with
two sliding-window fusion functions evaluated over the **N most recent**
readings.

- Language: C11 + POSIX threads
- Build: GNU Make or CMake
- Dependencies: none beyond libc, libm and pthreads

```
 Sensor 1 (96 kHz)   ──► SPSC ─┐
 Sensor 2 (71 kHz)   ──► SPSC ─┤                        ┌─► Fusion 1 (geometric mean)
 Sensor 3 (69.9 kHz) ──► SPSC ─┼─► Aggregator ─► SPSC ──┤
 Sensor 4 (23 kHz)   ──► SPSC ─┘  (k-way merge)         └─► Fusion 2 (pairwise strength)
                                                                   │
                                                                   ▼
                                                           fusion_output.txt
```

---

## 1. Repository layout

```
include/sf/        public headers
  sample.h         sample record {timestamp_us, value, sensor_id}
  spsc_queue.h     lock-free single-producer/single-consumer ring buffer
  sensor.h         simulated sensor (thread + timestamp arithmetic)
  aggregator.h     k-way temporal merge with conflict resolution
  fusion.h         sliding-window fusion engine
  writer.h         buffered, timestamped output writer
  backoff.h        spin -> yield -> sleep wait strategy
  clock.h, rng.h   monotonic clock, SplitMix64 generator
src/               implementation + main.c (application)
tools/fusion_file.c  loads a text file and prints both fusion functions
tests/             unit tests, file-based tests, sample data
```

---

## 2. Build, run, test

### Make

```sh
make                 # builds build/sensor_fusion, build/fusion_file and the tests
make test            # unit tests + file-based tests
make run             # 5 s acquisition -> fusion_output.txt
make run ARGS="-n 10000 -o out.txt"
make sanitize        # AddressSanitizer + UndefinedBehaviorSanitizer test run
make tsan            # ThreadSanitizer test run
make clean
```

Extra compiler flags are appended, never substituted:
`make CFLAGS=-Werror`, `make CC=clang`, `make OPT="-O3 -march=native"`.

### CMake

```sh
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake -j
ctest --test-dir build-cmake --output-on-failure
```

### Running the pipeline

```sh
./build/sensor_fusion [options]

  -n, --window N         number of most recent readings (N > 4096, default 8192)
  -d, --duration MS      acquisition time in milliseconds (default 5000)
  -o, --output PATH      output file (default fusion_output.txt)
  -m, --f2-norm MODE     formula | pair-mean   (see §3.6, default formula)
  -f, --fault-rate P     probability of an injected invalid reading (default 0.0005)
  -s, --seed S           random seed (default derived from the clock)
  -k, --stride K         write every K-th fusion result (default 1)
  -b, --batch-us US      sensor wake-up interval in microseconds (default 100)
```

`N <= 4096` is rejected, as required by the specification.

Output file:

```
[10:11:05.787]: Program started
[10:11:05.822]: fusion function 1 = 48.164049
[10:11:05.822]: fusion function 2 = 4575.105429
...
Count of generated values for each sensor:
[10:11:10.788]: Sensor 1 = 480000
[10:11:10.788]: Sensor 2 = 355000
[10:11:10.788]: Sensor 3 = 349500
[10:11:10.788]: Sensor 4 = 115000
[10:11:10.788]: Program finished
```

A run summary (per-sensor forwarded / conflict-discarded / injected-invalid
counts, rejected readings, elapsed time, seed) is printed to `stderr` so the
output file keeps exactly the required format.

### Testing the fusion functions against a file

```sh
./build/fusion_file dataset_1_20250719_104255.csv
./build/fusion_file -m both dataset_*.csv          # print both F2 normalisations
./build/fusion_file -n 5000 data.txt               # only the last 5000 valid readings
```

The tool reads one number per line. If a line has several fields separated by
`,` `;` or whitespace, the last field is used, so `index,value` CSV files work
unchanged. Header lines and anything that does not parse as a number are
counted as malformed and skipped; values outside `[0, 100]`, `NaN` and `inf`
are counted as rejected. By default the window equals the number of valid
readings in the file.

Test inventory:

| Suite                   | What it proves                                                                 |
|-------------------------|--------------------------------------------------------------------------------|
| `test_fusion`           | closed-form results, window eviction, zero handling, invalid rejection, range boundaries, O(N²) brute-force equivalence, 3 M-sample drift check against a `long double` reference, formatter |
| `test_pipeline`         | SPSC ordering, wraparound, 2 M-item threaded FIFO; timestamp arithmetic; deterministic merge + conflict resolution; end-to-end run checked against an independent count of distinct timestamps |
| `run_file_tests.sh`     | `fusion_file` on `tests/data/*` (plain, CSV, malformed lines, zeros, windowing) |

---

## 3. Design decisions

### 3.1 Time base and timestamps

The four clocks are synchronized, so every sensor shares one origin. Sample `k`
of a sensor with frequency `f` is stamped

```
t_k = floor(k * 1e6 / f)  microseconds
```

computed in pure integer arithmetic with `f` stored in millihertz
(69.9 kHz = 69 900 000 mHz). Floating-point periods would drift and would make
the "identical to the microsecond" rule nondeterministic; integer arithmetic
makes the timestamps exact and reproducible.

Each sensor thread is paced by the monotonic clock: it wakes every
`--batch-us` (100 µs), emits every sample whose timestamp has elapsed and sleeps
again. Sub-10 µs sleeps are not achievable on a general-purpose OS, so batching
keeps real-time behaviour without busy-waiting a core per sensor. Sample count
over the acquisition window is exact:
`ceil(duration_us * f / 1e6)` → 480 000 / 355 000 / 349 500 / 115 000 for 5 s.

### 3.2 Sensor signal

A bounded, mean-reverting random walk around a per-sensor anchor in
`[20, 80]`, clamped to `[0, 100]`. With `--fault-rate` > 0 a small fraction of
readings is replaced by `NaN`, `+inf`, a negative value or a value above 100 to
exercise the validation path continuously.

### 3.3 Queues

Every hop is a bounded lock-free **SPSC ring buffer** (power-of-two capacity,
acquire/release atomics, head and tail on separate cache lines, each side
caching the other side's index to avoid cache-line ping-pong). SPSC is the
exact topology of the pipeline, so no locks or CAS loops are needed. Queues are
bounded; a full queue applies back-pressure (the producer waits) instead of
dropping, because sensors are defined as perfect and no value may be lost.

A `closed` flag published with release semantics after the last push lets the
consumer distinguish "empty for now" from "finished".

### 3.4 Aggregation and conflict resolution

The aggregator performs a k-way merge. It only emits when every live source has
a head sample available, which guarantees global temporal order: the smallest
head is the smallest timestamp that will ever be produced.

Sources are scanned in **ascending frequency** order and the minimum is chosen
with a strict `<`, so on equal timestamps the lowest-frequency sensor wins.
All other heads carrying the same microsecond timestamp are then discarded and
counted. Each sensor's timestamps are strictly increasing (period > 1 µs), so a
conflict involves at most one sample per sensor. At `t = 0` all four collide
and sensor 4 (23 kHz) is kept.

### 3.5 Fusion engine – O(1) per sample

A ring buffer of the last `N` accepted readings stores `(x, ln x)` pairs.
Three running sums are maintained incrementally on insert/evict:

| Quantity         | Used for                                  |
|------------------|-------------------------------------------|
| `L = Σ ln xᵢ`    | geometric mean `exp(L / N)`               |
| `S = Σ xᵢ`       | pairwise sum `Σ_{i≠j} xᵢxⱼ = S² − Q`      |
| `Q = Σ xᵢ²`      |                                           |

**Geometric mean.** The product of thousands of values in `[0, 100]`
overflows or underflows `double` immediately, so it is evaluated in log space.
`ln 0 = −∞` would poison the running sum, therefore zeros are tracked with a
counter instead: if any zero is in the window the result is exactly `0`, and the
log sum is kept only over positive values, so the value recovers correctly as
soon as the zero leaves the window.

**Pairwise strength.** The double sum over `i ≠ j` is `O(N²)`; the identity
`Σ_{i≠j} xᵢxⱼ = (Σxᵢ)² − Σxᵢ²` reduces it to `O(1)`. Because
`Q / S² ≈ 1/N`, the subtraction does not suffer catastrophic cancellation; a
tiny negative result from rounding is clamped to `0` before `sqrt`.

**Numerical drift.** Sliding sums accumulate rounding error over millions of
add/subtract cycles. Two measures bound it:

1. Neumaier-compensated summation for all three sums.
2. A full rebase (exact recomputation from the buffer) every `N` insertions.
   This costs `O(N)` once per `N` samples, i.e. amortised `O(1)`.

The drift test pushes 3 million samples whose magnitude alternates between
`1e-3` and `100` and matches a `long double` reference to 1e-11 relative.
The code must not be built with `-ffast-math`, which would remove the
compensation terms.

**Invalid values.** Derived from what each algorithm can consume and the
specified domain:

| Input                   | Why invalid                                   | Handling                    |
|-------------------------|-----------------------------------------------|-----------------------------|
| `NaN`, `±inf`           | poisons every running sum permanently         | rejected                    |
| `x < 0`                 | `ln x` undefined; outside domain              | rejected                    |
| `x > 100`               | outside domain                                | rejected                    |
| `x = 0`                 | valid reading; `ln 0 = −∞`                    | accepted, tracked by counter|

Rejected readings never enter the window, so the fusion values are always
finite. Results are only written once the window holds `N` valid readings.
Geometric mean and pair-mean outputs are clamped to `[0, 100]` to absorb
last-ulp rounding.

### 3.6 Fusion function 2 normalisation

The specification's prose says "square root of the **average** of all distinct
pairwise products", while the printed formula divides the sum over `i ≠ j` by
`N`. These differ by a factor of `√(N−1)`. Both are implemented:

| `--f2-norm`         | Formula                                    | Constant input `c` gives |
|---------------------|--------------------------------------------|--------------------------|
| `formula` (default) | `sqrt( (S² − Q) / N )`                     | `c·√(N−1)`               |
| `pair-mean`         | `sqrt( (S² − Q) / (N(N−1)) )`              | `c`                      |

The default follows the formula as printed. `fusion_file -m both` prints both
variants so the reference `Results.txt` identifies the intended one.

### 3.7 Output writer

The output volume is large (≈ 2.3 M lines, ≈ 110 MB for a 5 s run with
stride 1), so formatting is on the hot path:

- 1 MiB `stdio` buffer; each result pair is one `fwrite`.
- The `[HH:MM:SS.` prefix is recomputed only when the second changes
  (`localtime_r` is not called per line); milliseconds are written directly.
- Values use a dedicated fixed-point formatter (6 decimals) instead of
  `printf("%f")`, with a `snprintf` fallback for non-finite or huge values.

`--stride K` reduces the file size when the full per-sample trace is not
needed.

### 3.8 Threads

| Thread       | Role                                               |
|--------------|----------------------------------------------------|
| 4 × sensor   | generate and push samples, paced by the clock      |
| aggregator   | k-way merge, conflict resolution                   |
| main         | fusion + output, runs until the aggregate queue is drained |

Waiting uses an adaptive backoff: `pause` spin → `sched_yield` → 50 µs sleep, so
idle stages cost almost no CPU and busy stages react within nanoseconds.

---

## 4. Complexity

| Operation                     | Time              | Space        |
|-------------------------------|-------------------|--------------|
| Fusion 1 / Fusion 2 query     | O(1)              | –            |
| Insert with eviction          | O(1) amortised (O(N) rebase every N inserts) | – |
| Fusion window                 | –                 | O(N) (16 bytes per reading) |
| Aggregator per emitted sample | O(k), k = 4 sensors | O(queue capacity) |
| Naive reference Fusion 2      | O(N²)             | O(N)         |

For a 5 s run the pipeline sustains ≈ 260 k samples/s end-to-end, including
writing every result, and finishes within a few milliseconds after the
acquisition window closes.

---

## 5. Technical challenges

1. **Exact simultaneity.** Deciding "identical to the microsecond" requires a
   deterministic time base; integer millihertz arithmetic solves it and makes
   runs reproducible.
2. **Ordering without losing data.** A merge may only emit when every live
   stream has a pending sample; otherwise a late sample could arrive with a
   smaller timestamp. The `closed` flag removes finished streams from the
   decision.
3. **Numerically stable sliding window.** Log-space product, zero counter,
   compensated sums and periodic rebase together keep the O(1) algorithm
   within 1e-11 of the exact result.
4. **Memory ordering.** The SPSC queues rely on acquire/release pairs only;
   the full test suite is run under ThreadSanitizer (`make tsan`).
5. **Output throughput.** The file writer, not the math, is the bottleneck;
   the prefix cache and custom formatter keep it well above real time.

---

## 6. Implementation limitations

- Real-time pacing is soft: sensors emit in ~100 µs batches rather than at
  exact per-sample instants. Timestamps are exact regardless, and ordering is
  unaffected.
- If the output device is slower than the sensor rate, back-pressure makes the
  sensors lag wall-clock time instead of dropping data; the run then takes
  longer than 5 s but remains complete and correct.
- Conflict resolution is applied on timestamps before validation, as the rule
  states; if the lower-frequency sensor's reading is invalid, the discarded
  higher-frequency reading is not used as a substitute.
- A single zero in the window forces the geometric mean to `0` for the next
  `N` samples, which is mathematically correct but may not be desirable for a
  physical temperature sensor.
- The aggregator supports up to 8 sources (compile-time constant).
- POSIX-only (`pthread`, `clock_gettime`, `nanosleep`, `getline`); tested on
  Linux with GCC 13 and Clang.

## 7. External libraries

None. Only the C standard library, `libm` and POSIX threads are used.
