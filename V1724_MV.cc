#include "V1724_MV.hh"
#include "MongoLog.hh"
#include "Options.hh"

V1724_MV::V1724_MV(std::shared_ptr<MongoLog>& log, std::shared_ptr<Options>& opts, int bid, unsigned address) :
V1724(log, opts, bid, address) {
  // MV boards seem to have reg 0x1n80 for channel n threshold
  fChTrigRegister = 0x1080;
  fInputDelayRegister = fInputDelayChRegister = 0xFFFFFFFF; // disabled
  fArtificialDeadtimeChannel = 791;
  fDefaultDelay = 0;
  fDefaultPreTrig = 0; // no default provided
  fPreTrigRegister = 0x8114; // actually the post-trig register
  fPreTrigChRegister = 0xFFFFFFFF; // disabled
  // the MV is offset by ~2.5us relative to the other detectors for reasons
  // related to trigger formation. This is the easiest way to handle this
  fConstantTimeOffset = opts->GetInt("mv_time_offset", 2420);
}

V1724_MV::~V1724_MV(){}

std::tuple<int64_t, int, uint16_t, std::u32string_view> 
V1724_MV::UnpackChannelHeader(std::u32string_view sv, long rollovers,
    uint32_t header_time, uint32_t event_time, int event_words, int n_channels, short ch) {
  int words = (event_words-4)/n_channels;
  // returns {timestamp (ns), words this ch, baseline, waveform}
  // More rollover logic here, because processing is multithreaded.
  // We leverage the fact that readout windows are
  // short and polled frequently compared to the rollover timescale, so there
  // will never be a large difference in timestamps in one data packet
  // also 'fPreTrig' is actually the post-trigger, so we convert back here
  int pre_trig_ns = words * 2 * fSampleWidth - fPreTrigPerCh[ch] + fConstantTimeOffset;
  if (event_time > 15e8 && header_time < 5e8 && rollovers != 0) rollovers--;
  else if (event_time < 5e8 && header_time > 15e8) rollovers++;
  return {((rollovers<<31)+event_time)*fClockCycle - pre_trig_ns,
          words,
          0,
          sv.substr(0, words)};
}
