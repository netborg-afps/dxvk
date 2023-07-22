#pragma once

#include <synchapi.h>
#include <atomic>
#include "../log/log.h"

//#define _atomic_signal_debug(x) Logger::debug(x)
#define _atomic_signal_debug(x) void()

namespace dxvk::sync {

  /**
   * \brief WaitOnAddress based sync implementation
   */
  class AtomicSignal {

  public:

    AtomicSignal(const char* name, bool initValue)
    : m_name(name) {
      if (initValue)
        m_flag.test_and_set();
    }

    ~AtomicSignal() {}

    void wait() {
      _atomic_signal_debug( std::string("enter wait for ") + m_name );

      WaitOnAddress(&m_flag, (void*) &m_flagFalse, 1, m_infinite);
      m_flag.clear();

      _atomic_signal_debug( std::string("finish wait for ") + m_name );
    }

    void signal_one() {
      _atomic_signal_debug( std::string("enter signal_one for ") + m_name );

      m_flag.test_and_set();
      WakeByAddressSingle(&m_flag);

      _atomic_signal_debug( std::string("finish signal_one for ") + m_name );
    }

    void signal_all() {
      _atomic_signal_debug( std::string("enter signal_all for ") + m_name );

      m_flag.test_and_set();
      WakeByAddressAll(&m_flag);

      _atomic_signal_debug( std::string("finish signal_all for ") + m_name );
    }

    void clear() {
      _atomic_signal_debug( std::string("clear flag for ") + m_name );
      m_flag.clear();
    }

  private:

    alignas(64) std::atomic_flag        m_flag = ATOMIC_FLAG_INIT;
    const char*                         m_name;

    static constexpr std::atomic_flag   m_flagFalse = ATOMIC_FLAG_INIT;
    static constexpr uint32_t           m_infinite  = 0xffffffff; // matches INFINITE define in winbase.h
  };

}