/*
 * object_pool_impl.hpp
 *
 *  Created on: May 29, 2026
 *      Author: Ruslan Mardugalliamov
 */

#pragma once

#include <cstddef>

class ObjectPoolImpl {
public:
    ObjectPoolImpl(std::size_t chunkSize, std::size_t maxCapacity, std::size_t minCapacity);
    ~ObjectPoolImpl();

    // Returns pointer to chunk data, or nullptr if pool is exhausted
    void* allocate();

    // Returns chunk to pool
    void deallocate(void* ptr);

    // Non-copyable, non-movable
    ObjectPoolImpl(const ObjectPoolImpl&) = delete;
    ObjectPoolImpl& operator=(const ObjectPoolImpl&) = delete;
    ObjectPoolImpl(ObjectPoolImpl&&) = delete;
    ObjectPoolImpl& operator=(ObjectPoolImpl&&) = delete;

private:
    struct Impl;
    Impl* pImpl;
};
