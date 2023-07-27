#pragma once

#include "sync_atomic_signal.h"
#include "lockfree/concurrentqueue/concurrentqueue.h"

namespace dxvk::sync {

  /**
   * \brief A fast lockfree memory pool to handle transient data and objects.
   * It's up to the user to ensure pointers are given back so they can be reused.
   * It's a fixed-size pool which saves one copy/move operation when compared
   * to using emplace + memcpy, which may be significant for larger objects.
   */
  template<typename T>
  class MemoryPool {

  public:

    MemoryPool(uint64_t size);
    ~MemoryPool();

    T* acquire();
    void release( T* entry );

  private:

    typedef moodycamel::ConcurrentQueue
      <T*, moodycamel::ConcurrentQueueDefaultTraits> lockfree_queue_t;

    uint64_t m_size;
    lockfree_queue_t* m_queue;
    T* m_entries;
    AtomicSignal m_sync = { "memory_pool", false };

  };



  template<typename T>
  inline MemoryPool<T>::MemoryPool(uint64_t size)
  : m_size(size) {
    m_queue = new lockfree_queue_t(size);
    m_entries = new T[size];
    for (size_t i=0; i<size; ++i) {
      m_queue->enqueue( &m_entries[i] );
    }
  }


  template<typename T>
  inline MemoryPool<T>::~MemoryPool() {
    delete m_queue;
    delete m_entries;
  }


  template<typename T>
  inline T* MemoryPool<T>::acquire() {
    T* res;

    while (!m_queue->try_dequeue(res)) {
      m_sync.wait();
    }

    return res;
  }


  template<typename T>
  inline void MemoryPool<T>::release(T* entry) {
//    if (!( entry >= m_entries && entry < &m_entries[m_size] )
//      Logger::err("MemoryPool::release() got a wrong pointer!");

    // assumes enqueue() always returns true, which should be the case here
    // if MemoryPool::release() is used in the right way
    m_queue->enqueue(entry);
    m_sync.signal_one();
  }

}
