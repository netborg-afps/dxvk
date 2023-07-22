#include "sync_recursive.h"
#include "sync_spinlock.h"

#include "util_time.h"
#include "log/log.h"

namespace dxvk::sync {

  void RecursiveSpinlock::lock() {
      auto t0 = dxvk::high_resolution_clock::now();

      spin(2000, [this] { return try_lock(); });

      auto t1 = dxvk::high_resolution_clock::now();
      m_timeToGetLock  = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

  }


  void RecursiveSpinlock::unlock() {
    if (likely(m_counter == 0))
      m_owner.store(0, std::memory_order_release);
    else
      m_counter -= 1;

    if( m_timeToGetLock > 10 )
      Logger::debug( std::string(m_name) + " aquire recursive_spinlock did take " + std::to_string(m_timeToGetLock) + std::string(" us "));
  }


  bool RecursiveSpinlock::try_lock() {
    uint32_t threadId = dxvk::this_thread::get_id();
    uint32_t expected = 0;

    bool status = m_owner.compare_exchange_weak(
      expected, threadId, std::memory_order_acquire);
    
    if (status)
      return true;
    
    if (expected != threadId)
      return false;
    
    m_counter += 1;
    return true;
  }

}
