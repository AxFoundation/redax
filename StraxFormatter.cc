#include "StraxFormatter.hh"
#include "DAQController.hh"
#include "MongoLog.hh"
#include "Options.hh"
#include "V1724.hh"
#include <lz4frame.h>
#include <blosc.h>
#include <thread>
#include <sstream>
#include <bitset>
#include <ctime>
#include <cmath>

namespace fs=std::experimental::filesystem;
using namespace std::chrono;
const int event_header_words = 4, max_channels = 16;

long compress_blosc(std::shared_ptr<std::string>& in, std::shared_ptr<std::string>& out, long& size_in) {
  long max_compressed_size = size_in + BLOSC_MAX_OVERHEAD;
  out = std::make_shared<std::string>(max_compressed_size, 0);
  return blosc_compress_ctx(5, 1, sizeof(char), size_in, in->data(), out->data(), max_compressed_size,"lz4", 0, 2);
}

long compress_lz4(std::shared_ptr<std::string>& in, std::shared_ptr<std::string>& out, long& size_in) {
  // Note: the current package repo version for Ubuntu 18.04 (Oct 2019) is 1.7.1, which is
  // so old it is not tracked on the lz4 github. The API for frame compression has changed
  // just slightly in the meantime. So if you update and it breaks you'll have to tune at least
  // the LZ4F_preferences_t object to the new format.
  // Can tune here as needed, these are defaults from the LZ4 examples
  LZ4F_preferences_t kPrefs = {
    { LZ4F_max256KB, LZ4F_blockLinked, LZ4F_noContentChecksum, LZ4F_frame, 0, { 0, 0 } },
      0,   /* compression level; 0 == default */
      0,   /* autoflush */
      { 0, 0, 0 },  /* reserved, must be set to 0 */
  };
  long max_compressed_size = LZ4F_compressFrameBound(size_in, &kPrefs);
  out = std::make_shared<std::string>(max_compressed_size, 0);
  return LZ4F_compressFrame(out->data(), max_compressed_size, in->data(), size_in, &kPrefs);
}

long compress_none(std::shared_ptr<std::string>& in, std::shared_ptr<std::string>& out, long& size_in) {
  out = in;
  return size_in;
}

long compress_devnull(std::shared_ptr<std::string>&, std::shared_ptr<std::string>&, long& size_in) {
  // this function is why we pass long&, so we can trick the calling function into deleting the data
  // without writing it out first, because the uncompressed size is the determining factor
  size_in = 0;
  return 0;
}

const std::map<std::string, std::function<long(std::shared_ptr<std::string>&, std::shared_ptr<std::string>&, long&)>> compressors = {
  {"blosc", compress_blosc},
  {"lz4", compress_lz4},
  {"none", compress_none},
  {"delete", compress_devnull}
};

StraxFormatter::StraxFormatter(std::shared_ptr<Options>& opts, std::shared_ptr<MongoLog>& log, const std::map<int, std::vector<std::shared_ptr<V1724>>>& digis){
  fActive = true;
  fChunkNameLength=6;
  fStraxHeaderSize=24;
  fBytesProcessed = 0;
  fInputBufferSize = 0;
  fOutputBufferSize = 0;
  fProcTimeDP = fProcTimeEv = fProcTimeCh = fCompTime = 0.;
  fChunkLength = long(opts->GetDouble("strax_chunk_length", 5)*1e9); // default 5s
  fChunkOverlap = long(opts->GetDouble("strax_chunk_overlap", 0.5)*1e9); // default 0.5s
  fFragmentBytes = opts->GetInt("strax_fragment_payload_bytes", 110*2);
  fFullFragmentSize = fFragmentBytes + fStraxHeaderSize;
  try {
    fCompressor = compressors.at(opts->GetString("compressor", "lz4"));
  } catch (...) {
    fLog->Entry(MongoLog::Error, "Invalid compressor specified");
    throw std::runtime_error("Invalid compressor");
  }
  fFullChunkLength = fChunkLength+fChunkOverlap;
  fHostname = opts->Hostname();
  const int run_name_length = 6;
  std::string run_name(run_name_length+1, '\0');
  fRunNumber = opts->GetInt("number", -1);
  if (fRunNumber == -1) run_name = "run";
  else sprintf(run_name.data(), "06d", fRunNumber); // run name length is 6

  // cache channel map
  for (const auto& link : digis) for (const auto& digi: link) for (unsigned ch = 0; ch < digi->GetNumChannels; ++ch)
    fChannelMap[digi->fBID()].push_back(Options->GetChannel(digi->fBID(), ch));

  fEmptyVerified = 0;
  fLog = log;

  fBufferNumChunks = opts->GetInt("strax_buffer_num_chunks", 2);
  fWarnIfChunkOlderThan = opts->GetInt("strax_chunk_phase_limit", 2);
  fMutexWaitTime.reserve(1<<20);

  std::string output_path = opts->GetString("strax_output_path", "./");
  try{
    fs::path op(output_path);
    op /= run_name;
    fOutputPath = op;
    fs::create_directory(op);
  }
  catch(...){
    fLog->Entry(MongoLog::Error, "StraxFormatter::Initialize tried to create output directory but failed. Check that you have permission to write here.");
    throw std::runtime_error("No write permissions");
  }
}

