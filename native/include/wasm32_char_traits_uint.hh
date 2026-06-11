/* std::char_traits<unsigned int> — libc++ only provides char_traits for
 * char, wchar_t, char8_t, char16_t, and char32_t. unsigned int is NOT
 * char32_t (it's a distinct fundamental type in C++), so this
 * specialization is required for any code that instantiates
 * basic_string<unsigned int>. */

#ifndef EM_X11_WASM32_CHAR_TRAITS_UINT_HH
#define EM_X11_WASM32_CHAR_TRAITS_UINT_HH

#ifdef __cplusplus

#include <algorithm>
#include <cstring>
#include <ios>

namespace std {

template <>
struct char_traits<unsigned int> {
  using char_type  = unsigned int;
  using int_type   = unsigned int;
  using off_type   = std::streamoff;
  using pos_type   = std::streampos;
  using state_type = std::mbstate_t;

  static void assign(char_type &c1, const char_type &c2) noexcept { c1 = c2; }

  static bool eq(char_type c1, char_type c2) noexcept { return c1 == c2; }
  static bool lt(char_type c1, char_type c2) noexcept { return c1 < c2; }

  static int compare(const char_type *s1, const char_type *s2, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
      if (lt(s1[i], s2[i])) return -1;
      if (lt(s2[i], s1[i])) return 1;
    }
    return 0;
  }

  static std::size_t length(const char_type *s) {
    std::size_t n = 0;
    while (!eq(s[n], char_type())) ++n;
    return n;
  }

  static const char_type *find(const char_type *s, std::size_t n,
                               const char_type &a) {
    for (std::size_t i = 0; i < n; ++i)
      if (eq(s[i], a)) return s + i;
    return nullptr;
  }

  static char_type *move(char_type *s1, const char_type *s2, std::size_t n) {
    if (n == 0) return s1;
    return static_cast<char_type *>(
        std::memmove(s1, s2, n * sizeof(char_type)));
  }

  static char_type *copy(char_type *s1, const char_type *s2, std::size_t n) {
    if (n == 0) return s1;
    return static_cast<char_type *>(
        std::memcpy(s1, s2, n * sizeof(char_type)));
  }

  static char_type *assign(char_type *s, std::size_t n, char_type a) {
    for (std::size_t i = 0; i < n; ++i) s[i] = a;
    return s;
  }

  static int_type not_eof(int_type c) noexcept {
    return eq_int_type(c, eof()) ? int_type() : c;
  }

  static char_type to_char_type(int_type c) noexcept { return c; }
  static int_type to_int_type(char_type c) noexcept { return c; }

  static bool eq_int_type(int_type c1, int_type c2) noexcept {
    return c1 == c2;
  }

  static int_type eof() noexcept { return static_cast<int_type>(-1); }
};

} // namespace std

#endif // __cplusplus

#endif
