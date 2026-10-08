#pragma once
// stb_truetype declarations for the global module fragment of the UI font module. The implementation lives in
// stb_impl.cpp.

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push, 0)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#endif

#include <stb_truetype.h>

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
