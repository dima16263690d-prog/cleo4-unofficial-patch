// Host unit test for DebugUtils.LeakFit.h.
//
// The fit decides whether a script's memory growth is a steady leak (a straight,
// positive line, R^2 near 1) or just noisy net growth (low R^2). A wrong sign or a
// wrong R^2 would either miss a real leak or cry wolf, so the math has a test. It
// has no Windows dependency, so it builds and runs on the host:
//
//   g++ -std=c++17 -I Plugins/DebugUtils Plugins/DebugUtils/Tests/leak_fit_test.cpp -o /tmp/leak_fit_test
//   /tmp/leak_fit_test
//
// Exit code 0 = all checks passed, 1 = at least one failed.

#include "DebugUtils.LeakFit.h"

#include <cmath>
#include <cstdio>

using namespace DebugUtilsLeakFit;

static int g_failures = 0;
static int g_checks = 0;

static void CheckNear(const char* what, double got, double want, double tol)
{
    ++g_checks;
    if (std::fabs(got - want) > tol)
    {
        ++g_failures;
        std::printf("FAIL %-46s got %.6f, want %.6f\n", what, got, want);
    }
    else
    {
        std::printf("ok   %-46s = %.6f\n", what, got);
    }
}

static void CheckTrue(const char* what, bool got)
{
    ++g_checks;
    if (!got)
    {
        ++g_failures;
        std::printf("FAIL %-46s got false, want true\n", what);
    }
    else
    {
        std::printf("ok   %-46s\n", what);
    }
}

int main()
{
    // A perfect straight line: 1 KB per second, so the slope is exactly 1024 B/s and
    // R^2 is 1. This is the "steady leak" case.
    {
        const unsigned int xs[] = {0, 10, 20, 30, 40};
        const unsigned long long ys[] = {0, 10240, 20480, 30720, 40960};
        const Fit fit = LeastSquares(xs, ys, 5);
        CheckTrue("linear: ok", fit.ok);
        CheckNear("linear: slope", fit.slope, 1024.0, 1e-6);
        CheckNear("linear: rSquared", fit.rSquared, 1.0, 1e-9);
    }

    // A downward line (memory released over time): negative slope, still R^2 = 1.
    {
        const unsigned int xs[] = {0, 10, 20, 30};
        const unsigned long long ys[] = {40960, 30720, 20480, 10240};
        const Fit fit = LeastSquares(xs, ys, 4);
        CheckTrue("falling: ok", fit.ok);
        CheckNear("falling: slope", fit.slope, -1024.0, 1e-6);
        CheckNear("falling: rSquared", fit.rSquared, 1.0, 1e-9);
    }

    // Noisy growth: the endpoints differ (so a naive endpoint check would call it a
    // leak) but the line fits poorly. R^2 must be low enough to reject it.
    {
        const unsigned int xs[] = {0, 10, 20, 30, 40};
        const unsigned long long ys[] = {100000, 40000, 90000, 50000, 110000};
        const Fit fit = LeastSquares(xs, ys, 5);
        CheckTrue("noisy: ok", fit.ok);
        CheckTrue("noisy: rSquared < 0.3", fit.rSquared < 0.3);
    }

    // Too few points: a fit needs at least three samples to mean anything.
    {
        const unsigned int xs[] = {0, 10};
        const unsigned long long ys[] = {0, 1024};
        const Fit fit = LeastSquares(xs, ys, 2);
        CheckTrue("two points: not ok", !fit.ok);
    }

    // A flat series has no y-variance: R^2 is undefined, so the fit is refused
    // rather than reporting a confident 0-slope "leak".
    {
        const unsigned int xs[] = {0, 10, 20, 30};
        const unsigned long long ys[] = {5000, 5000, 5000, 5000};
        const Fit fit = LeastSquares(xs, ys, 4);
        CheckTrue("flat: not ok", !fit.ok);
    }

    // All samples at the same second: no x-spread, so no slope to fit.
    {
        const unsigned int xs[] = {5, 5, 5, 5};
        const unsigned long long ys[] = {0, 100, 200, 300};
        const Fit fit = LeastSquares(xs, ys, 4);
        CheckTrue("same second: not ok", !fit.ok);
    }

    std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
