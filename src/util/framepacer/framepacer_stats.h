#pragma once
#include <atomic>


/*
 *  note: simple modulo hash mapping is used for frameIds
 *        they are expected to monotonically increase by one
 */


namespace dxvk {


  class FrameStatsStorage;


  struct FrameStats {

    uint32_t frametime;

    uint32_t appThreadLatency;
    uint32_t submitLatency;
    uint32_t finishedLatency;
    uint32_t presentLatency;

    high_resolution_clock::time_point start;
    high_resolution_clock::time_point end;

    uint64_t presentId;

  };


  class FrameStatsReader {

  public:

    FrameStatsReader( const FrameStatsStorage* storage, uint32_t numEntries );
    bool getNext( FrameStats& result );
    uint32_t getNumStatsAvailable();

  private:

    const FrameStatsStorage* m_storage;
    uint64_t m_index;

  };


  class FrameStatsStorage {

    friend class FrameStatsReader;

  public:

    FrameStatsStorage() {

    m_producerIndex.store(0);
    m_stats = new FrameStats[m_numStats]();

  }


  ~FrameStatsStorage() {

    delete[] m_stats;

  }


  FrameStatsReader getReader( uint32_t numEntries ) {

    return FrameStatsReader(this, numEntries);

  }


  void registerFrameStart( uint64_t presentId ) {

    auto t = high_resolution_clock::now();

    FrameStats* stats = getStats(presentId);
    stats->start = t;
    stats->presentId = presentId;

  }


  void registerAppThreadStartsSubmitting( uint64_t presentId ) {

    auto t = high_resolution_clock::now();

    FrameStats* stats = getStats(presentId);
    stats->appThreadLatency = getDuration(t, stats->start);

  }


  void registerSubmitFinished( uint64_t presentId ) {

    auto t = high_resolution_clock::now();

    FrameStats* stats = getStats(presentId);
    stats->submitLatency = getDuration(t, stats->start);

  }


  void registerGPUFinished( uint64_t presentId ) {

    auto t = high_resolution_clock::now();

    FrameStats* stats = getStats(presentId);
    stats->finishedLatency = getDuration(t, stats->start);

  }


  void registerFrameEnd( uint64_t presentId ) {

    auto t = high_resolution_clock::now();

    FrameStats* stats = getStats(presentId);
    stats->presentLatency = getDuration(t, stats->start);
    stats->end = t;

    FrameStats* stats_prev = getStats(presentId-1);
    stats->frametime = getDuration(t, stats_prev->end);

    m_producerIndex.store( presentId );

  }


  high_resolution_clock::time_point getFrameStart( uint64_t presentId ) const {

    return m_stats[presentId % m_numStats].start;

  }


  high_resolution_clock::time_point getFrameEnd( uint64_t presentId ) const {

    return m_stats[presentId % m_numStats].end;

  }


  private:

    FrameStats* getStats( uint64_t presentId ) {

      return &m_stats[presentId % m_numStats];

    }


    static uint32_t getDuration( high_resolution_clock::time_point t0, high_resolution_clock::time_point t1 ) {

      uint64_t duration = std::chrono::duration_cast<std::chrono::microseconds>(t0 - t1).count();

      // we get huge values for the first frame, set them to 0
      if (duration > 100'000'000)
        duration = 0;

      return duration;

    }


    // select the ringbuffer large enough, so we never come into a
    // situation where the reader cannot keep up with the producer
    FrameStats* m_stats;
    static constexpr uint16_t m_numStats = 512;
    std::atomic<uint64_t> m_producerIndex;

  };



  inline FrameStatsReader::FrameStatsReader( const FrameStatsStorage* storage, uint32_t numEntries )
  : m_storage(storage) {

    m_index = 0;
    if (m_storage->m_producerIndex.load() > numEntries)
      m_index = m_storage->m_producerIndex.load() - numEntries;

  }


  inline bool FrameStatsReader::getNext( FrameStats& result ) {

    uint64_t producerIndex = m_storage->m_producerIndex.load();

    if (m_index == producerIndex)
      return false;

    result = m_storage->m_stats[m_index % m_storage->m_numStats];
    m_index++;

    return true;

  }


  inline uint32_t FrameStatsReader::getNumStatsAvailable() {

    return m_storage->m_producerIndex.load() - m_index;

  }



  extern std::unique_ptr<FrameStatsStorage> g_frameStatsStorage;

}