#pragma once

#include <ctime>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <vector>
#include <mutex>
#include <assert.h>

//#define BENCH_SCOPE(x) void()
#define BENCH_SCOPE(x) BenchmarkScope name(x)


class Benchmark {

public:

  enum Flags : uint8_t {

    collectOverTimeStats    = 0b10000000,
    collectOutliers         = 0b01000000,
    collectDistribution     = 0b00100000,
    printWhenLowSampleCount = 0b00001000,
    printEmptyStatementTime = 0b00000100,
    continuousWrite         = 0b00000001

  };

  constexpr static uint8_t defaultFlags = 0b00000000;

  Benchmark( const char* name, uint8_t flags = defaultFlags )
  : m_name(name), m_flags(flags) {

    m_totalTime.store(0);
    m_startTimestamp = std::chrono::high_resolution_clock::now();
    for( size_t i=0; i<m_data.size(); ++i )
      m_data[i].store(0);

    if( m_flags & Flags::continuousWrite ) {
      m_file = std::ofstream( m_name, std::ios::app );
      printHeader( m_file );
    }

  }

  ~Benchmark() {
    if( !(m_flags & Flags::continuousWrite) ) {
      m_file = std::ofstream( m_name, std::ios::app );
      printResults(m_file);
    } else {
      if( (m_flags & Flags::printWhenLowSampleCount) || (getSampleCount() >= 10) ) {
        printResults(m_file);
        m_file << "benchmark did close properly" << std::endl;
      }
    }
    m_file.close();
  }

  typedef std::chrono::time_point<std::chrono::high_resolution_clock> time_point_t;

  static time_point_t getTime()
    { return std::chrono::high_resolution_clock::now(); }

  static uint64_t getDuration( const time_point_t& t0, const time_point_t& t1 )
    { return std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count(); }

  void registerMeasurement( const time_point_t& t0, const time_point_t& t1, const time_point_t& t2 ) {

    uint64_t emptyStatementTime = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    uint64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();
    m_totalTime += ns;
    m_totalTimeES += emptyStatementTime;

    if( m_flags & Flags::collectDistribution ) {
      if( ns < 1000 ) m_data[ns/100]++;                       // 0-9
      else if( ns < 10000 ) m_data[9 + ns/1000]++;            // 10-18
      else if( ns < 100000 ) m_data[18 + ns/10000]++;         // 19-27
      else if( ns < 1000000 ) m_data[27 + ns/100000]++;       // 28-36
      else m_data[37]++;
    } else {
      m_numSamples++;
    }

    if( m_flags & Flags::collectOutliers ) {
      Outlier event = { .timeDuration = ns/1000, .timestamp = getTimestamp() };
      m_outliersMutex.lock();
      m_outliers.push_back(event);
      m_outliersMutex.unlock();
    }

    if( m_flags & Flags::collectOverTimeStats ) {
      uint64_t second = std::chrono::duration_cast<std::chrono::seconds>(t1 - m_startTimestamp).count();
      if( second < m_eventsPerSecond.size() ) {
        m_eventsPerSecond[second].totalTime += ns;
        m_eventsPerSecond[second].numEvents++;
      }
    }

    if( m_flags & Flags::continuousWrite ) {
      if( m_numSamples % (1 << 20) == 0 ) {
        printResults(m_file);
      }
    }
  }

private:

  std::string getTimestamp() {

    auto t = std::chrono::high_resolution_clock::now();
    uint64_t duration = std::chrono::duration_cast<std::chrono::milliseconds>(t - m_startTimestamp).count();
    uint64_t s = duration / 1000;
    uint64_t ms = duration % 1000;

    auto str_ms = std::to_string(ms);
    if( ms < 10 ) str_ms.insert(0, "00");
    else if( ms < 100 ) str_ms.insert(0, "0");

    return std::to_string(s) + "." + str_ms + ": ";

  }

  uint64_t getSampleCount() {

    if( m_flags & Flags::collectDistribution ) {
      uint64_t count = 0;
      for( size_t i=0; i<m_data.size(); ++i )
        count += m_data[i];
      count += m_outliers.size();
      return count;
    } else {
      return m_numSamples;
    }

  }

  uint64_t mapArrayIndexToNanoSeconds( uint8_t index ) {

    assert( index < m_data.size() );

    if( index < 10 ) return 100*index;          // 0-9
    if( index < 19 ) return 1000*(index-9);     // 10-18
    if( index < 28 ) return 10000*(index-18);   // 19-27
    return 100000*(index-27);                   // 28-36 (37)

  }

