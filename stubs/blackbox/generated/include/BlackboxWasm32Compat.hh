/* wasm32 compat for blackbox — wasm32's time_t is long long (64-bit) but
 * long is only 32-bit. Blackbox's code uses std::max(1000l, ...) where
 * the second argument involves time_t arithmetic, causing template
 * deduction failure: "deduced conflicting types for parameter '_Tp'
 * ('long' vs. 'time_t' (aka 'long long'))".
 *
 * On x86_64 Linux, both types are 64-bit and this never trips.
 * We bridge the gap with a single overload rather than patching the
 * third-party source. */

#ifndef EMX11_BLACKBOX_WASM32_COMPAT_HH
#define EMX11_BLACKBOX_WASM32_COMPAT_HH

#ifdef __cplusplus

#include <sys/time.h>
#include <algorithm>

namespace std {

inline time_t max(long a, long long b) { return a > (time_t)b ? (time_t)a : b; }
inline time_t max(long long a, long b) { return a > (time_t)b ? a : (time_t)b; }

} // namespace std

#endif // __cplusplus
#endif
