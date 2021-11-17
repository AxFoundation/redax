#include "V1730.hh"
#include "MongoLog.hh"
#include "Options.hh"

V1730::V1730(std::shared_ptr<MongoLog>& log, std::shared_ptr<Options>& options, int bid, unsigned address)
  :V1724(log, options, bid, address){
  fNChannels = 16;
  fSampleWidth = 2;
  fClockCycle = 2;
  fArtificialDeadtimeChannel = 792;
  fDefaultDelay = 2*fSampleWidth*0xA; // see register document
}

V1730::~V1730(){}

std::tuple<int, int, bool, uint32_t> V1730::UnpackEventHeader(std::u32string_view sv) {
  // returns {words this event, channel mask, board fail, header timestamp}
  return {sv[0]&0xFFFFFFF,
         (sv[1]&0xFF) | ((sv[2]>>16)&0xFF00),
          sv[1]&0x4000000,
          sv[3]&0x7FFFFFFF};
}

std::tuple<int64_t, int, uint16_t, std::u32string_view>
V1730::UnpackChannelHeader(std::u32string_view sv, long, uint32_t, uint32_t, int, int, short ch) {
  // returns {timestamp (ns), words this channel, baseline, waveform}
  int words = sv[0]&0x7FFFFF;
  return {(long(sv[1]) | (long(sv[2]&0xFFFF)<<32))*fClockCycle - fDelayPerCh[ch],
          words,
          (sv[2]>>16)&0x3FFF,
          sv.substr(3, words-3)};
}
