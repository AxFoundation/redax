#ifndef _STRAXINSERTER_HH_
#define _STRAXINSERTER_HH_

#include <cstdlib>
#include <cstdint>
#include <string>
#include <map>
#include <mutex>
#include <experimental/filesystem>
#include <numeric>
#include <atomic>
#include <vector>
#include <thread>
#include <condition_variable>
#include <list>
#include <memory>
#include <string_view>
#include <chrono>
#include <functional>

class Options;
class MongoLog;
class V1724;

struct data_packet{
  data_packet() : clock_counter(0), header_time(0) {}
  data_packet(std::u32string s, uint32_t ht, long cc) :
      buff(std::move(s)), clock_counter(cc), header_time(ht) {}
  data_packet(const data_packet& rhs)=delete;
  data_packet(data_packet&& rhs) : buff(std::move(rhs.buff)),
      clock_counter(rhs.clock_counter), header_time(rhs.header_time), digi(rhs.digi) {}
  ~data_packet() {buff.clear(); digi.reset();}

  data_packet& operator=(const data_packet& rhs)=delete;
  data_packet& operator=(data_packet&& rhs) {
    buff=std::move(rhs.buff);
    clock_counter=rhs.clock_counter;
    header_time=rhs.header_time;
    digi=rhs.digi;
    return *this;
  }

  std::u32string buff;
  long clock_counter;
  uint32_t header_time;
  std::shared_ptr<V1724> digi;
};

class StraxFormatter{
  /*
    Reformats raw data into strax format
  */

public:
  StraxFormatter(std::shared_ptr<Options>&, std::shared_ptr<MongoLog>&, const std::map<int, std::vector<std::shared_ptr<V1724>>>&);
  ~StraxFormatter();

  void Close(std::map<int,int>& ret);

  void Process();
  std::pair<int, int> GetBufferSize() {return {fInputBufferSize.load(), fOutputBufferSize.load()};}
  void GetDataPerChan(std::map<int, int>& ret);
  int ReceiveDatapackets(std::list<std::unique_ptr<data_packet>>&, int);

private:
  void ProcessDatapacket(std::unique_ptr<data_packet> dp);
  int ProcessEvent(std::u32string_view, const std::unique_ptr<data_packet>&,
      std::map<int, int>&);
  int ProcessChannel(std::u32string_view, int, int, uint32_t, int&, int,
      const std::unique_ptr<data_packet>&, std::map<int, int>&);
  void WriteOutChunk(int);
  void WriteOutChunks();
  void End();
  void AddFragmentToBuffer(std::string, uint32_t, int);
  std::vector<std::string> GetChunkNames(int);

  std::experimental::filesystem::path GetFilePath(const std::string&, bool=false);
  std::experimental::filesystem::path GetDirectoryPath(const std::string&, bool=false);
  std::string GetStringFormat(int id);
  void CreateEmpty(int);
  int fEmptyVerified;

  std::function<long(std::shared_ptr<std::string>&, std::shared_ptr<std::string>&, long&)> fCompressor;
  int64_t fChunkLength; // ns
  int64_t fChunkOverlap; // ns
  int fFragmentBytes;
  int fStraxHeaderSize; // bytes
  int fFullFragmentSize;
  int fBufferNumChunks;
  int fWarnIfChunkOlderThan;
  int fRunNumber;
  unsigned fChunkNameLength;
  int64_t fFullChunkLength;
  std::string fOutputPath, fHostname, fFullHostname;
  std::shared_ptr<MongoLog> fLog;
  std::atomic_bool fActive;
  std::map<int, std::list<std::string>> fChunks, fOverlaps;
  std::map<int, int> fFailCounter;
  std::map<int, int> fDataPerChan;
  std::mutex fDPC_mutex;
  std::map<int, long> fBufferCounter;
  std::map<int, long> fFragsPerEvent;
  std::map<int, long> fEvPerDP;
  std::map<int, long> fBytesPerChunk;
  std::atomic_int fInputBufferSize, fOutputBufferSize;
  long fBytesProcessed;
  std::map<int, std::vector<int>> fChannelMap;

  double fProcTimeDP, fProcTimeEv, fProcTimeCh, fCompTime;
  std::thread::id fThreadId;
  std::condition_variable fCV;
  std::mutex fBufferMutex;
  std::list<std::unique_ptr<data_packet>> fBuffer;
  std::vector<int> fMutexWaitTime;
};

#endif
