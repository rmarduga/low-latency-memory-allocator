/*
 * object_pool_impl.cpp
 *
 *  Created on: May 29, 2026
 *      Author: Ruslan Mardugalliamov
 */

#include "object_pool_impl.hpp"
#include "spinlock_mutex.hpp"

#include <cassert>
#include <forward_list>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>

struct ObjectPoolImpl::Impl {
    struct Chunk {
        Chunk* nextAvailable;
        unsigned char* data() {
            return reinterpret_cast<unsigned char*>(this + 1);
        }
        static Chunk* fromData(void* ptr) {
            return reinterpret_cast<Chunk*>(static_cast<char*>(ptr) - sizeof(Chunk));
        }
    };

    struct Page {
        void* memory;
        const std::size_t size;
        Chunk* chunks;

        Page(std::size_t chunkSize, std::size_t chunksNumber, Chunk* nextChunk)
            : memory(nullptr), size(chunksNumber), chunks(nullptr) {
            assert(chunksNumber > 0);
            std::size_t chunkTotalSize = sizeof(Chunk) + chunkSize;
            memory = ::operator new(chunkTotalSize * chunksNumber);
            char* base = static_cast<char*>(memory);
            for (std::size_t i = 0; i < chunksNumber; ++i) {
                Chunk* chunk = reinterpret_cast<Chunk*>(base + i * chunkTotalSize);
                if (i < chunksNumber - 1) {
                    chunk->nextAvailable = reinterpret_cast<Chunk*>(base + (i + 1) * chunkTotalSize);
                } else {
                    chunk->nextAvailable = nextChunk;
                }
            }
            chunks = reinterpret_cast<Chunk*>(base);
        }

        ~Page() {
            ::operator delete(memory);
        }

        // Non-copyable
        Page(const Page&) = delete;
        Page& operator=(const Page&) = delete;
    };

    Impl(std::size_t chunkSize, std::size_t maxCapacity, std::size_t minCapacity)
        : chunkSize(chunkSize)
        , pages()
        , capacity(maxCapacity)
        , firstAvailableChunk(nullptr)
        , minCapacity(minCapacity)
        , newPageSize(maxCapacity - minCapacity)
        , isReplenishNeeded(false)
        , isRunning(true)
        , replenishPoolThread(&Impl::poolReplenishWorker, this)
    {
        pages.emplace_front(chunkSize, maxCapacity, nullptr);
        firstAvailableChunk = pages.front().chunks;
    }

    ~Impl() {
        {
            std::lock_guard<std::mutex> lk(replenishMutex);
            isRunning = false;
        }
        replenishActivatorCV.notify_one();
        replenishPoolThread.join();
    }

    void* allocate() {
        if (!capacity) {
            return nullptr;
        }
        bool useStandardAllocator = false;
        Chunk* chunkToUse = nullptr;
        bool isActivateReplenish = false;
        do {
            std::lock_guard<SpinLockMutex> lk(spinLockMutex);
            if (!capacity) {
                useStandardAllocator = true;
                break;
            } else {
                --capacity;
                if (capacity == static_cast<ssize_t>(minCapacity)) {
                    isActivateReplenish = true;
                }
                chunkToUse = firstAvailableChunk;
                firstAvailableChunk = chunkToUse->nextAvailable;
            }
        } while (false);

        if (useStandardAllocator) {
            return nullptr;
        }

        if (isActivateReplenish) {
            runReplenish();
        }
        return chunkToUse->data();
    }

    void deallocate(void* ptr) {
        Chunk* freeChunk = Chunk::fromData(ptr);
        std::lock_guard<SpinLockMutex> lk(spinLockMutex);
        freeChunk->nextAvailable = firstAvailableChunk;
        firstAvailableChunk = freeChunk;
        ++capacity;
    }

private:
    void runReplenish() {
        isReplenishNeeded = true;
        {
            if (!replenishMutex.try_lock()) {
                // replenish worker is awake. No need to notify it.
                return;
            }
            std::lock_guard<std::mutex> lk(replenishMutex, std::adopt_lock);
            isReplenishNeeded = true; // modifying isReplenishNeeded under mutex in order to correctly publish
                                      // the modification to the waiting thread.
        }
        replenishActivatorCV.notify_one();
    }

    void poolReplenishWorker() {
        while (true) {
            {
                std::unique_lock<std::mutex> lk(replenishMutex);
                replenishActivatorCV.wait(lk, [this] { return isReplenishNeeded || !isRunning; });
            }
            isReplenishNeeded = false;
            if (!isRunning) return;
            pages.emplace_front(chunkSize, newPageSize, nullptr);
            {
                std::lock_guard<SpinLockMutex> spinlockMutexGuard(spinLockMutex);
                capacity += newPageSize;
                Chunk* nextAvailableChunk = firstAvailableChunk;
                // Get the last chunk of the new page
                std::size_t chunkTotalSize = sizeof(Chunk) + chunkSize;
                char* base = static_cast<char*>(pages.front().memory);
                Chunk* lastChunk = reinterpret_cast<Chunk*>(base + (pages.front().size - 1) * chunkTotalSize);
                lastChunk->nextAvailable = nextAvailableChunk;
                firstAvailableChunk = pages.front().chunks;
            }
            // by that time it is possible that another thread requested the replenish again
            // this situation will be handled by replenishActivatorCV.wait() with predicate that
            // will not block the thread if isReplenishNeeded == true
        }
    }

    const std::size_t chunkSize;
    std::forward_list<Page> pages;
    ssize_t capacity;
    Chunk* firstAvailableChunk;
    const std::size_t minCapacity;
    const std::size_t newPageSize;
    bool isReplenishNeeded;
    SpinLockMutex spinLockMutex;
    bool isRunning;
    std::mutex replenishMutex;
    std::condition_variable replenishActivatorCV;
    std::thread replenishPoolThread;
};

ObjectPoolImpl::ObjectPoolImpl(std::size_t chunkSize, std::size_t maxCapacity, std::size_t minCapacity)
    : pImpl(new Impl(chunkSize, maxCapacity, minCapacity))
{
}

ObjectPoolImpl::~ObjectPoolImpl() {
    delete pImpl;
}

void* ObjectPoolImpl::allocate() {
    return pImpl->allocate();
}

void ObjectPoolImpl::deallocate(void* ptr) {
    pImpl->deallocate(ptr);
}
