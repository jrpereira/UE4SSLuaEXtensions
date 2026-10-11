#pragma once

// CHECK for every portable C++ suite. A failed CHECK reports its expression
// and location and the suite carries on, so one run shows every failure; it
// is not compiled out by NDEBUG the way assert is. End main with
//   return LexTests::finish("SuiteName");

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace LexTests
{
inline std::atomic<int> failures{0};

inline void report(bool ok, const char* expression, const char* file, int line)
{
    if (ok) return;
    ++failures;
    std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", file, line, expression);
}

inline int finish(const char* suite)
{
    const int failed = failures.load();
    if (failed != 0)
    {
        std::fprintf(stderr, "%s: %d check(s) failed\n", suite, failed);
        return EXIT_FAILURE;
    }
    std::printf("%s: all checks passed\n", suite);
    return EXIT_SUCCESS;
}
}

#define CHECK(expression) ::LexTests::report(static_cast<bool>(expression), #expression, __FILE__, __LINE__)