StraxFormatter::~StraxFormatter(){
  if (fMutexWaitTime.size() > 0) {
    fLog->Entry(MongoLog::Local, "Thread %lx mutex report: min %i max %i mean %i median %i num %i",
        fThreadId, fMutexWaitTime.front(), fMutexWaitTime.back(),
        std::accumulate(fMutexWaitTime.begin(), fMutexWaitTime.end(), 0l)/fMutexWaitTime.size(),
        fMutexWaitTime[fMutexWaitTime.size()/2], fMutexWaitTime.size());
  }
}

void StraxFormatter::Close(std::map<int,int>& ret){
  fActive = false;
  for (auto& iter : fFailCounter) ret[iter.first] += iter.second;
  fCV.notify_one();
}

void StraxFormatter::GetDataPerChan(std::map<int, int>& ret) {
  if (!fActive) return;
  const std::lock_guard<std::mutex> lk(fDPC_mutex);
  for (auto& pair : fDataPerChan) {
    ret[pair.first] += pair.second;
    pair.second = 0;
  }
  return;
}

void StraxFormatter::ProcessDatapacket(std::unique_ptr<data_packet> dp){
  // Take a buffer and break it up into one document per channel
  auto it = dp->buff.begin();
  int evs_this_dp(0), words(0);
  bool missed = false;
  std::map<int, int> dpc;
  do {
    if((*it)>>28 == 0xA){
      missed = true; // it works out
      words = (*it)&0xFFFFFFF;
      std::u32string_view sv(dp->buff.data() + std::distance(dp->buff.begin(), it), words);
      // std::u32string_view sv(it, it+words); //c++20 :(
      ProcessEvent(sv, dp, dpc);
      evs_this_dp++;
      it += words;
    } else {
      if (missed) {
        fLog->Entry(MongoLog::Warning, "Missed an event from %i at idx %x/%x (%x)",
            dp->digi->bid(), std::distance(dp->buff.begin(), it), dp->buff.size(), *it);
        missed = false;
        // this happens quite rarely, the chance of overwriting ourselves is vanishing
        // but it's nice to be able to know why we missed an event
        std::string filename = std::to_string(fRunNumber) + "_missed";
        std::ofstream fout(filename, std::ios::out | std::ios::binary);
        fout.write((char*)dp->buff.data(), dp->buff.size()*sizeof(dp->buff[0]));
        fout.close();
      }
      it++;
    }
  } while (it < dp->buff.end() && fActive == true);
  fBytesProcessed += dp->buff.size()*sizeof(char32_t);
  fEvPerDP[evs_this_dp]++;
  {
    const std::lock_guard<std::mutex> lk(fDPC_mutex);
    for (auto& p : dpc) fDataPerChan[p.first] += p.second;
  }
  fInputBufferSize -= dp->buff.size()*sizeof(char32_t);
}

