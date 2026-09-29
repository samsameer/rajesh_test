# Sensor Fusion Pipeline

This project simulates four synchronized temperature sensors (96 kHz, 71 kHz,
69.9 kHz and 23 kHz). It merges their readings into one stream sorted by time
and, for every new reading, computes two fusion values over the N most recent
readings:

1. the geometric mean, and
2. the square root of the normalised sum of pairwise products.

It's written in plain C11 with POSIX threads and has no third-party
dependencies.

More detail lives in `docs/`:

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md): how the pieces fit
  together, with diagrams
- [`docs/TESTING.md`](docs/TESTING.md): what the tests cover and how to check
  results against the sample datasets

---

## Requirements checklist

| Requirement                                              | Where it's handled                                   |
|----------------------------------------------------------|------------------------------------------------------|
| Four sensors at 96 / 71 / 69.9 / 23 kHz, simulated       | `src/sensor.c`, one thread each                      |
| Streams aggregated and temporally sorted                 | `src/aggregator.c`, k-way merge                      |
| Same-microsecond timestamps: keep the lower-frequency sensor | `src/aggregator.c`, scan in ascending frequency  |
| No value missed apart from conflicts                     | bounded queues with back-pressure; checked in `test_end_to_end_pipeline` |
| Fusion 1: geometric mean of the N most recent readings   | `sf_fusion_geometric_mean` in `src/fusion.c`         |
| Fusion 2: pairwise-product fusion                        | `sf_fusion_pairwise_strength` in `src/fusion.c`      |
| Invalid values defined per algorithm, results always valid | "What counts as invalid" below; `test_invalid_inputs_rejected` |
| N > 4096, set on the command line                        | `-n / --window`, smaller values rejected             |
| Runs for 5 s, keeps writing until the queue is empty     | `src/main.c`, drain-then-close shutdown              |
| Output file format                                       | `src/writer.c`; example below                        |
| Unit test that loads a one-number-per-line file          | `tools/fusion_file.c`, `tests/run_file_tests.sh`     |
| CMake or Makefile build                                  | both provided                                        |
| Build, run and test instructions                         | this file, plus `docs/TESTING.md`                    |
| Design decisions, complexity, challenges, limitations    | sections below                                       |
| External libraries justified                             | none used                                            |

---

## Quick start

```sh
make            # build everything into ./build
make test       # run all the tests
make run        # 5 second acquisition, writes fusion_output.txt
```

If you prefer CMake:

```sh
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake -j
ctest --test-dir build-cmake --output-on-failure
```

You need a C11 compiler (GCC or Clang) and `make` or CMake 3.13+. It builds
on Linux, macOS and Windows (MinGW-w64).

### Windows

Plain `cmd` has no `make` or C compiler, so pick one of these:

**MSYS2 (native `.exe` files)**

1. Install MSYS2 from <https://www.msys2.org>.
2. Open the **MSYS2 UCRT64** terminal and install the tools:
   ```sh
   pacman -S --needed mingw-w64-ucrt-x86_64-gcc make git
   ```
3. Build and test:
   ```sh
   git clone https://github.com/samsameer/rajesh_test && cd rajesh_test
   make LDFLAGS=-static
   make test
   ```
4. The programs are ordinary Windows executables, statically linked, so you
   can also run them straight from `cmd`:
   ```bat
   build\sensor_fusion.exe -o fusion_output.txt
   build\fusion_file.exe -m both dataset_1_20250719_104255.csv
   ```

**CMake with MinGW** (if CMake and MinGW-w64 are already on your `PATH`):

```bat
cmake -S . -B build-cmake -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake
ctest --test-dir build-cmake --output-on-failure
build-cmake\sensor_fusion.exe
```

**WSL**: run `wsl --install` once, then follow the Linux steps inside
Ubuntu.

MSVC isn't supported, because it lacks C11 `<stdatomic.h>` and POSIX threads.

## Running it

