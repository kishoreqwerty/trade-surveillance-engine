# cpp/fix/ — TSan findings and suppressions

This file documents an investigation, not the module's design (see the
architecture doc and this directory's headers for that). It exists because
running `cpp/tests/fix/fix_tests` under ThreadSanitizer surfaces real data
races, and both the races and how they're handled needed to be on the
record rather than silently fixed in a commit message no one reads.

## What TSan found

`fix_tests` brings up a genuine `FIX::SocketAcceptor`/`FIX::SocketInitiator`
pair over loopback TCP for the `LiveSessionFixture`-based tests (real
threads, real sockets — see `cpp/tests/fix/fix_session_test_fixture.cpp`).
Running that under TSan (`-DENABLE_SANITIZERS=ON -DSANITIZER=thread`)
reports data races. Frame-by-frame inspection of the actual TSan output
(not just the SUMMARY line) confirmed all of them originate inside
QuickFIX's own vendored source — the racing frame (frame `#0` of the
report) is never in this project's code:

| Confirmed race | QuickFIX source location |
|---|---|
| `FIX::Mutex::lock()` | `src/C++/Mutex.h:57` |
| `FIX::SessionState::enabled(bool)` | `src/C++/SessionState.h:57` |
| `FIX::SocketAcceptor::onStart()` / `onStop()` | `src/C++/SocketAcceptor.cpp:117` / `:147` |

The `onStart()`/`onStop()` pair is the clearest: `onStart()` (run on the
acceptor's background thread) writes a field with no synchronization;
`onStop()` (run on the main thread during `LiveSessionFixture::TearDown()`)
reads the same field, also unsynchronized. Every `LiveSessionFixture` test
does exactly `start()` then, a few messages later, `stop()` — the pattern
that triggers it.

None of `fix_application.cpp`, `message_translator.cpp`, `types.cpp`, or
any test file ever appears as the racing frame — only as a caller several
frames up the stack (e.g. `LiveSessionFixture::TearDown()` calling
`Initiator::stop()`, which internally calls `Session::lookupSession()`,
which is what actually races inside `FIX::Mutex::lock()`). This is what
justifies treating these as pre-existing QuickFIX library bugs to
suppress, not project bugs to fix.

## The suppressions

`/tsan_suppressions.txt` (repo root) is the canonical, documented copy —
read it for the full rationale on each entry. It uses `race_top` (not
`race`), which only matches when the named function is the *topmost*
frame of one of the two racing stacks. That's deliberate: it means this
suppression list can't accidentally swallow a real, new race in our own
code just because that race's call chain happens to pass through
`FIX::Session::lookupSession()` or similar on its way to the actual bug.

Two mechanisms apply it, so it doesn't depend on how the binary happens to
be invoked:

1. **`TSAN_OPTIONS` via CTest.** The root `CMakeLists.txt` sets
   `TSE_TEST_ENVIRONMENT` to `TSAN_OPTIONS=suppressions=<repo>/tsan_suppressions.txt`
   whenever `SANITIZER=thread`, and every `cpp/tests/*/CMakeLists.txt`
   passes it via `gtest_discover_tests(... PROPERTIES ENVIRONMENT
   "${TSE_TEST_ENVIRONMENT}")`. This covers `ctest`-driven runs.

2. **Compiled into the binary.** `cpp/fix/tsan_suppressions.cpp` defines
   `__tsan_default_suppressions()` — a weak hook compiler-rt's TSan
   runtime looks up by name at process startup — returning the same four
   lines. This covers running a test binary directly (`./fix_tests`,
   `./fix_tests --gtest_filter=...`), which bypasses CTest's environment
   entirely. It's only compiled in under `SANITIZER=thread`
   (`cpp/fix/CMakeLists.txt`).

   Getting this working took a second fix: nothing in this codebase
   *calls* `__tsan_default_suppressions()` — only TSan's runtime looks it
   up by symbol name — so the linker was silently dropping
   `tsan_suppressions.o` from `libtse_fix.a` (static archives only pull in
   object files that resolve some other object's undefined symbol).
   `fix_application.cpp` — unconditionally part of `tse_fix` — now forces
   a real reference to it under `__has_feature(thread_sanitizer)`, which
   is what actually keeps the object file linked in. Verified via `nm`
   that the symbol is present in the final `fix_tests` binary before
   trusting the suppression to work.

   Keep `tsan_suppressions.cpp` and `/tsan_suppressions.txt` in sync if
   you ever edit one.

## SIGABRT at process exit — and why it matters for CI

Before suppression, running `fix_tests` under TSan — directly, not through
`ctest` — printed `[PASSED] 17 tests` and then the process exited with
**code 134 (SIGABRT)**, apparently from Apple's TSan runtime aborting
after accumulating reported races at exit. Confirmed by isolating the real
exit code (`echo $?` immediately after the run, not after a pipe — piping
into `tail` masks the real code with `tail`'s own exit status, which is
almost always 0 and produced a false "it's fine" reading during this
investigation).

This matters beyond cosmetics: a nonzero exit code from a test binary that
actually passed every test is exactly what makes `ctest` — and any CI
system that just checks `$?` — report a fully green run as a failure. A
future contributor re-running this under TSan and seeing "all tests
passed" immediately followed by a nonzero exit would reasonably assume
something is broken.

With the suppressions correctly applied (both mechanisms above), the
races are never reported, so whatever triggers the abort never fires:

```
$ ./fix_tests > /tmp/run.log 2>&1; echo "exit: $?"
...
[  PASSED  ] 17 tests.
exit: 0
$ grep -c "WARNING: ThreadSanitizer" /tmp/run.log
0
```

Verified project-wide too — `ctest --test-dir build-debug-tsan`, all 68
tests, and `build-debug-tsan/Testing/Temporary/LastTest.log` (CTest's full
captured output regardless of pass/fail) contains zero occurrences of
`ThreadSanitizer: data race` or `ThreadSanitizer: reported`.

**The fix was suppression-driven, not `abort_on_error=0`-driven,
deliberately.** `TSAN_OPTIONS` was not given a blanket
`abort_on_error=0`/`halt_on_error=0` — doing that would also swallow the
abort (and the hard-failure signal) for a genuinely new race in code this
project actually owns. Suppressing only the three confirmed
third-party locations, and leaving TSan's default error handling
otherwise untouched, is what makes a future real race — say, in Phase 3's
SPSC ring buffer or Phase 6's live pipeline integration — still show up as
a hard, unmissable CI failure instead of being quietly absorbed by the
same override.

## An unrelated bug this investigation also surfaced

While tracking down why the suppressions "worked" when running the whole
`fix_tests` binary directly but two `LiveSessionFixture` tests hung and
timed out under `ctest`: `gtest_discover_tests` runs *every individual
test* as its own OS process (`fix_tests --gtest_filter=Suite.OneTest`).
`fix_session_test_fixture.cpp`'s port-picking counter was a
process-local `std::atomic<int>` starting at a fixed base — fine for
multiple fixtures sharing one process, but it resets to the same starting
port for every fresh ctest-spawned process. Two `LiveSessionFixture` tests
run back-to-back by `ctest` would both claim port 19870; the second could
hang binding a port still in the first process's `TIME_WAIT`. Fixed by
salting the port with `getpid()` so distinct OS processes land in
different port ranges, while the atomic counter still spreads out
multiple fixtures sharing a single process. Unrelated to TSan itself, but
found in the course of getting a genuinely clean TSan signal end to end,
so it's recorded here rather than left to look like an unexplained
one-line diff.
