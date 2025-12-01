#pragma once

#include <memory>
#include <mutex>
#include <vector>
#include <queue>
#include <new>

#if defined(_WIN32)
#include <malloc.h>
#else
#include <cstdlib>
#endif

namespace util {
    struct AlignedDeleter {
        void operator()(void* p) const {
#if defined(_WIN32)
            _aligned_free(p);
#else
            std::free(p);
#endif
        }
    };

    template <class T>
    class ObjectsPool {
    public:
        ObjectsPool(size_t preallocated = 0) {
            for (size_t i = 0; i < preallocated; i++) {
                allocateNew();
            }
        }

        template<typename... Args>
        std::shared_ptr<T> create(Args&&... args) {
            std::lock_guard lock(mutex);
            if (freeObjects.empty()) {
                allocateNew();
            }
            auto ptr = freeObjects.front();
            freeObjects.pop();
            new (ptr)T(std::forward<Args>(args)...);
            return std::shared_ptr<T>(reinterpret_cast<T*>(ptr), [this](T* ptr) {
                ptr->~T();
                std::lock_guard lock(mutex);
                freeObjects.push(ptr);
            });
        }
    private:
        std::vector<std::unique_ptr<void, AlignedDeleter>> objects;
        std::queue<void*> freeObjects;
        std::mutex mutex;

        void allocateNew() {
            // Use posix_memalign on POSIX systems as aligned_alloc has stricter requirements
            constexpr size_t alignment = alignof(T) < sizeof(void*) ? sizeof(void*) : alignof(T);
            constexpr size_t size = sizeof(T);
            void* rawPtr = nullptr;
#if defined(_WIN32)
            rawPtr = _aligned_malloc(size, alignment);
#else
            if (posix_memalign(&rawPtr, alignment, size) != 0) {
                rawPtr = nullptr;
            }
#endif
            if (rawPtr == nullptr) {
                throw std::bad_alloc();
            }
            std::unique_ptr<void, AlignedDeleter> ptr(rawPtr);
            freeObjects.push(ptr.get());
            objects.push_back(std::move(ptr));
        }
    };
}
