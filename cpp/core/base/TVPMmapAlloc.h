#ifndef TVP_MMAP_ALLOC_H
#define TVP_MMAP_ALLOC_H

#if defined(__APPLE__) || defined(__linux__) || defined(__ANDROID__)
#include <sys/mman.h>
#include <unistd.h>
#include <cstddef>
#include <limits>

inline void *TVPMmapAlloc(size_t size) {
    const long rawPageSize = sysconf(_SC_PAGESIZE);
    if(rawPageSize <= 0)
        return nullptr;
    const auto unsignedPageSize = static_cast<unsigned long long>(rawPageSize);
    if(unsignedPageSize > static_cast<unsigned long long>(
                              std::numeric_limits<size_t>::max()))
        return nullptr;
    const size_t pageSize = static_cast<size_t>(rawPageSize);
    if(pageSize == 0)
        return nullptr;
    if(size > std::numeric_limits<size_t>::max() - sizeof(size_t))
        return nullptr;
    size_t totalSize = size + sizeof(size_t);
    const size_t remainder = totalSize % pageSize;
    if(remainder != 0) {
        const size_t padding = pageSize - remainder;
        if(totalSize > std::numeric_limits<size_t>::max() - padding)
            return nullptr;
        totalSize += padding;
    }
    void *ptr = mmap(nullptr, totalSize, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANON, -1, 0);
    if(ptr == MAP_FAILED) return nullptr;
    *(size_t *)ptr = totalSize;
    return (char *)ptr + sizeof(size_t);
}

inline void TVPMmapFree(void *mem) {
    if(!mem) return;
    char *base = (char *)mem - sizeof(size_t);
    size_t totalSize = *(size_t *)base;
    munmap(base, totalSize);
}

#define TVP_USE_MMAP_TEMP 1
#endif

#endif
