#include <cstddef>

////////////////////////////////////////////////////////////////////////////////

extern "C" size_t malloc_usable_size(void* ptr) noexcept;
extern "C" size_t nallocx(size_t size, int flags) noexcept;

////////////////////////////////////////////////////////////////////////////////
