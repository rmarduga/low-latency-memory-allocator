/*
 * object_pool_test.cpp
 *
 *  Created on: May 29, 2026
 *      Author: Ruslan Mardugalliamov
 */

#include <gtest/gtest.h>
#include <cstdint>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>

#include "object_pool_impl.hpp"

struct MyClass {
    int64_t int64Array[4];  // 32 bytes
    int32_t int32Array[8];  // 64 bytes
    int16_t int16Array[16]; // 96 bytes
    int32_t int32;          // 104 bytes (+4 bytes padding)
    
    MyClass() : int32(42) {
        for (int i = 0; i < 4; ++i) int64Array[i] = i;
        for (int i = 0; i < 8; ++i) int32Array[i] = i;
        for (int i = 0; i < 16; ++i) int16Array[i] = i;
    }
    
    MyClass(int val) : int32(val) {
        for (int i = 0; i < 4; ++i) int64Array[i] = val;
        for (int i = 0; i < 8; ++i) int32Array[i] = val;
        for (int i = 0; i < 16; ++i) int16Array[i] = val;
    }
    
    ~MyClass() {
        int32 = -1; // Mark as destroyed
    }
};

// Test fixture for ObjectPoolImpl tests
class ObjectPoolImplTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Default setup
    }
    
    void TearDown() override {
        // Cleanup
    }
};

// Basic functionality tests for ObjectPoolImpl
TEST_F(ObjectPoolImplTest, BasicAllocateDeallocate) {
    ObjectPoolImpl pool(sizeof(MyClass), 100, 10);
    
    void* chunk = pool.allocate();
    ASSERT_NE(chunk, nullptr);
    
    // Construct object in the chunk
    MyClass* obj = new (chunk) MyClass();
    EXPECT_EQ(obj->int32, 42);
    
    // Destroy and deallocate
    obj->~MyClass();
    pool.deallocate(chunk);
}

TEST_F(ObjectPoolImplTest, AllocateWithConstruction) {
    ObjectPoolImpl pool(sizeof(MyClass), 100, 10);
    
    void* chunk = pool.allocate();
    ASSERT_NE(chunk, nullptr);
    
    MyClass* obj = new (chunk) MyClass(123);
    EXPECT_EQ(obj->int32, 123);
    EXPECT_EQ(obj->int64Array[0], 123);
    
    obj->~MyClass();
    pool.deallocate(chunk);
}

TEST_F(ObjectPoolImplTest, MultipleAllocations) {
    ObjectPoolImpl pool(sizeof(MyClass), 10, 2);
    std::vector<void*> chunks;
    std::vector<MyClass*> objects;
    
    for (int i = 0; i < 10; ++i) {
        void* chunk = pool.allocate();
        ASSERT_NE(chunk, nullptr);
        MyClass* obj = new (chunk) MyClass(i);
        EXPECT_EQ(obj->int32, i);
        chunks.push_back(chunk);
        objects.push_back(obj);
    }
    
    // Cleanup
    for (size_t i = 0; i < objects.size(); ++i) {
        objects[i]->~MyClass();
        pool.deallocate(chunks[i]);
    }
}

TEST_F(ObjectPoolImplTest, PoolExhaustionReturnsNull) {
    // Create a pool with capacity 2
    ObjectPoolImpl pool(sizeof(MyClass), 2, 1);
    
    void* chunk1 = pool.allocate();
    void* chunk2 = pool.allocate();
    void* chunk3 = pool.allocate(); // Should return nullptr (pool exhausted)
    
    ASSERT_NE(chunk1, nullptr);
    ASSERT_NE(chunk2, nullptr);
    EXPECT_EQ(chunk3, nullptr);
    
    // Cleanup
    pool.deallocate(chunk1);
    pool.deallocate(chunk2);
}

