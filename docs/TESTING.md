# Testing

Everything runs with one command:

```sh
make test
```

That builds and runs two unit-test programs and a set of file-based checks.
Each prints one line per test and a final tally, and exits non-zero if
anything fails, so it drops straight into CI.

```
[  OK  ] test_known_values
[  OK  ] test_window_eviction
...
213 checks, 0 failures
```

## What gets tested

### `test_fusion`: the maths

These tests don't touch threads at all. They feed numbers to the fusion engine
and check the answers.

| Test                            | Why it's there                                                        |
|---------------------------------|-----------------------------------------------------------------------|
| known values                    | `{1, 2, 4}` has a geometric mean of exactly 2; both fusion-2 variants checked against hand-worked answers |
| window eviction                 | after pushing more than N values, only the last N count              |
| zero handling                   | a zero forces the geometric mean to 0, which recovers once the zero leaves the window |
| invalid inputs                  | NaN, ±∞, −0.001, 100.0000001 and 1e308 are rejected and leave the state untouched |
| range boundaries                | exactly 0 and exactly 100 are accepted; the next representable doubles outside are not |
| extreme small values            | 1e-300 doesn't underflow the geometric mean                          |
| constant stream                 | N copies of c give c, c and c·√(N−1), as the maths says they should  |
| outputs stay in range           | rounding never pushes a result above 100                             |
| brute force comparison          | a naive O(N²) version, run in `long double`, agrees at many points along a random stream |
| long run drift                  | 3 million samples jumping between tiny and large values, then compared with an exact recomputation |
| number formatter                | the fast fixed-point output matches what `printf` would print        |

### `test_pipeline`: the plumbing

| Test                            | Why it's there                                                        |
|---------------------------------|-----------------------------------------------------------------------|
| queue capacity and order        | capacity rounds up to a power of two; a full queue refuses pushes; items come out in order |
| wraparound and bulk pop         | index wraparound over thousands of cycles with odd batch sizes       |
| threaded FIFO                   | 2 million items through a real producer and consumer thread, checked one by one |
| sensor timestamps               | spot values of the timestamp formula and the exact 5-second sample counts |
| merge and conflicts             | hand-built streams with deliberate collisions; checks both the order and which sensor wins each tie |
| empty sources                   | the aggregator shuts down cleanly when a sensor sends nothing        |
| end to end                      | real sensor threads for 300 ms with bad values injected; see below   |

The end-to-end test is the one I trust most. It runs the real pipeline and
then checks, independently of the code under test:

- the timestamps coming out are strictly increasing;
- each sensor produced exactly the expected number of samples;
- forwarded + dropped = generated, so nothing went missing;
- the number of forwarded samples equals the number of *distinct* timestamps
  across all four sensors, worked out separately by the test itself;
- the 23 kHz sensor never loses a collision;
- injected bad values were rejected (never more than were injected, since
  some are dropped earlier by collisions), and every fusion value stayed
  within 0–100.

### `run_file_tests.sh`: the command-line tool

This runs `fusion_file` against the small files in `tests/data/` and compares
its output with the expected values in `tests/data/cases.txt`, to a relative
tolerance of 1e-9.

| File               | What it exercises                                          |
|--------------------|------------------------------------------------------------|
| `basic.txt`        | the simplest possible case                                 |
| `with_invalid.txt` | text, NaN, inf, negatives, >100, blank lines, Windows line endings |
| `csv_style.csv`    | a header line and `index,value` rows                       |
| `with_zero.txt`    | the zero rule                                              |
| `constant.txt`     | 5000 identical readings                                    |
| `window_tail.txt`  | `-n 3`, so only the last three readings count              |

To add a case, drop a file in `tests/data/` and add a line to `cases.txt`.

## Checking against the provided datasets

The assignment comes with three sample files and a `Results.txt`. Put them
anywhere and run:

```sh
./build/fusion_file -m both dataset_1_20250719_104255.csv \
                            dataset_2_20250719_104255.csv \
                            dataset_3_20250719_104256.csv
```

For each file this prints the geometric mean and both versions of fusion 2.
Compare them with `Results.txt`. Whichever fusion-2 line matches tells you
which normalisation the reference used. Then use that as `-m` for the main
program.

If the reference computes over only the last N readings, rather than the
whole file, add `-n N`.

## Sanitizers

```sh
make sanitize   # AddressSanitizer + UndefinedBehaviorSanitizer
make tsan       # ThreadSanitizer
```

Both run the full test suite in a separate build directory. They're slower,
but they catch the bugs that normal tests can't see: out-of-bounds reads,
undefined behaviour, and data races in the lock-free queues. Both come back
clean.

## Extra compiler checks

```sh
make CFLAGS=-Werror
make CC=clang CFLAGS=-Werror
```

The code builds without a single warning under `-Wall -Wextra -Wpedantic
-Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wdouble-promotion` on both
GCC and Clang.
