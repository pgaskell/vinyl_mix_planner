#pragma once

#include <sys/mman.h>

#include <cstddef>
#include <cstdlib>
#include <new>

namespace vmp
{

// For track-sized buffers (tens to hundreds of MB): maps them straight from the system and
// unmaps them when freed, so the memory goes back at once. macOS's allocator otherwise keeps
// large freed blocks dirty for reuse, and they count against the app until it quits. Small
// allocations go through malloc as usual.
template <typename T>
struct LargeAllocator
{
    using value_type = T;
    static constexpr size_t kMapAbove = size_t{1} << 20;

    LargeAllocator() = default;
    template <typename U>
    LargeAllocator(const LargeAllocator<U>&) noexcept
    {
    }

    T* allocate(size_t n)
    {
        const size_t bytes = n * sizeof(T);
        if (bytes < kMapAbove)
        {
            if (void* p = std::malloc(bytes))
                return static_cast<T*>(p);
            throw std::bad_alloc();
        }
        void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p == MAP_FAILED)
            throw std::bad_alloc();
        return static_cast<T*>(p);
    }

    void deallocate(T* p, size_t n) noexcept
    {
        const size_t bytes = n * sizeof(T);
        if (bytes < kMapAbove)
            std::free(p);
        else
            ::munmap(p, bytes);
    }

    template <typename U>
    bool operator==(const LargeAllocator<U>&) const noexcept
    {
        return true;
    }
};

} // namespace vmp
