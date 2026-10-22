#pragma once

// Pure least-squares fit for the memory-leak tracker, with no Windows dependency
// so it can be unit-tested on the host (Tests/leak_fit_test.cpp). It fits live
// bytes (y) against uptime seconds (x) and reports the slope in bytes/second plus
// R^2, the fraction of the variance the straight line explains. The tracker uses
// it to turn a heuristic "grows on average" verdict into a measured one: R^2 near
// 1 with a positive slope is a steady leak; R^2 near 0 means the series is noisy
// and the net growth may just be jitter.
namespace DebugUtilsLeakFit
{
    struct Fit
    {
        double slope = 0.0;     // bytes per second
        double rSquared = 0.0;  // 0..1
        bool ok = false;        // false when the points cannot define a line
    };

    // Fit n points. Requires n >= 3, at least two distinct x values, and a non-zero
    // spread in y (a flat series has no variance to explain, so R^2 is undefined
    // and ok stays false). Returns ok=false rather than a misleading 0 slope.
    inline Fit LeastSquares(
        const unsigned int* xs,
        const unsigned long long* ys,
        unsigned int n)
    {
        Fit fit;
        if (xs == nullptr || ys == nullptr || n < 3)
            return fit;

        double sumX = 0.0, sumY = 0.0, sumXY = 0.0, sumXX = 0.0, sumYY = 0.0;
        for (unsigned int i = 0; i < n; ++i)
        {
            const double x = static_cast<double>(xs[i]);
            const double y = static_cast<double>(ys[i]);
            sumX += x;
            sumY += y;
            sumXY += x * y;
            sumXX += x * x;
            sumYY += y * y;
        }

        const double dn = static_cast<double>(n);
        const double denomX = dn * sumXX - sumX * sumX;
        if (denomX <= 0.0)
            return fit; // every sample at the same second: no slope to fit

        const double num = dn * sumXY - sumX * sumY;
        const double denomY = dn * sumYY - sumY * sumY;
        // No variance in y (a flat series): R^2 is undefined, so there is nothing
        // for the fit to explain. Report no fit rather than a confident 0 slope.
        if (denomY <= 0.0)
            return fit;

        fit.slope = num / denomX;
        fit.rSquared = (num * num) / (denomX * denomY);
        fit.ok = true;
        return fit;
    }
}
