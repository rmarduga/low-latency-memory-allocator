/*
 * low_latency_object_pool_test.cpp
 *
 *  Created on: Oct 11, 2017
 *      Author: rmarduga
 */

#include <cstdint>
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <memory>
#include <chrono>
#include <csignal>
#include <functional>
#include <stddef.h>
#include <thread>
#include <mutex>
#include <condition_variable>


#include "object_pool_impl.hpp"

struct MyClass{
    int64_t int64Array[4];  // 32 bytes
    int32_t int32Array[8];  // 64 bytes
    int16_t int16Array[16]; // 96 bytes
    int32_t int32;          // 104 bytes (+4 bytes padding)
    
    MyClass() : int32(42) {}
    ~MyClass() {}
};
typedef std::chrono::duration<int64_t> duration;
struct LatencyResult{
    size_t min = 0;
    size_t max = 0;
    size_t avg = 0;
    size_t total = 0;
    size_t number = 0;
};



template<class TestImpl>
class AbstructTest {
public:
    AbstructTest(size_t threadCount, size_t maxCapacity, size_t minCapacity) :
        threadCount(threadCount),
        results(threadCount),
        objectPool(sizeof(MyClass), maxCapacity, minCapacity) {
    }
    virtual ~AbstructTest() {}
    void start() {
        std::vector<std::thread> threadPool(threadCount - 1);
        size_t objectListSizePerThread = totalNumberOfObjects / threadCount;
        size_t threadId = 0;
        for (; threadId < threadPool.size(); ++threadId) {
            threadPool[threadId] = std::thread(
                                        &AbstructTest::threadFunction,
                                        this,
                                        objectListSizePerThread,
                                        threadId
                                    );
        }
        {
            std::lock_guard<std::mutex> lk(m);
            isRunning = true;
        }
        cv.notify_all();
        doThreadFunction(totalNumberOfObjects - threadPool.size() * objectListSizePerThread, threadId);
        for (auto&& thread : threadPool) {
            thread.join();
        }
    }
    void threadFunction(size_t objectListSizePerThread, int threadId) {
        {
            std::unique_lock<std::mutex> lk(m);
            cv.wait(lk, [this]{return isRunning;});
        }
        doThreadFunction(objectListSizePerThread, threadId);
    }
    void printStatistics(){

        std::cout << "Statistics per thread:\n"
                  << std::right << std::setw(35) << "min, nanooseconds"
                  << std::setw(30) << "max, nanoseconds"
                  << std::setw(30) << "avg, nanooseconds\n";
        LatencyResult allocResTotal, deallocResTotal;
        std::tie(allocResTotal, deallocResTotal) = results[0];
        for (auto&& result : results) {
            LatencyResult allocRes, deallocRes;
            std::tie(allocRes, deallocRes) = result;
            if (allocRes.min < allocResTotal.min) {
                allocResTotal.min = allocRes.min;
            } else if (allocRes.max > allocResTotal.max) {
                allocResTotal.max = allocRes.max;
            }
            allocResTotal.total += allocRes.total;
            allocResTotal.number+= allocRes.number;

            std::cout << std::left  << std::setw(15) << "allocation"
                      << std::right << std::setw(20) << allocRes.min
                                    << std::setw(30) << allocRes.max
                                    << std::setw(29) << allocRes.avg << "\n"
                      << std::left  << std::setw(15) <<  "deallocation:"
                      << std::right << std::setw(20) << deallocRes.min
                                    << std::setw(30) << deallocRes.max
                                    << std::setw(29) << deallocRes.avg << "\n\n";
        }

        std::cout << "Total statistics:\n";
        std::cout << std::left  << std::setw(15) << "allocation"
                  << std::right << std::setw(20) << allocResTotal.min
                                << std::setw(30) << allocResTotal.max
                                << std::setw(29) << allocResTotal.avg << "\n"
                  << std::left  << std::setw(15) <<  "deallocation:"
                  << std::right << std::setw(20) << deallocResTotal.min
                                << std::setw(30) << deallocResTotal.max
                                << std::setw(29) << deallocResTotal.avg << "\n\n";


        std::cout.flush();
    }
protected:
    void doThreadFunction(size_t objectListSizePerThread, int threadId) {
        using std::chrono::nanoseconds;
        static_cast<TestImpl*>(this)->doInitImpl(objectListSizePerThread, threadId);
        duration totalLatency = duration::zero();
        LatencyResult allocResult;
        allocResult.number = objectListSizePerThread;
        allocResult.total = 0;
        for (size_t i = 0; i < objectListSizePerThread; ++i) {
            auto start = std::chrono::high_resolution_clock::now();
            static_cast<TestImpl*>(this)->doAllocImpl(threadId);
            auto end = std::chrono::high_resolution_clock::now();
            size_t latency = std::chrono::duration_cast<nanoseconds>(end-start).count();
            if (latency > allocResult.max) {
                allocResult.max = latency;
            }else if (latency < allocResult.min || allocResult.min == 0) {
                allocResult.min = latency;
            }
            allocResult.total += latency;

        }
        allocResult.avg = allocResult.total / allocResult.number;

        {
            {
                std::lock_guard<std::mutex> lkGuard(m);
                ++numberOfThreadsThatFinishedAllocation;
            }
            cv.notify_all();
            {
                std::unique_lock<std::mutex> lk(m);
                std::lock_guard<std::mutex> lkGuard(m, std::adopt_lock);
                cv.wait(lk, [this]{return numberOfThreadsThatFinishedAllocation == threadCount;});
            }


        }


        static_cast<TestImpl*>(this)->doPreDeallocInitImpl(threadId);

        LatencyResult deallocResult;
        deallocResult.number = objectListSizePerThread;
        for (size_t i = 0; i < objectListSizePerThread; ++i) {
            auto start = std::chrono::high_resolution_clock::now();
            static_cast<TestImpl*>(this)->doDeallocImpl(threadId);
            auto end = std::chrono::high_resolution_clock::now();
            size_t latency = std::chrono::duration_cast<nanoseconds>(end-start).count();
            if (latency > deallocResult.max) {
                deallocResult.max = latency;
            }else if (latency < deallocResult.min || deallocResult.min == 0) {
                deallocResult.min = latency;
            }
            deallocResult.total += latency;

        }
        deallocResult.avg = deallocResult.total / deallocResult.number;
        results[threadId] = std::make_pair(allocResult, deallocResult);

    }
protected:
    bool isRunning = false;
    size_t threadCount;
    std::vector<std::pair<LatencyResult, LatencyResult>> results;
    const size_t totalNumberOfObjects = 1000000;
    ObjectPoolImpl objectPool;
    typedef MyClass* ObjectPtr;
    typedef std::vector<ObjectPtr> ObjectPtrList;
    size_t numberOfThreadsThatFinishedAllocation = 0;
    std::mutex m;
    std::condition_variable cv;
};