```sh
./build/sensor_fusion                    # defaults: N = 8192, 5 seconds
./build/sensor_fusion -n 5000 -o run.txt
./build/sensor_fusion --help
```

| Option              | What it does                                              | Default             |
|---------------------|-----------------------------------------------------------|---------------------|
| `-n, --window N`    | how many recent readings the fusion functions look at     | 8192                |
| `-d, --duration MS` | how long the sensors run, in milliseconds                 | 5000                |
| `-o, --output PATH` | where to write results                                    | `fusion_output.txt` |
| `-m, --f2-norm`     | `formula` or `pair-mean` (see "The one ambiguity" below)  | `formula`           |
| `-f, --fault-rate`  | chance that a sensor sends a bad value, 0 to 1            | 0.0005              |
| `-s, --seed`        | random seed, for repeatable runs                          | taken from the clock |
| `-k, --stride K`    | only write every K-th result                              | 1                   |
| `-b, --batch-us`    | how often sensor threads wake up, in microseconds         | 100                 |

N has to be bigger than 4096, as the assignment requires. Smaller values are
rejected with a clear message.

The output file looks like this:

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

A short run summary goes to the terminal (stderr), so the output file keeps
exactly the requested format. The summary shows how many samples each sensor
lost to timestamp collisions, how many bad values were rejected, how long the
run took, and the seed, so you can reproduce the run.

## Checking a data file

`fusion_file` reads one number per line and prints both fusion values:

```sh
./build/fusion_file dataset_1_20250719_104255.csv
./build/fusion_file -m both dataset_*.csv     # show both fusion-2 variants
./build/fusion_file -n 5000 readings.txt      # only the last 5000 valid readings
```

It's forgiving about input. For a line like `index,value` it uses the last
field. It skips header lines and anything else that isn't a number, and
rejects values outside 0–100. It tells you how many lines fell into each
bucket, so nothing gets ignored silently.

---

## Design decisions

The short version is below. [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)
has diagrams and the longer reasoning.

**Integer timestamps.** Sample `k` of a sensor at frequency `f` is stamped
`floor(k × 10⁶ / f)` µs, computed entirely in integers (frequencies are stored
in millihertz). The spec defines a collision as "identical to the
microsecond", and floating-point periods would make that check depend on
rounding. With integers it's exact and repeatable.

**One producer, one consumer per queue.** Each sensor has its own queue into
the aggregator, and the aggregator has one queue into the fusion stage. With
exactly one writer and one reader per queue, I could use a simple lock-free
ring buffer with no mutexes. Queues are bounded, and a full queue makes the
writer wait rather than drop data, because the spec says sensors never miss a
value.

**Merge by waiting for everyone.** The aggregator only forwards a sample once
every sensor that's still running has something queued. Only then can it be
sure it's holding the earliest timestamp. On a tie it keeps the
lower-frequency sensor and drops the rest, as the spec says.

**O(1) fusion functions.** Recomputing both functions over the whole window
for every sample would cost O(N), or O(N²) for fusion 2 done naively. Instead
the engine keeps three running sums (Σ ln x, Σ x, Σ x²) and updates them when
a value enters or leaves the window. Fusion 2 uses the identity
`Σᵢ≠ⱼ xᵢxⱼ = (Σx)² − Σx²`.

**Working in log space.** Multiplying thousands of numbers between 0 and 100
overflows (or underflows) a `double` almost straight away, so the geometric
mean is computed as `exp(mean of ln x)`. Zero is a legitimate reading, but
`ln 0` is minus infinity, so zeros are counted separately. While a zero is in
the window the answer is exactly 0, and it recovers once the zero slides out.

**Controlling drift.** Running sums pick up rounding error over millions of
updates. I use compensated (Neumaier) summation and also recompute the sums
exactly every N samples. The recompute is O(N) but only happens once per N
samples, so it stays O(1) on average. Please don't build with `-ffast-math`,
which would optimise the compensation away.