TEST_F(ObjectPoolImplTest, ReuseAfterDeallocate) {
    ObjectPoolImpl pool(sizeof(MyClass), 5, 2);
    
    void* chunk1 = pool.allocate();
    ASSERT_NE(chunk1, nullptr);
    MyClass* obj1 = new (chunk1) MyClass(100);
    EXPECT_EQ(obj1->int32, 100);
    obj1->~MyClass();
    pool.deallocate(chunk1);
    
    // Allocate again - should reuse the freed chunk
    void* chunk2 = pool.allocate();
    ASSERT_NE(chunk2, nullptr);
    // The chunk address should be the same (reused)
    EXPECT_EQ(chunk1, chunk2);
    
    MyClass* obj2 = new (chunk2) MyClass(200);
    EXPECT_EQ(obj2->int32, 200);
    obj2->~MyClass();
    pool.deallocate(chunk2);
}

// Multithreaded tests for ObjectPoolImpl
TEST_F(ObjectPoolImplTest, MultithreadedAllocateDeallocate) {
    ObjectPoolImpl pool(sizeof(MyClass), 1000, 100);
    const int numThreads = 4;
    const int allocsPerThread = 1000;
    std::vector<std::thread> threads;
    std::atomic<int> successCount{0};
    
    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([&pool, allocsPerThread, &successCount, t]() {
            for (int i = 0; i < allocsPerThread; ++i) {
                void* chunk = pool.allocate();
                if (chunk) {
                    MyClass* obj = new (chunk) MyClass(t * 1000 + i);
                    if (obj->int32 == t * 1000 + i) {
                        successCount++;
                    }
                    obj->~MyClass();
                    pool.deallocate(chunk);
                }
            }
        });
    }
    
    for (auto& thread : threads) {
        thread.join();
    }
    
    EXPECT_EQ(successCount, numThreads * allocsPerThread);
}

TEST_F(ObjectPoolImplTest, MultithreadedCrossThreadDealloc) {
    ObjectPoolImpl pool(sizeof(MyClass), 1000, 100);
    const int numThreads = 4;
    const int allocsPerThread = 100;
    std::vector<std::thread> threads;
    std::vector<void*> sharedChunks;
    std::mutex chunksMutex;
    std::atomic<int> allocCount{0};
    
    // Allocate in multiple threads
    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([&pool, &sharedChunks, &chunksMutex, &allocCount, t, allocsPerThread]() {
            for (int i = 0; i < allocsPerThread; ++i) {
                void* chunk = pool.allocate();
                if (chunk) {
                    MyClass* obj = new (chunk) MyClass(t * 1000 + i);
                    {
                        std::lock_guard<std::mutex> lock(chunksMutex);
                        sharedChunks.push_back(chunk);
                    }
                    allocCount++;
                }
            }
        });
    }
    
    for (auto& thread : threads) {
        thread.join();
    }
    
    EXPECT_EQ(allocCount, numThreads * allocsPerThread);
    
    // Deallocate all chunks in main thread (different from alloc threads)
    for (void* chunk : sharedChunks) {
        MyClass* obj = static_cast<MyClass*>(chunk);
        obj->~MyClass();
        pool.deallocate(chunk);
    }
}

// Performance tests comparing std::allocator vs ObjectPoolImpl
TEST_F(ObjectPoolImplTest, PerformanceComparison) {
    const int numIterations = 10000;
    
    // Test std::allocator (not make_shared)
    std::allocator<MyClass> stdAllocator;
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < numIterations; ++i) {
        MyClass* obj = stdAllocator.allocate(1);
        stdAllocator.construct(obj, i);
        stdAllocator.destroy(obj);
        stdAllocator.deallocate(obj, 1);
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto stdAllocTime = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    
    // Test ObjectPoolImpl
    ObjectPoolImpl pool(sizeof(MyClass), numIterations, numIterations / 10);
    start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < numIterations; ++i) {
        void* chunk = pool.allocate();
        if (chunk) {
            MyClass* obj = new (chunk) MyClass(i);
            obj->~MyClass();
            pool.deallocate(chunk);
        }
    }
    end = std::chrono::high_resolution_clock::now();
    auto poolAllocTime = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    
    // Output performance info (not a failure condition)
    std::cout << "std::allocator time: " << stdAllocTime << " μs\n";
    std::cout << "ObjectPoolImpl time: " << poolAllocTime << " μs\n";
    
    // Both should complete successfully
    EXPECT_GT(stdAllocTime, 0);
    EXPECT_GT(poolAllocTime, 0);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
