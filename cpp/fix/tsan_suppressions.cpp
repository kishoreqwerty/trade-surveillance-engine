// Compiles the same suppressions as the repo-root tsan_suppressions.txt
// directly into any binary linking tse_fix, via TSan's __tsan_default_suppressions
// weak-hook (part of compiler-rt's sanitizer common interface — no header
// needed, TSan's runtime looks this symbol up by name at startup).
//
// This exists so the suppressions apply no matter how a test binary is
// invoked — via `ctest` (which also gets them through each
// gtest_discover_tests(... ENVIRONMENT "${TSE_TEST_ENVIRONMENT}") in
// cpp/tests/*/CMakeLists.txt) or by running e.g. `./fix_tests` directly,
// which bypasses CTest's environment entirely. Only compiled in when
// SANITIZER=thread (see cpp/fix/CMakeLists.txt) — under any other
// configuration this file isn't part of the build at all.
//
// Keep this in sync with /tsan_suppressions.txt at the repo root, which is
// the canonical, documented copy (see cpp/fix/README.md for the full
// investigation behind each entry). If you change one, change both.

extern "C" const char* __tsan_default_suppressions() {
    return "race_top:FIX::Mutex::lock*\n"
           "race_top:FIX::SessionState::enabled*\n"
           "race_top:FIX::SocketAcceptor::onStart*\n"
           "race_top:FIX::SocketAcceptor::onStop*\n";
}
