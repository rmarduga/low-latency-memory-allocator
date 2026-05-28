/*
 * object_pool.hpp
 *
 *  Created on: Oct 10, 2017
 *      Author: Ruslan Mardugalliamov
 */

#pragma once

#include <cstddef>
#include <cassert>
#include <memory>
#include <vector>
#include <atomic>
#include <utility>
#include <forward_list>
#include <mutex>
#include <thread>
#include <condition_variable>

#include "spinlock_mutex.hpp"

template<std::size_t MAX_OBJ_SIZE>
class ObjectPool {

    struct Chunk {
        unsigned char data[MAX_OBJ_SIZE];
        Chunk* nextAvailable;
    };
    struct Page {
        Chunk* chunks;
        const std::size_t size;
        Page(std::size_t chunksNumber, Chunk* nextChunk) :
            chunks(nullptr), size(chunksNumber) {
            assert(chunksNumber > 0);
            chunks = new Chunk[chunksNumber];
            chunks[chunksNumber -1].nextAvailable = nextChunk;
            for (std::size_t i = 0; i < chunksNumber - 1; ++i){
                chunks[i].nextAvailable = &chunks[i+1];
            }
        }
    };

public:
    ObjectPool (std::size_t maxCapacity, std::size_t minCapacity) :
         pages(1, Page(maxCapacity, nullptr))
      ,  firstAvailableChunk(nullptr)
      ,  capacity(maxCapacity)
      ,  minCapacity(minCapacity)
      ,  newPageSize(maxCapacity - minCapacity)
      ,  isReplenishNeeded(false)
      ,  replenishPoolThread(&ObjectPool::poolReplenishWorker, this)


    {
        firstAvailableChunk = &(pages.front().chunks[0]);
    }

    ~ObjectPool() {
        for(auto&& page: pages) {
            delete[] page.chunks;
        }
        {
            std::lock_guard<std::mutex> lk(replenishMutex, std::adopt_lock);
            isRunning = false;
        }
        replenishActivatorCV.notify_one();
        replenishPoolThread.join();
    }

    template<typename OBJECT, typename ... ARGS>
    void alloc (std::shared_ptr<OBJECT> & object, ARGS&& ... args){
        
        if (!capacity)
        {
            allocateUsingStdAllocator(object, std::forward<ARGS>(args) ...);
            return;
        }
        bool useStandardAllocator = false;
        Chunk* chunkToUse = nullptr;
        bool isActivateReplenish = false;
        do{
            std::lock_guard<SpinLockMutex> lk(spinLockMutex);
            if (!capacity) {
                useStandardAllocator = true;
                break;
            } else {
                --capacity;
                if (capacity == minCapacity) {
                    isActivateReplenish = true;
                }
                chunkToUse = firstAvailableChunk;
                firstAvailableChunk = chunkToUse->nextAvailable;
            }

        } while(false);

        if (useStandardAllocator) {
            allocateUsingStdAllocator(object, std::forward<ARGS>(args) ...);
            return;
        }


        if (isActivateReplenish) {
            runReplenish();
        }
        object = std::shared_ptr<OBJECT>( new (chunkToUse->data) OBJECT(std::forward<ARGS>(args) ... )
                                          , [this](OBJECT* pObject) {this->dealloc(pObject);}
        );
    }
    template<typename OBJECT, typename ... ARGS>
    void dealloc(OBJECT* object){
        object->~OBJECT();
        std::lock_guard<SpinLockMutex> lk(spinLockMutex);
        Chunk* freeChunk = reinterpret_cast<Chunk*>(object);
        freeChunk->nextAvailable = firstAvailableChunk;
        firstAvailableChunk = freeChunk;
        ++capacity;
    }

private:

    template<typename OBJECT, typename ... ARGS>
    void allocateUsingStdAllocator (std::shared_ptr<OBJECT> & object, ARGS&& ... args){
        std::allocator<OBJECT> stdAllocator;
        OBJECT* objPtr = stdAllocator.allocate(1);
        stdAllocator.construct(objPtr, std::forward<ARGS>(args) ...);
        object = std::shared_ptr<OBJECT>(objPtr, [stdAllocator](OBJECT* pObject) mutable -> void  {
            stdAllocator.destroy(pObject);
            stdAllocator.deallocate(pObject, 1);
        });
    }

    void runReplenish() {
        isReplenishNeeded = true;
        {
            if (!replenishMutex.try_lock()){
                // replenish worker is awake. No need to notify it.
                return;
            }
            std::lock_guard<std::mutex> lk(replenishMutex, std::adopt_lock);
            isReplenishNeeded = true; // modifying isReplenishNeeded under mutex in order to correctly publish
                                      // the modification to the waiting thread.
        }
        replenishActivatorCV.notify_one();

    }
    void poolReplenishWorker(){
        while (true){
            {
                std::unique_lock<std::mutex> lk(replenishMutex);
                std::lock_guard<std::mutex> lkReplenishMutexGuard(replenishMutex, std::adopt_lock);
                replenishActivatorCV.wait(lk, [this]{return isReplenishNeeded || !isRunning;});
            }
            isReplenishNeeded = false;
            if (!isRunning) return;
            pages.push_front(Page(newPageSize, nullptr));
            {

                std::lock_guard<SpinLockMutex> spinlockMutexGuard(spinLockMutex);
                capacity += newPageSize;
                Chunk* nextAvailableChunk = firstAvailableChunk;
                pages.front().chunks[ pages.front().size - 1 ].nextAvailable = nextAvailableChunk;
                firstAvailableChunk = &(pages.front().chunks[0]);
            }
            // by that time it is possible that another thread requested the replenish again
            // this situation will be handled by replenishActivatorCV.wait() with predicate that
            // will not block the thread if isReplenishNeeded == true
        }
    }
private:
    std::forward_list<Page> pages;
    ssize_t capacity;
    Chunk* firstAvailableChunk;
    const std::size_t minCapacity;
    const std::size_t newPageSize;
    bool isReplenishNeeded = false;
    SpinLockMutex spinLockMutex;
    bool isRunning = true;
    std::mutex replenishMutex;
    std::condition_variable replenishActivatorCV;
    std::thread replenishPoolThread;

};