int StraxFormatter::ProcessEvent(std::u32string_view buff,
    const std::unique_ptr<data_packet>& dp, std::map<int, int>& dpc) {
  // buff = start of event

  // returns {words this event, channel mask, board fail, header timestamp}
  auto [words, channel_mask, fail, event_time] = dp->digi->UnpackEventHeader(buff);

  if(fail){ // board fail
    //GenerateArtificialDeadtime(((dp->clock_counter<<31) + dp->header_time), dp->digi);
    dp->digi->CheckFail(true);
    fFailCounter[dp->digi->bid()]++;
    return event_header_words;
  }

  buff.remove_prefix(event_header_words);
  int ret;
  int frags(0);
  unsigned n_chan = dp->digi->GetNumChannels();

  for(unsigned ch=0; ch<n_chan; ch++){
    if (channel_mask & (1<<ch)) {
      ret = ProcessChannel(buff, words, channel_mask, event_time, frags, ch, dp, dpc);
      buff.remove_prefix(ret);
    }
  }
  fFragsPerEvent[frags]++;
  return words;
}

int StraxFormatter::ProcessChannel(std::u32string_view buff, int words_in_event,
    int channel_mask, uint32_t event_time, int& frags, int channel,
    const std::unique_ptr<data_packet>& dp, std::map<int, int>& dpc) {
  // buff points to the first word of the channel's data

  int n_channels = std::bitset<max_channels>(channel_mask).count();
  // returns {timestamp (ns), words this channel, baseline, waveform}
  auto [timestamp, channel_words, baseline_ch, wf] = dp->digi->UnpackChannelHeader(
      buff, dp->clock_counter, dp->header_time, event_time, words_in_event, n_channels, channel);

  uint32_t samples_in_pulse = wf.size()*sizeof(char32_t)/sizeof(uint16_t);
  uint16_t sw = dp->digi->SampleWidth();
  int samples_per_frag= fFragmentBytes>>1;
  int16_t global_ch = fChannelMap[dp->digi->bid()][channel];
  // Failing to discern which channel we're getting data from seems serious enough to throw
  if(global_ch==-1)
    throw std::runtime_error("Failed to parse channel map. I'm gonna just kms now.");

  int num_frags = std::ceil(1.*samples_in_pulse/samples_per_frag);
  frags += num_frags;
  int32_t samples_this_frag = 0;
  int64_t time_this_frag = 0;
  const uint16_t zero_filler = 0;
  for (uint16_t frag_i = 0; frag_i < num_frags; frag_i++) {
    std::string fragment;
    fragment.reserve(fFullFragmentSize);

    // How long is this fragment?
    samples_this_frag = samples_per_frag;
    if (frag_i == num_frags-1)
      samples_this_frag = samples_in_pulse - frag_i*samples_per_frag;

    time_this_frag = timestamp + samples_per_frag*sw*frag_i;
    fragment.append((char*)&time_this_frag, sizeof(time_this_frag));
    fragment.append((char*)&samples_this_frag, sizeof(samples_this_frag));
    fragment.append((char*)&sw, sizeof(sw));
    fragment.append((char*)&global_ch, sizeof(global_ch));
    fragment.append((char*)&samples_in_pulse, sizeof(samples_in_pulse));
    fragment.append((char*)&frag_i, sizeof(frag_i));
    fragment.append((char*)&baseline_ch, sizeof(baseline_ch));

    // Copy the raw buffer
    fragment.append((char*)wf.data(), samples_this_frag*sizeof(uint16_t));
    wf.remove_prefix(samples_this_frag*sizeof(uint16_t)/sizeof(char32_t));
    for (; samples_this_frag < samples_per_frag; samples_this_frag++)
      fragment.append((char*)&zero_filler, sizeof(zero_filler));

    AddFragmentToBuffer(std::move(fragment), event_time, dp->clock_counter);
  } // loop over frag_i
  dpc[global_ch] += samples_in_pulse*sizeof(uint16_t);
  return channel_words;
}