struct SystemAllocatorTest : public AbstructTest<SystemAllocatorTest> {
    SystemAllocatorTest(size_t threadCount, size_t maxCapacity, size_t minCapacity) :
        AbstructTest(threadCount, maxCapacity, minCapacity)
      , objectListPerThread(threadCount)
    {

    }

    void doInitImpl(size_t objectListSizePerThread, int threadId) {
        objectListPerThread[threadId].reserve(objectListSizePerThread);
    }

    void doAllocImpl(int threadId) {
        MyClass* obj = stdAllocator.allocate(1);
        stdAllocator.construct(obj);
        objectListPerThread[threadId].push_back(obj);
    }

    void doPreDeallocInitImpl(int threadId) {}


    void doDeallocImpl(int threadId) {
        MyClass* obj = objectListPerThread[threadId].back();
        objectListPerThread[threadId].pop_back();
        stdAllocator.destroy(obj);
        stdAllocator.deallocate(obj, 1);
    }

private:
    std::vector<ObjectPtrList> objectListPerThread;
    std::allocator<MyClass> stdAllocator;
};


struct AllocationAndDeallocationInSameThreadsTest : public AbstructTest<AllocationAndDeallocationInSameThreadsTest> {
    AllocationAndDeallocationInSameThreadsTest(size_t threadCount, size_t maxCapacity, size_t minCapacity) :
        AbstructTest(threadCount, maxCapacity, minCapacity)
      , objectListPerThread(threadCount)
      , isFromPoolPerThread(threadCount)
    {

    }

    void doInitImpl(size_t objectListSizePerThread, int threadId) {
        objectListPerThread[threadId].reserve(objectListSizePerThread);
        isFromPoolPerThread[threadId].reserve(objectListSizePerThread);
    }

    void doAllocImpl(int threadId) {
        void* chunk = objectPool.allocate();
        if (chunk) {
            MyClass* obj = new (chunk) MyClass();
            objectListPerThread[threadId].push_back(obj);
            isFromPoolPerThread[threadId].push_back(true);
        } else {
            // Fallback to std allocator when pool exhausted
            MyClass* obj = stdAllocator.allocate(1);
            stdAllocator.construct(obj);
            objectListPerThread[threadId].push_back(obj);
            isFromPoolPerThread[threadId].push_back(false);
        }
    }

    void doPreDeallocInitImpl(int threadId) {}

