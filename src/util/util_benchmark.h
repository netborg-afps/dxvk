#pragma once

#include "util_time.h"
#include <fstream>
#include <iostream>

class Benchmark {

public:

  Benchmark( const char* name ) : m_name(name) {

    m_startTimestamp = dxvk::high_resolution_clock::now();
    for( size_t i=0; i<m_data.size(); ++i )
      m_data[i].store(0);

  }

  ~Benchmark() {

    printResults();

  }

  typedef std::chrono::time_point<dxvk::high_resolution_clock> time_point_t;

  const time_point_t startSample() {

    auto t0 = dxvk::high_resolution_clock::now();
    return t0;

  }

  void endSample( const time_point_t& t0 ) {

    auto t1 = dxvk::high_resolution_clock::now();
    uint64_t us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    if( us < 10 ) m_data[us]++;                     // 0-9
    else if( us < 100 ) m_data[9 + us/10]++;        // 10-18
    else if( us < 1000 ) m_data[19 + us/100]++;     // 19-27
    else {
      SingleEvent event = { .timeDuration = us, .timestamp = getTimestamp() };
      m_singleEvents.push_back(event);
    }


  }

private:

  std::string getTimestamp() {

    auto t = dxvk::high_resolution_clock::now();
    uint64_t duration = std::chrono::duration_cast<std::chrono::milliseconds>(t - m_startTimestamp).count();
    uint64_t s = duration / 1000;
    uint64_t ms = duration % 1000;

    auto str_ms = std::to_string(ms);
    if( ms < 10 ) str_ms.insert(0, "00");
    else if( ms < 100 ) str_ms.insert(0, "0");

    return std::to_string(s) + "." + str_ms + ": ";

  }

  uint64_t getSampleCount() {

    uint64_t count = 0;
    for( size_t i=0; i<m_data.size(); ++i )
      count += m_data[i];
    count += m_singleEvents.size();
    return count;

  }


  void printResults() {

    if( getSampleCount() == 0 )
      return;

    std::ofstream file = std::ofstream( m_name, std::ios::app );

    file << std::endl << std::endl << std::endl;
    file << "benchmark " << m_name << " collected " << getSampleCount() << " samples " << std::endl;
    file << std::endl;

    for( size_t i=0; i<10; ++i )
      file << "   " << i << " us: #" << m_data[i] << std::endl;

    file << std::endl;

    for( size_t i=1; i<10; ++i )
      file << "  " << i*10 << " us: #" << m_data[9+i] << std::endl;

    file << std::endl;

    for( size_t i=1; i<10; ++i )
      file << " " << i*100 << " us: #" << m_data[18+i] << std::endl;

    if( !m_singleEvents.empty() ) {

      file << std::endl;
      file << "Also the following single samples were collected:" << std::endl;
      for( SingleEvent event : m_singleEvents )
        file << event.timestamp << ": " << event.timeDuration << " us" << std::endl;

    }

    file.close();
  }


  struct SingleEvent {
    uint64_t timeDuration;
    std::string timestamp;
  };

  const char*  m_name;
  time_point_t m_startTimestamp;

  std::array< std::atomic<uint64_t>, 28 > m_data;
  std::vector< SingleEvent > m_singleEvents;

};