void StraxFormatter::AddFragmentToBuffer(std::string fragment, uint32_t ts, int rollovers) {
  // Get the CHUNK and decide if this event also goes into a PRE/POST file
  int64_t timestamp = *(int64_t*)fragment.data();
  int chunk_id = timestamp/fFullChunkLength;
  bool overlap = (chunk_id+1)* fFullChunkLength - timestamp <= fChunkOverlap;
  int min_chunk(0), max_chunk(1);
  if (fChunks.size() > 0) {
    auto [min_iter, max_iter] = std::minmax_element(fChunks.begin(), fChunks.end(), 
      [&](auto& l, auto& r) {return l.first < r.first;});
    min_chunk = (*min_iter).first;
    max_chunk = (*max_iter).first;
  }

  const short* channel = (const short*)(fragment.data()+14);
  if (min_chunk - chunk_id > fWarnIfChunkOlderThan) {
    fLog->Entry(MongoLog::Warning,
        "Thread %lx got data from ch %i that's in chunk %i instead of %i/%i (ts %lx), it might get lost (ts %lx ro %i)",
        fThreadId, *channel, chunk_id, min_chunk, max_chunk, timestamp, ts, rollovers);
  } else if (chunk_id - max_chunk > 1) {
    fLog->Entry(MongoLog::Message, "Thread %lx skipped %i chunk(s) (ch%i)",
        fThreadId, chunk_id - max_chunk - 1, *channel);
  }

  fOutputBufferSize += fFullFragmentSize;

  auto chunks = {&fChunks, &fOverlaps};
  (*chunks[overlap])[chunk_id].emplace_back(std::move(fragment));

/*  if(!overlap){
    fChunks[chunk_id].emplace_back(std::move(fragment));
  } else {
    fOverlaps[chunk_id].emplace_back(std::move(fragment));
  }*/
}

int StraxFormatter::ReceiveDatapackets(std::list<std::unique_ptr<data_packet>>& in, int bytes) {
  using namespace std::chrono;
  auto start = high_resolution_clock::now();
  if (fBufferMutex.try_lock()) {
    auto end = high_resolution_clock::now();
    fBufferCounter[in.size()]++;
    fBuffer.splice(fBuffer.end(), in);
    fInputBufferSize += bytes;
    fMutexWaitTime.push_back(duration_cast<nanoseconds>(end-start).count());
    fBufferMutex.unlock();
    fCV.notify_one();
    return 0;
  }
  return 1;
}

void StraxFormatter::Process() {
  // this func runs in its own thread
  fThreadId = std::this_thread::get_id();
  std::stringstream ss;
  ss<<fHostname<<'_'<<fThreadId;
  fFullHostname = ss.str();
  fActive = true;
  std::unique_ptr<data_packet> dp;
  while (fActive == true || fBuffer.size() > 0) {
    std::unique_lock<std::mutex> lk(fBufferMutex);
    fCV.wait(lk, [&]{return fBuffer.size() > 0 || fActive == false;});
    if (fBuffer.size() > 0) {
      dp = std::move(fBuffer.front());
      fBuffer.pop_front();
      lk.unlock();
      ProcessDatapacket(std::move(dp));
      if (fActive == true) WriteOutChunks();
    } else {
      lk.unlock();
    }
  }
  if (fBytesProcessed > 0)
    End();
  if (fMutexWaitTime.size() > 0) std::sort(fMutexWaitTime.begin(), fMutexWaitTime.end());
}

void StraxFormatter::WriteOutChunk(int chunk_i){
  // Write the contents of the buffers to compressed files

  std::list<std::string>* buffers[2] = {&fChunks[chunk_i], &fOverlaps[chunk_i]};
  long uncompressed_size[3] = {0L, 0L, 0L};
  std::shared_ptr<std::string> uncompressed;
  std::shared_ptr<std::string> compressed[3];
  long wsize[3];

  for (int i = 0; i < 2; i++) {
    if (buffers[i]->size() == 0) continue;
    uncompressed_size[i] = buffers[i]->size()*fFullFragmentSize;
    uncompressed = std::make_shared<std::string>();
    uncompressed->reserve(uncompressed_size[i]);
    for (auto it = buffers[i]->begin(); it != buffers[i]->end(); it++)
      *uncompressed += *it; // std::accumulate would be nice but 3x slower without -O2
    // (also only works on c++20 because std::move, but still)
    buffers[i]->clear();
    wsize[i] = fCompressor(uncompressed, compressed[i], uncompressed_size[i]);
    fBytesPerChunk[int(std::log2(uncompressed_size[i]))]++;
    fOutputBufferSize -= uncompressed_size[i];
  }
  uncompressed.reset();
  fChunks.erase(chunk_i);
  fOverlaps.erase(chunk_i);

  // "copy" from n_post to n+1_pre
  // we used shared_ptr because we don't want any actual copying to happen
  compressed[2] = compressed[1];
  wsize[2] = wsize[1];
  uncompressed_size[2] = uncompressed_size[1];
  auto names = GetChunkNames(chunk_i);
  for (int i = 0; i < 3; i++) {
    if (uncompressed_size[i] == 0) continue;
    // write to *_TEMP
    auto output_dir_temp = GetDirectoryPath(names[i], true);
    auto filename_temp = GetFilePath(names[i], true);
    if (!fs::exists(output_dir_temp))
      fs::create_directory(output_dir_temp);
    std::ofstream writefile(filename_temp, std::ios::binary);
    writefile.write(compressed[i]->data(), wsize[i]);
    writefile.close();
    compressed[i].reset();

    auto output_dir = GetDirectoryPath(names[i]);
    auto filename = GetFilePath(names[i]);
    // shenanigans or skulduggery?
    if(fs::exists(filename)) {
      fLog->Entry(MongoLog::Warning, "Chunk %s from thread %lx already exists? %li vs %li bytes (%lx)",
          names[i].c_str(), fThreadId, fs::file_size(filename), wsize[i], uncompressed_size[i]);
    }

    // Move this chunk from *_TEMP to the same path without TEMP
    if(!fs::exists(output_dir))
      fs::create_directory(output_dir);
    fs::rename(filename_temp, filename);
  } // End writing
  return;
}

