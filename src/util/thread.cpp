#include <atomic>

#include "thread.h"
#include "util_likely.h"
#include "log/log.h"
#include "util_time.h"
#include "../dxvk/hud/dxvk_hud_item.h"

std::atomic<const char*> dxvk::hud::HudDebugStallsItem::m_name = {   "" };
std::atomic<uint64_t>    dxvk::hud::HudDebugStallsItem::m_us   = { 0ull };
std::atomic<uint64_t>    dxvk::hud::HudDebugStallsItem::m_timestamp_ms = { 0ull };
dxvk::high_resolution_clock::time_point dxvk::hud::HudDebugStallsItem::m_startTime = dxvk::high_resolution_clock::now();

#ifdef _WIN32

namespace dxvk {

  static const uint64_t logging_threshold = 20;
  static const uint64_t print_hud_threshold = 100;
  static auto start = dxvk::high_resolution_clock::now();
  std::string getTimestamp() {
    auto t = dxvk::high_resolution_clock::now();
    uint64_t duration = std::chrono::duration_cast<std::chrono::milliseconds>(t - start).count();
    uint64_t s = duration / 1000;
    uint64_t ms = duration % 1000;

    auto str_ms = std::to_string(ms);
    if( ms < 10 ) str_ms.insert(0, "00");
    else if( ms < 100 ) str_ms.insert(0, "0");

    return std::to_string(s) + "." + str_ms + ": ";
  }


  void mutex::lock() {
    auto t0 = dxvk::high_resolution_clock::now();

    AcquireSRWLockExclusive(&m_lock);

    auto t1 = dxvk::high_resolution_clock::now();
    m_timeToGetLock  = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

    if( m_timeToGetLock > print_hud_threshold && m_timeToGetLock > dxvk::hud::HudDebugStallsItem::m_us ) {
      dxvk::hud::HudDebugStallsItem::m_name = m_name;
      dxvk::hud::HudDebugStallsItem::m_us   = m_timeToGetLock;

      auto& s = dxvk::hud::HudDebugStallsItem::m_startTime;
      uint64_t duration = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - s).count();
      dxvk::hud::HudDebugStallsItem::m_timestamp_ms = duration;
    }
  }


  void mutex::unlock() {
    ReleaseSRWLockExclusive(&m_lock);

    if( m_timeToGetLock > logging_threshold && strcmp(m_name, "log") ) {
      Logger::debug( getTimestamp() + std::string(m_name) + " aquire mutex lock did take "
        + std::to_string(m_timeToGetLock) + std::string(" us "));
    }
  }


  void recursive_mutex::lock() {
    auto t0 = dxvk::high_resolution_clock::now();

    EnterCriticalSection(&m_lock);

    auto t1 = dxvk::high_resolution_clock::now();
    m_timeToGetLock = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
  }


  void recursive_mutex::unlock() {
    LeaveCriticalSection(&m_lock);

    if( m_timeToGetLock > logging_threshold ) {
    Logger::debug( getTimestamp() + std::string(m_name) + " aquire recursive_mutex lock did take "
        + std::to_string(m_timeToGetLock) + std::string(" us "));
    }
  }



  thread::thread(ThreadProc&& proc)
  : m_data(new ThreadData(std::move(proc))) {
    m_data->handle = ::CreateThread(nullptr, 0x100000,
      thread::threadProc, m_data, STACK_SIZE_PARAM_IS_A_RESERVATION,
      &m_data->id);

    if (!m_data->handle) {
      delete m_data;
      throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again), "Failed to create thread");
    }
  }


  thread::~thread() {
    if (joinable())
      std::terminate();
  }


  void thread::join() {
    if (!joinable())
      throw std::system_error(std::make_error_code(std::errc::invalid_argument), "Thread not joinable");

    if (get_id() == this_thread::get_id())
      throw std::system_error(std::make_error_code(std::errc::resource_deadlock_would_occur), "Cannot join current thread");

    if(::WaitForSingleObjectEx(m_data->handle, INFINITE, FALSE) == WAIT_FAILED)
      throw std::system_error(std::make_error_code(std::errc::invalid_argument), "Joining thread failed");

    detach();
  }


  void thread::set_priority(ThreadPriority priority) {
    int32_t value;
    switch (priority) {
      default:
      case ThreadPriority::Normal: value = THREAD_PRIORITY_NORMAL; break;
      case ThreadPriority::Lowest: value = THREAD_PRIORITY_LOWEST; break;
    }

    if (m_data)
      ::SetThreadPriority(m_data->handle, int32_t(value));
  }


  uint32_t thread::hardware_concurrency() {
    SYSTEM_INFO info = { };
    ::GetSystemInfo(&info);
    return info.dwNumberOfProcessors;
  }


  DWORD WINAPI thread::threadProc(void* arg) {
    auto data = reinterpret_cast<ThreadData*>(arg);
    DWORD exitCode = 0;

    try {
      data->proc();
    } catch (...) {
      exitCode = 1;
    }

    data->decRef();
    return exitCode;
  }

}


namespace dxvk::this_thread {

  bool isInModuleDetachment() {
    using PFN_RtlDllShutdownInProgress = BOOLEAN (WINAPI *)();

    static auto RtlDllShutdownInProgress = reinterpret_cast<PFN_RtlDllShutdownInProgress>(
      ::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "RtlDllShutdownInProgress"));

    return RtlDllShutdownInProgress();
  }

}

#else

namespace dxvk::this_thread {
  
  static std::atomic<uint32_t> g_threadCtr = { 0u };
  static thread_local uint32_t g_threadId  = 0u;
  
  // This implementation returns thread ids unique to the current instance.
  // ie. if you use this across multiple .so's then you might get conflicting ids.
  //
  // This isn't an issue for us, as it is only used by the spinlock implementation,
  // but may be for you if you use this elsewhere.
  uint32_t get_id() {
    if (unlikely(!g_threadId))
      g_threadId = ++g_threadCtr;

    return g_threadId;
  }

}

#endif
