/*
 * object_pool.hpp
 *
 *  Created on: Oct 10, 2017
 *      Author: Ruslan Mardugalliamov
 */

#pragma once

#include <cstddef>
#include <memory>
#include <utility>

#include "object_pool_impl.hpp"

template<std::size_t MAX_OBJ_SIZE>
class ObjectPool {

public:
    ObjectPool (std::size_t maxCapacity, std::size_t minCapacity)
        : impl(MAX_OBJ_SIZE, maxCapacity, minCapacity)
    {
    }

    ~ObjectPool() = default;

    template<typename OBJECT, typename ... ARGS>
    void alloc (std::shared_ptr<OBJECT> & object, ARGS&& ... args){
        static_assert(sizeof(OBJECT) <= MAX_OBJ_SIZE, "Object size exceeds pool chunk size");
        
        void* chunk = impl.allocate();
        if (!chunk) {
            allocateUsingStdAllocator(object, std::forward<ARGS>(args) ...);
            return;
        }

        object = std::shared_ptr<OBJECT>( new (chunk) OBJECT(std::forward<ARGS>(args) ... )
                                          , [this](OBJECT* pObject) {this->dealloc(pObject);}
        );
    }

    template<typename OBJECT>
    void dealloc(OBJECT* object){
        object->~OBJECT();
        impl.deallocate(object);
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

private:
    ObjectPoolImpl impl;
};