    void doDeallocImpl(int threadId) {
        MyClass* obj = objectListPerThread[threadId].back();
        bool isFromPool = isFromPoolPerThread[threadId].back();
        objectListPerThread[threadId].pop_back();
        isFromPoolPerThread[threadId].pop_back();
        
        if (isFromPool) {
            obj->~MyClass();
            objectPool.deallocate(obj);
        } else {
            stdAllocator.destroy(obj);
            stdAllocator.deallocate(obj, 1);
        }
    }
private:
    std::vector<ObjectPtrList> objectListPerThread;
    std::vector<std::vector<bool>> isFromPoolPerThread;
    std::allocator<MyClass> stdAllocator;
};


class AllocationAndDeallocationInDifferentThreadsTest : public AbstructTest<AllocationAndDeallocationInDifferentThreadsTest> {
public:
    AllocationAndDeallocationInDifferentThreadsTest(size_t threadCount, size_t maxCapacity, size_t minCapacity) :
        AbstructTest(threadCount, maxCapacity, minCapacity)
        , objectListPerThread(threadCount)
        , isFromPoolPerThread(threadCount)
    {

    }

    void doInitImpl(size_t objectListSizePerThread, int threadId) {
        objectListPerThread[threadId].reserve(objectListSizePerThread);
        isFromPoolPerThread[threadId].reserve(objectListSizePerThread);
    }

    void doAllocImpl(int threadId) {
        void* chunk = objectPool.allocate();
        if (chunk) {
            MyClass* obj = new (chunk) MyClass();
            objectListPerThread[threadId].push_back(obj);
            isFromPoolPerThread[threadId].push_back(true);
        } else {
            // Fallback to std allocator when pool exhausted
            MyClass* obj = stdAllocator.allocate(1);
            stdAllocator.construct(obj);
            objectListPerThread[threadId].push_back(obj);
            isFromPoolPerThread[threadId].push_back(false);
        }
    }

    void doPreDeallocInitImpl(int threadId) {

    }

    void doDeallocImpl(int threadId) {
        auto& objectList = objectListPerThread[(threadId + 1) % threadCount];
        auto& isFromPoolList = isFromPoolPerThread[(threadId + 1) % threadCount];
        if (objectList.empty()) return;
        MyClass* obj = objectList.back();
        bool isFromPool = isFromPoolList.back();
        objectList.pop_back();
        isFromPoolList.pop_back();
        
        if (isFromPool) {
            obj->~MyClass();
            objectPool.deallocate(obj);
        } else {
            stdAllocator.destroy(obj);
            stdAllocator.deallocate(obj, 1);
        }
    }
private:
    std::vector<ObjectPtrList> objectListPerThread;
    std::vector<std::vector<bool>> isFromPoolPerThread;
    std::allocator<MyClass> stdAllocator;
};




void printUsage(const char* progName){
    std::cout << "Usage: " << progName << " <thread count> <maxCapacity> <minCapacity>\n"
              << "<thread count> in range 1..<number of available hardware threads -1>\n"
              << "<maxCapacity> is greater than <minCapacity>" << std::endl;
}

int main (int argc, char *argv[]) {
    if (argc != 4) {
        printUsage(argv[0]);
        return -1;
    }
    for (int i = 0; i <= argc; ++i) {
        if ( argv[i] == "-h" || argv[i] == "--help" || argv[i] == "--usage" ) {
            printUsage(argv[0]);
       }
    }
    size_t threadCount = std::stoi(argv[1]);
    size_t maxCapacity = std::stoi(argv[2]);
    size_t minCapacity = std::stoi(argv[3]);

    if (threadCount < 1) {
        std::cout << "<thread count> should be at least 1>" << std::endl;
        return -1;
    }
    if (maxCapacity <= minCapacity) {
        std::cout << "<maxCapacity> should be greater than <minCapacity>" << std::endl;
        return -1;
    }

    {
        std::cout << "\n***** Standard Allocator Using std::allocator Test ******\n";
        SystemAllocatorTest test(threadCount, maxCapacity, minCapacity);
        test.start();
        test.printStatistics();
    }

    {
        std::cout << "\n***** ObjectPoolImpl Test with all allocations and deallocations happened within the same thread ******\n";
        AllocationAndDeallocationInSameThreadsTest test(threadCount, maxCapacity, minCapacity);
        test.start();
        test.printStatistics();

    }

    {
        std::cout << "\n***** ObjectPoolImpl Test with all allocations and deallocations happened in the different threads ******\n";
        AllocationAndDeallocationInDifferentThreadsTest test(threadCount, maxCapacity, minCapacity);
        test.start();
        test.printStatistics();

    }


}


