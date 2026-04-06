/*
 * Tiny in-place median (nth_element style) for the measurement module.
 * Operates on `int64_t` samples; not stable, not the fastest, but
 * trivial and small. n ≤ 200 in practice.
 */

#include <stdint.h>
#include <stddef.h>

static void swap64(int64_t *a, int64_t *b) {
    int64_t t = *a; *a = *b; *b = t;
}

static size_t partition(int64_t *a, size_t lo, size_t hi) {
    int64_t pivot = a[hi];
    size_t i = lo;
    for (size_t j = lo; j < hi; j++) {
        if (a[j] < pivot) { swap64(&a[i], &a[j]); i++; }
    }
    swap64(&a[i], &a[hi]);
    return i;
}

static void nth_element(int64_t *a, size_t lo, size_t hi, size_t k) {
    while (lo < hi) {
        size_t p = partition(a, lo, hi);
        if (p == k) return;
        if (k < p) hi = p - 1;
        else       lo = p + 1;
    }
}

int64_t median_i64(int64_t *a, size_t n) {
    if (n == 0) return 0;
    nth_element(a, 0, n - 1, n / 2);
    int64_t mid = a[n / 2];
    if ((n & 1) == 0) {
        /* For even n, the spec follows std::nth_element semantics from
         * the reference (`Median.hpp`) — average the two middle ones. */
        nth_element(a, 0, n - 1, n / 2 - 1);
        return (a[n / 2 - 1] + mid) / 2;
    }
    return mid;
}