  void printHeader( std::ofstream& file ) {

    auto now = std::chrono::system_clock::now();
    auto now_in_time_t = std::chrono::system_clock::to_time_t(now);

    file << std::endl << std::endl << std::endl;
    file << std::put_time(std::localtime(&now_in_time_t), "%Y-%m-%d %X") << std::endl;

  }

  void printResults( std::ofstream& file ) {

    uint64_t sampleCount = getSampleCount();

    if( !(m_flags & Flags::printWhenLowSampleCount) && (sampleCount < 10) )
      return;

    if( !(m_flags & Flags::continuousWrite) )
      printHeader( file );

    uint64_t totalTime = m_totalTime - m_totalTimeES;
    float avgRuntime = totalTime / ((float) sampleCount * 1000.0f);
    float avgRuntimeES = m_totalTimeES / ((float) sampleCount * 1000.0f);

    file << "benchmark " << m_name << " collected " << sampleCount << " samples " << std::endl;
    file << "avg runtime per call : " << std::setprecision(3) << avgRuntime << " us" << std::endl;
    file << "total time taken : " << std::setprecision(3) << totalTime / 1000000000.0f << " seconds" << std::endl;

    if( m_flags & Flags::printEmptyStatementTime )
      file << "avg empty statement per call : " << std::setprecision(3) << avgRuntimeES << " us (got subtracted)" << std::endl;

    file << std::endl;

    if( m_flags & Flags::collectDistribution ) {

      file << "   " << 0 << " ns: #" << m_data[0] << std::endl;
      for( size_t i=1; i<10; ++i )
        file << " " << i*100 << " ns: #" << m_data[i] << std::endl;

      file << std::endl;

      for( size_t i=1; i<10; ++i )
        file << "   " << i << " us: #" << m_data[9+i] << std::endl;

      file << std::endl;

      for( size_t i=1; i<10; ++i )
        file << "  " << i*10 << " us: #" << m_data[18+i] << std::endl;

      file << std::endl;

      for( size_t i=1; i<10; ++i )
        file << " " << i*100 << " us: #" << m_data[27+i] << std::endl;

      file << std::endl;
      file << " 1 ms+ : #" << m_data[37] << std::endl;

    }

    if( !m_outliers.empty() ) {

      file << std::endl;
      file << "Also the following outliers were collected:" << std::endl;
      for( Outlier outlier : m_outliers )
        file << outlier.timestamp << ": " << outlier.timeDuration << " us" << std::endl;

    }

    if( m_flags & Flags::collectOverTimeStats ) { // todo: handle file better

      auto now = std::chrono::system_clock::now();
      auto now_in_time_t = std::chrono::system_clock::to_time_t(now);

      file = std::ofstream( (std::string("eps_") + m_name).c_str(), std::ios::app );
      file << std::endl;
      file << std::put_time(std::localtime(&now_in_time_t), "%Y-%m-%d %X") << std::endl;
      for( size_t i=0; i<m_eventsPerSecond.size(); ++i ) {
        GroupedEvents& events = m_eventsPerSecond[i];
        if( events.numEvents == 0 )
          continue;

        float avg = events.totalTime/(1000.0f*events.numEvents);
        file << i << " avg " << std::setprecision(3) << avg << " us for " << events.numEvents << " calls" << std::endl;
      }

      file.close();

    }
  }

  struct Outlier {
    uint64_t timeDuration;
    std::string timestamp;
  };

  struct GroupedEvents {
    GroupedEvents() {
      totalTime.store(0);
      numEvents.store(0);
    }

    std::atomic<uint64_t> totalTime;
    std::atomic<uint64_t> numEvents;
  };

  const char*  m_name;
  uint32_t     m_flags;
  time_point_t m_startTimestamp;

  std::array< std::atomic<uint64_t>, 38 > m_data;
  std::atomic<uint64_t> m_numSamples;
  std::vector< Outlier > m_outliers;
  std::mutex m_outliersMutex;

  std::atomic< uint64_t > m_totalTime;
  std::atomic< uint64_t > m_totalTimeES;

  std::array< GroupedEvents, 1000 > m_eventsPerSecond;

  std::ofstream m_file;

};


class BenchmarkScope {
public:

  BenchmarkScope( Benchmark& benchmark )
  : m_benchmark(benchmark) {

    m_t0 = Benchmark::getTime();
    m_t1 = Benchmark::getTime();

  }

  ~BenchmarkScope() {

    m_t2 = Benchmark::getTime();
    m_benchmark.registerMeasurement(m_t0, m_t1, m_t2);

  }

private:

  Benchmark& m_benchmark;
  Benchmark::time_point_t m_t0;
  Benchmark::time_point_t m_t1;
  Benchmark::time_point_t m_t2;

};
