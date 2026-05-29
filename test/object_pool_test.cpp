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

#include "object_pool.hpp"

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
};

// Test fixture for ObjectPool tests
class ObjectPoolTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Default setup
    }
    
    void TearDown() override {
        // Cleanup
    }
};

// Basic functionality tests
TEST_F(ObjectPoolTest, BasicAllocDealloc) {
    ObjectPool<sizeof(MyClass)> pool(100, 10);
    std::shared_ptr<MyClass> obj;
    
    pool.alloc(obj);
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(obj->int32, 42);
    
    // Object should be automatically deallocated when shared_ptr goes out of scope
}

TEST_F(ObjectPoolTest, AllocWithArgs) {
    ObjectPool<sizeof(MyClass)> pool(100, 10);
    std::shared_ptr<MyClass> obj;
    
    pool.alloc(obj, 123);
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(obj->int32, 123);
    EXPECT_EQ(obj->int64Array[0], 123);
}

TEST_F(ObjectPoolTest, MultipleAllocs) {
    ObjectPool<sizeof(MyClass)> pool(10, 2);
    std::vector<std::shared_ptr<MyClass>> objects;
    
    for (int i = 0; i < 10; ++i) {
        std::shared_ptr<MyClass> obj;
        pool.alloc(obj, i);
        ASSERT_NE(obj, nullptr);
        EXPECT_EQ(obj->int32, i);
        objects.push_back(obj);
    }
    
    // All objects should be valid
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(objects[i]->int32, i);
    }
}

TEST_F(ObjectPoolTest, PoolExhaustionFallback) {
    // Create a pool with capacity 2
    ObjectPool<sizeof(MyClass)> pool(2, 1);
    std::vector<std::shared_ptr<MyClass>> objects;
    
    // Allocate 5 objects (more than capacity)
    // The pool should fallback to std allocator for excess
    for (int i = 0; i < 5; ++i) {
        std::shared_ptr<MyClass> obj;
        pool.alloc(obj, i);
        ASSERT_NE(obj, nullptr);
        EXPECT_EQ(obj->int32, i);
        objects.push_back(obj);
    }
}

TEST_F(ObjectPoolTest, ObjectSizeCheck) {
    // This should compile - MyClass fits in pool
    ObjectPool<sizeof(MyClass)> pool1(10, 2);
    std::shared_ptr<MyClass> obj1;
    pool1.alloc(obj1);
    EXPECT_NE(obj1, nullptr);
    
    // Pool with smaller size should still work for smaller objects
    struct SmallClass {
        int x;
    };
    ObjectPool<sizeof(SmallClass)> pool2(10, 2);
    std::shared_ptr<SmallClass> obj2;
    pool2.alloc(obj2);
    EXPECT_NE(obj2, nullptr);
}

TEST_F(ObjectPoolTest, ReuseAfterDealloc) {
    ObjectPool<sizeof(MyClass)> pool(5, 2);
    
    {
        std::shared_ptr<MyClass> obj1;
        pool.alloc(obj1, 100);
        ASSERT_NE(obj1, nullptr);
        EXPECT_EQ(obj1->int32, 100);
        // obj1 goes out of scope here, should be returned to pool
    }
    
    // Allocate again - should reuse the freed chunk
    std::shared_ptr<MyClass> obj2;
    pool.alloc(obj2, 200);
    ASSERT_NE(obj2, nullptr);
    EXPECT_EQ(obj2->int32, 200);
}

// Multithreaded tests
TEST_F(ObjectPoolTest, MultithreadedSameThreadAllocDealloc) {
    ObjectPool<sizeof(MyClass)> pool(1000, 100);
    const int numThreads = 4;
    const int allocsPerThread = 1000;
    std::vector<std::thread> threads;
    std::atomic<int> successCount{0};
    
    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([&pool, allocsPerThread, &successCount, t]() {
            for (int i = 0; i < allocsPerThread; ++i) {
                std::shared_ptr<MyClass> obj;
                pool.alloc(obj, t * 1000 + i);
                if (obj && obj->int32 == t * 1000 + i) {
                    successCount++;
                }
                // obj automatically deallocated here
            }
        });
    }
    
    for (auto& thread : threads) {
        thread.join();
    }
    
    EXPECT_EQ(successCount, numThreads * allocsPerThread);
}

TEST_F(ObjectPoolTest, MultithreadedCrossThreadDealloc) {
    ObjectPool<sizeof(MyClass)> pool(1000, 100);
    const int numThreads = 4;
    const int allocsPerThread = 100;
    std::vector<std::thread> threads;
    std::vector<std::shared_ptr<MyClass>> sharedObjects;
    std::mutex objectsMutex;
    std::atomic<int> allocCount{0};
    
    // Allocate in multiple threads
    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([&pool, &sharedObjects, &objectsMutex, &allocCount, t, allocsPerThread]() {
            for (int i = 0; i < allocsPerThread; ++i) {
                std::shared_ptr<MyClass> obj;
                pool.alloc(obj, t * 1000 + i);
                if (obj) {
                    {
                        std::lock_guard<std::mutex> lock(objectsMutex);
                        sharedObjects.push_back(obj);
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
    
    // Clear objects (dealloc in main thread, different from alloc threads)
    sharedObjects.clear();
}

// Performance tests (informational, not asserting on performance)
TEST_F(ObjectPoolTest, PerformanceComparison) {
    const int numIterations = 10000;
    
    // Test standard allocator
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < numIterations; ++i) {
        auto obj = std::make_shared<MyClass>(i);
        (void)obj;
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto stdAllocTime = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    
    // Test object pool
    ObjectPool<sizeof(MyClass)> pool(numIterations, numIterations / 10);
    start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < numIterations; ++i) {
        std::shared_ptr<MyClass> obj;
        pool.alloc(obj, i);
        (void)obj;
    }
    end = std::chrono::high_resolution_clock::now();
    auto poolAllocTime = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    
    // Output performance info (not a failure condition)
    std::cout << "Standard allocator time: " << stdAllocTime << " μs\n";
    std::cout << "Object pool time: " << poolAllocTime << " μs\n";
    
    // Pool should generally be faster or comparable (but don't fail test on this)
    // Just ensure both completed successfully
    EXPECT_GT(stdAllocTime, 0);
    EXPECT_GT(poolAllocTime, 0);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