void StraxFormatter::WriteOutChunks() {
  int min_chunk(999999), max_chunk(0), tot_frags(0), n_frags(0);
  double average_chunk(0);
  for (auto it = fChunks.begin(); it != fChunks.end(); it++) {
    min_chunk = std::min(min_chunk, it->first);
    max_chunk = std::max(max_chunk, it->first);
    n_frags = it->second.size() + fOverlaps[it->first].size();
    tot_frags += n_frags;
    average_chunk += it->first * n_frags;
  }
  if (tot_frags == 0) return;
  average_chunk /= tot_frags;
  for (; min_chunk < average_chunk - fBufferNumChunks; min_chunk++)
    WriteOutChunk(min_chunk);
  CreateEmpty(min_chunk);
  return;
}

void StraxFormatter::End() {
  // this line is awkward, but iterators don't always like it when you're
  // changing the container while looping over its contents
  int max_chunk = -1;
  while (fChunks.size() > 0) {
    max_chunk = std::max(max_chunk, fChunks.begin()->first);
    WriteOutChunk(max_chunk);
  }
  if (max_chunk != -1) CreateEmpty(max_chunk);
  fChunks.clear();
  auto end_dir = GetDirectoryPath("THE_END");
  if(!fs::exists(end_dir)){
    fLog->Entry(MongoLog::Local,"Creating END directory at %s", end_dir.c_str());
    try{
      fs::create_directory(end_dir);
    }
    catch(...){};
  }
  std::ofstream outfile(GetFilePath("THE_END"), std::ios::out);
  outfile<<"...my only friend\n";
  outfile.close();
  return;
}

std::string StraxFormatter::GetStringFormat(int id){
  std::string chunk_index = std::to_string(id);
  while(chunk_index.size() < fChunkNameLength)
    chunk_index.insert(0, "0");
  return chunk_index;
}

fs::path StraxFormatter::GetDirectoryPath(const std::string& id, bool temp){
  fs::path write_path(fOutputPath);
  write_path /= id;
  if(temp)
    write_path+="_temp";
  return write_path;
}

fs::path StraxFormatter::GetFilePath(const std::string& id, bool temp){
  return GetDirectoryPath(id, temp) / fFullHostname;
}

void StraxFormatter::CreateEmpty(int back_from){
  for(; fEmptyVerified<back_from; fEmptyVerified++){
    for (auto& n : GetChunkNames(fEmptyVerified)) {
      if(!fs::exists(GetFilePath(n))){
        if(!fs::exists(GetDirectoryPath(n)))
          fs::create_directory(GetDirectoryPath(n));
        std::ofstream o(GetFilePath(n));
        o.close();
      }
    } // name
  } // chunks
}

std::vector<std::string> StraxFormatter::GetChunkNames(int chunk) {
  std::vector<std::string> ret{{GetStringFormat(chunk), GetStringFormat(chunk)+"_post",
    GetStringFormat(chunk+1)+"_pre"}};
  return ret;
}