**What counts as invalid.** NaN, infinity, negative numbers and anything
above 100. These either break the maths (NaN or infinity would poison the
running sums forever, and `ln` of a negative is undefined) or fall outside
the stated range. Rejected values never enter the window, so the fusion
values are always finite.

**A fast output path.** A 5-second run writes about 2.3 million lines
(~110 MB), so the writer turned out to be the real bottleneck, not the maths.
It uses a 1 MB buffer, rebuilds the `HH:MM:SS` part of the timestamp only
when the second changes, and formats numbers with a small fixed-point routine
instead of `printf("%f")`.

### The one ambiguity

The assignment describes fusion 2 as "the square root of the **average** of
all distinct pairwise products", but the printed formula divides by N, not by
the number of pairs. The two differ by a factor of √(N−1): for readings that
are all 50, one gives about 4500 and the other gives 50.

I couldn't tell which was intended, so both are there:

- `formula` (the default) follows the printed equation: `sqrt((S² − Q) / N)`
- `pair-mean` follows the wording: `sqrt((S² − Q) / (N(N−1)))`

Running `./build/fusion_file -m both` on the provided datasets and comparing
with `Results.txt` settles it.

---

## Complexity

| Operation                          | Time                | Space           |
|------------------------------------|---------------------|-----------------|
| Evaluate fusion 1 or fusion 2      | O(1)                | –               |
| Add a reading (and evict the oldest) | O(1) amortised    | –               |
| Sliding window                     | –                   | O(N), 16 bytes per reading |
| Aggregator, per sample             | O(number of sensors) | bounded queues |

In practice a 5-second run handles about 260,000 samples per second end to
end, writing every result, and finishes a few milliseconds after the sensors
stop.

## Technical challenges

- **Getting "simultaneous" right.** Floating-point periods look natural but
  make the microsecond comparison depend on rounding. Integer millihertz
  arithmetic makes it exact.
- **Ordering without losing anything.** A merge that forwards too early can
  emit a sample and then receive an earlier one from a slower sensor. Making
  the aggregator wait for every live sensor, and using a "closed" flag to take
  finished sensors out of the picture, solved it cleanly.
- **Numerical stability.** A sliding window of logs and squares looks simple
  until you run it for millions of samples. The zero counter, compensated
  sums and periodic recompute together keep results within about 1e-11 of an
  exact recomputation.
- **Memory ordering.** Lock-free queues are easy to get subtly wrong, so the
  whole test suite also runs under ThreadSanitizer (`make tsan`).
- **Output speed.** Writing a line for every sample at 260 kHz is a lot of
  text. The per-line costs are number formatting and turning the clock into
  `HH:MM:SS`, so those are the two things the writer optimises.

## Limitations

- Timing is "soft" real time. Sensors wake in ~100 µs batches rather than
  exactly on every sample. The timestamps are still exact, and ordering isn't
  affected.
- If the disk can't keep up, the sensors slow down rather than drop data. The
  run then takes a little over 5 seconds but stays complete and correct.
- Collisions are resolved by timestamp before values are checked, as the rule
  is written. So if the lower-frequency sensor's value at a collision is
  invalid, the other sensor's value isn't used in its place.
- A single zero reading pins the geometric mean at 0 for the next N samples.
  That's mathematically right, but for a real temperature sensor you might
  prefer to treat 0 as a fault.
- Up to 8 sensors are supported (a compile-time constant).
- Needs POSIX threads and clocks: Linux, macOS, or MinGW-w64 on Windows.
  MSVC is not supported. On Windows the OS sleep granularity is about 1 ms,
  so sensors wake in coarser batches; the data and ordering are unchanged.

## External libraries

None. Only the C standard library, `libm` and POSIX threads.

## Project layout

```
include/sf/          headers, one per module
src/                 implementation, plus main.c for the application
tools/fusion_file.c  command-line tool for checking data files
tests/               unit tests, file-based tests and sample data
docs/                architecture and testing notes
```
