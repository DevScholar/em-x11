/* wasm32 time_t compat — wasm32's time_t is long long (64-bit) but
 * long is only 32-bit. Code using std::max(1000l, ...) where the second
 * argument involves time_t arithmetic causes template deduction failure:
 * "deduced conflicting types for parameter '_Tp' ('long' vs. 'time_t'
 * (aka 'long long'))".
 *
 * On x86_64 Linux, both types are 64-bit and this never trips. */

#ifndef EM_X11_WASM32_TIME_T_COMPAT_HH
#define EM_X11_WASM32_TIME_T_COMPAT_HH

#ifdef __cplusplus

#include <sys/time.h>
#include <algorithm>

namespace std {

inline time_t max(long a, long long b) { return a > (time_t)b ? (time_t)a : b; }
inline time_t max(long long a, long b) { return a > (time_t)b ? a : (time_t)b; }

} // namespace std

#endif // __cplusplus
#endif
