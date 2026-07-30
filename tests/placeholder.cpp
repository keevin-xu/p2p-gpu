// Placeholder so the `tests` target links before any real tests exist.
// Replace with real Catch2 sources as they land; keep the self-check below.
//
// ── SANITIZER SELF-CHECK ─────────────────────────────────────────────────
// A preset can link the ASan runtime and STILL compile your translation units
// uninstrumented. That build reports clean forever, and every "sanitizer-clean"
// claim downstream becomes meaningless — the exact failure mode docs/RISKS.md
// R-H warns about. So do not trust the preset's description; make it prove it:
//
//   cmake -B build/selfcheck --preset native-debug -DP2PGPU_SELFCHECK=ON
//   cmake --build build/selfcheck --target tests
//   ./build/selfcheck/tests/tests asan     # expect: heap-buffer-overflow, abort
//   ./build/selfcheck/tests/tests ubsan    # expect: signed overflow,     abort
//   rm -rf build/selfcheck
//
// A clean exit from either means the sanitizers are NOT doing anything and the
// build configuration is broken, however green it looks.

#include <cstdio>
#include <cstring>
#include <vector>

int main(int argc, char** argv) {
#if defined(P2PGPU_SELFCHECK)
    if (argc > 1 && std::strcmp(argv[1], "asan") == 0) {
        // Heap buffer overflow. `volatile` keeps the index out of reach of
        // constant folding, so this survives to runtime instead of becoming a
        // compile-time -Warray-bounds diagnostic.
        std::vector<int> v(4, 1);
        volatile std::size_t i = 8;
        std::printf("asan self-check read: %d\n", v[i]);
        std::puts("FAIL: ASan did not fire — this build is NOT instrumented");
        return 1;
    }
    if (argc > 1 && std::strcmp(argv[1], "ubsan") == 0) {
        // Signed integer overflow — undefined behaviour, caught by UBSan.
        volatile int x = 2147483647;
        std::printf("ubsan self-check value: %d\n", x + 1);
        std::puts("FAIL: UBSan did not fire — this build is NOT instrumented");
        return 1;
    }
#else
    (void)argc;
    (void)argv;
#endif
    return 0;
}
