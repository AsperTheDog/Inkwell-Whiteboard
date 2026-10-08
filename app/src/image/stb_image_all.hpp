#pragma once
// stb image headers (declarations only) for the global module fragment of the codec module. The implementations
// live in stb_impl.cpp.

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

#include <stb_image.h>
#include <stb_image_resize2.h>
#include <stb_image_write.h>

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
