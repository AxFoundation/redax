#include "V1724.hh"
#include "MongoLog.hh"
#include "Options.hh"
#include "StraxFormatter.hh"
#include <algorithm>
#include <cmath>
#include <CAENVMElib.h>
#include <sstream>
#include <list>
#include <utility>


V1724::V1724(std::shared_ptr<MongoLog>& log, std::shared_ptr<Options>& opts, int bid, unsigned address){
  fBoardHandle = -1;
  fLog = log;

  fAqCtrlRegister = 0x8100;
  fAqStatusRegister = 0x8104;
  fSwTrigRegister = 0x8108;
  fResetRegister = 0xEF24;
  fClearRegister = 0xEF28;
  fChStatusRegister = 0x1088;
  fChDACRegister = 0x1098;
  fNChannels = 8;
  fChTrigRegister = 0x1060;
  fSNRegisterMSB = 0xF080;
  fSNRegisterLSB = 0xF084;
  fBoardFailStatRegister = 0x8178;
  fReadoutStatusRegister = 0xEF04;
  fBoardErrRegister = 0xEF00;
  fInputDelayRegister = 0x8034;
  fInputDelayChRegister = 0x1034;
  fEventSizeRegister = 0x814C;
  fError = false;

  fSampleWidth = 10;
  fClockCycle = 10;
  fBID = bid;
  fBaseAddress = address;
  fRolloverCounter = 0;
  fLastClock = 0;
  fBLTSafety = opts->GetDouble("blt_safety_factor", 1.5);
  fBLTalloc = opts->GetBLTalloc();
  // there's a more elegant way to do this, but I'm not going to write it
  fClockPeriod = std::chrono::nanoseconds((1l<<31)*fClockCycle);
  fArtificialDeadtimeChannel = 790;
  fDefaultDelay = 0xA * 2 * fSampleWidth; // see register document
}

V1724::~V1724(){
  End();
  if (fBLTCounter.empty()) return;
  std::stringstream msg;
  msg << "BLT report for board " << fBID;
  for (auto p : fBLTCounter) msg << " | " << p.first << " " << int(std::log2(p.second));
  msg << " | " << long(fTotReadTime.count());
  fLog->Entry(MongoLog::Local, msg.str());
}

int V1724::Init(int link, int crate, std::shared_ptr<Options>& opts) {
  int a = CAENVME_Init(cvV2718, link, crate, &fBoardHandle);
  if(a != cvSuccess){
    fLog->Entry(MongoLog::Warning, "Board %i failed to init, error %i handle %i link %i bdnum %i",
            fBID, a, fBoardHandle, link, crate);
    fBoardHandle = -1;
    return -1;
  }
  fLog->Entry(MongoLog::Debug, "Board %i initialized with handle %i (link/crate)(%i/%i)",
	      fBID, fBoardHandle, link, crate);

  uint32_t word(0);
  int my_bid(0);

  if (Reset()) {
    fLog->Entry(MongoLog::Error, "Board %i unable to pre-load registers", fBID);
    return -1;
  } else {
    fLog->Entry(MongoLog::Local, "Board %i reset", fBID);
  }
  fDelayPerCh.assign(fNChannels, fDefaultDelay);
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  if (opts->GetInt("do_sn_check", 0) != 0) {
    if ((word = ReadRegister(fSNRegisterLSB)) == 0xFFFFFFFF) {
      fLog->Entry(MongoLog::Error, "Board %i couldn't read its SN lsb", fBID);
      return -1;
    }
    my_bid |= word&0xFF;
    if ((word = ReadRegister(fSNRegisterMSB)) == 0xFFFFFFFF) {
      fLog->Entry(MongoLog::Error, "Board %i couldn't read its SN msb", fBID);
      return -1;
    }
    my_bid |= ((word&0xFF)<<8);
    if (my_bid != fBID) {
      fLog->Entry(MongoLog::Local, "Link %i crate %i should be SN %i but is actually %i",
        link, crate, fBID, my_bid);
    }
  }
  return 0;
}

int V1724::SINStart(){
  fLastClockTime = std::chrono::high_resolution_clock::now();
  fRolloverCounter = 0;
  fLastClock = 0;
  return WriteRegister(fAqCtrlRegister,0x105);
}
int V1724::SoftwareStart(){
  fLastClockTime = std::chrono::high_resolution_clock::now();
  fRolloverCounter = 0;
  fLastClock = 0;
  return WriteRegister(fAqCtrlRegister, 0x104);
}
int V1724::AcquisitionStop(bool){
  return WriteRegister(fAqCtrlRegister, 0x100);
}
int V1724::SWTrigger(){
  return WriteRegister(fSwTrigRegister, 0x1);
}
bool V1724::EnsureReady(int ntries, int tsleep){
  return MonitorRegister(fAqStatusRegister, 0x100, ntries, tsleep, 0x1);
}
bool V1724::EnsureStarted(int ntries, int tsleep){
  return MonitorRegister(fAqStatusRegister, 0x4, ntries, tsleep, 0x1);
}
bool V1724::EnsureStopped(int ntries, int tsleep){
  return MonitorRegister(fAqStatusRegister, 0x4, ntries, tsleep, 0x0);
}
uint32_t V1724::GetAcquisitionStatus(){
  return ReadRegister(fAqStatusRegister);
}
int V1724::CheckErrors(){
  auto pll = ReadRegister(fBoardFailStatRegister);
  auto ros = ReadRegister(fReadoutStatusRegister);
  unsigned ERR = 0xFFFFFFFF;
  if ((pll == ERR) || (ros == ERR)) return -1;
  int ret = 0;
  if (pll & (1 << 4)) ret |= 0x1;
  if (ros & (1 << 2)) ret |= 0x2;
  return ret;
}

int V1724::Reset() {
  int ret = WriteRegister(fResetRegister, 0x1);
  ret += WriteRegister(fBoardErrRegister, 0x30);
  return ret;
}

std::tuple<uint32_t, long> V1724::GetClockInfo(std::u32string_view sv) {
  auto it = sv.begin();
  do {
    if ((*it)>>28 == 0xA) {
      uint32_t ht = *(it+3)&0x7FFFFFFF;
      return {ht, GetClockCounter(ht)};
    }
  } while (++it < sv.end());
  fLog->Entry(MongoLog::Message, "No clock info for %i?", fBID);
  return {0xFFFFFFFF, -1};
}

int V1724::GetClockCounter(uint32_t timestamp){
  // The V1724 has a 31-bit on board clock counter that counts 10ns samples.
  // So it will reset every 21 seconds. We need to count the resets or we
  // can't run longer than that. We can employ some clever logic
  // and real-time time differences to handle clock rollovers and catch any
  // that we happen to miss the usual way

  auto now = std::chrono::high_resolution_clock::now();
  std::chrono::nanoseconds dt = now - fLastClockTime;
  fLastClockTime += dt; // no operator=

  int n_missed = dt / fClockPeriod;
  if (n_missed > 0) {
    fLog->Entry(MongoLog::Message, "Board %i missed %i rollovers", fBID, n_missed);
    fRolloverCounter += n_missed;
  }

  if (timestamp < fLastClock) {
    // actually rolled over
    fRolloverCounter++;
    fLog->Entry(MongoLog::Local, "Board %i rollover %i (%x/%x)",
        fBID, fRolloverCounter, fLastClock, timestamp);
  } else {
    // not a rollover
  }
  fLastClock = timestamp;
  return fRolloverCounter;
}

int V1724::WriteRegister(unsigned int reg, unsigned int value){
  uint32_t write=0;
  write+=value;
  int ret = 0;
  if (reg == fInputDelayRegister)
    fDelayPerCh.assign(fNChannels, 2*fSampleWidth*value);
  else if ((reg & fInputDelayChRegister) == fInputDelayChRegister)
    fDelayPerCh[(reg>>16)&0xF] = 2*fSampleWidth*value;
  if((ret = CAENVME_WriteCycle(fBoardHandle, fBaseAddress+reg,
			&write,cvA32_U_DATA,cvD32)) != cvSuccess){
    fLog->Entry(MongoLog::Warning,
		"Board %i write returned %i (ret), reg 0x%04x, value 0x%08x",
		fBID, ret, reg, value);
    return -1;
  }
  return 0;
}

unsigned int V1724::ReadRegister(unsigned int reg){
  unsigned int temp;
  int ret = -100;
  if((ret = CAENVME_ReadCycle(fBoardHandle, fBaseAddress+reg, &temp,
			      cvA32_U_DATA, cvD32)) != cvSuccess){
    fLog->Entry(MongoLog::Warning,
		"Board %i read returned: %i (ret) 0x%08x (val) for reg 0x%04x",
		fBID, ret, temp, reg);
    return 0xFFFFFFFF;
  }
  return temp;
}

int V1724::Read(std::unique_ptr<data_packet>& outptr){
  using namespace std::chrono;
  auto t_start = high_resolution_clock::now();
  // Initialize
  int blt_words=0, nb=0, ret=-5;
  std::vector<std::pair<char32_t*, int>> xfer_buffers;

  int alloc_words, request_bytes;
  char32_t* thisBLT = nullptr;
  while ((GetAcquisitionStatus() & 0x8) != 0) {
    // how big is the event waiting for readout?
    if ((alloc_words = ReadRegister(fEventSizeRegister)) == 0xFFFFFFFF) {
      ret = 1;
      break;
    }
    if (alloc_words == 0) {
      fLog->Entry(MongoLog::Message, "Zero-sized event from %i?", fBID);
      break;
    }
    // Reserve space for this block transfer. Alloc more than "necessary" because CAEN driver
    thisBLT = new char32_t[int(alloc_words * fBLTSafety)];
    request_bytes = alloc_words * sizeof(char32_t);

    ret = CAENVME_FIFOBLTReadCycle(fBoardHandle, fBaseAddress, thisBLT,
				     request_bytes, cvA32_U_MBLT, cvD64, &nb);
    if( (ret != cvSuccess) && (ret != cvBusError) ){
      fLog->Entry(MongoLog::Error,
		  "Board %i read error after %i reads: (%i) and transferred %i bytes this read",
		  fBID, xfer_buffers.size(), ret, nb);

      delete[] thisBLT;
      ret = 1;
    }
    if (nb > request_bytes) fLog->Entry(MongoLog::Message,
        "Board %i got %x more bytes than asked for (headroom %i)",
        fBID, nb-request_bytes, alloc_words*sizeof(char32_t)-nb);

    blt_words+=nb/sizeof(char32_t);
    xfer_buffers.emplace_back(std::make_pair(thisBLT, nb/sizeof(char32_t)));
  }

  // Now we have to concatenate all this data into a single continuous buffer
  // because I'm too lazy to write a class that allows us to use fragmented
  // buffers as if they were continuous
  if(blt_words>0 && ret != 1){
    std::u32string s;
    s.reserve(blt_words);
    for (auto& xfer : xfer_buffers) {
      s.append(xfer.first, xfer.second);
    }
    fBLTCounter[int(std::ceil(std::log2(blt_words)))]++;
    auto [ht, cc] = GetClockInfo(s);
    outptr = std::make_unique<data_packet>(std::move(s), ht, cc);
  }
  for (auto b : xfer_buffers) delete[] b.first;
  fTotReadTime += duration_cast<nanoseconds>(high_resolution_clock::now()-t_start);
  return ret == 1 ? -1 : blt_words;
}

int V1724::LoadDAC(std::vector<uint16_t> &dac_values){
  // Loads DAC values into registers
  for(unsigned int x=0; x<fNChannels; x++){
    if(WriteRegister((fChDACRegister)+(0x100*x), dac_values[x])!=0){
      fLog->Entry(MongoLog::Error, "Board %i failed writing DAC 0x%04x in channel %i",
		  fBID, dac_values[x], x);
      return -1;
    }

  }
  return 0;
}

int V1724::SetThresholds(std::vector<uint16_t> vals) {
  int ret = 0;
  for (unsigned ch = 0; ch < fNChannels; ch++)
    ret += WriteRegister(fChTrigRegister + 0x100*ch, vals[ch]);
  return ret;
}

int V1724::End(){
  if(fBoardHandle>=0)
    CAENVME_End(fBoardHandle);
  fBoardHandle=-1;
  fBaseAddress=0;
  return 0;
}

void V1724::ClampDACValues(std::vector<uint16_t> &dac_values,
                  std::map<std::string, std::vector<double>> &cal_values) {
  uint16_t min_dac, max_dac(0xffff);
  for (unsigned ch = 0; ch < fNChannels; ch++) {
    if (cal_values["yint"][ch] > 0x3fff) {
      min_dac = (0x3fff - cal_values["yint"][ch])/cal_values["slope"][ch];
    } else {
      min_dac = 0;
    }
    dac_values[ch] = std::clamp(dac_values[ch], min_dac, max_dac);
    if ((dac_values[ch] == min_dac) || (dac_values[ch] == max_dac)) {
      fLog->Entry(MongoLog::Local, "Board %i channel %i clamped dac to 0x%04x",
	fBID, ch, dac_values[ch]);
    }
  }
}

bool V1724::MonitorRegister(uint32_t reg, uint32_t mask, int ntries, int sleep, uint32_t val){
  uint32_t rval = 0;
  if(val == 0) rval = 0xffffffff;
  for(int counter = 0; counter < ntries; counter++){
    rval = ReadRegister(reg);
    if(rval == 0xffffffff)
      break;
    if((val == 1 && (rval&mask)) || (val == 0 && !(rval&mask)))
      return true;
    usleep(sleep);
  }
  fLog->Entry(MongoLog::Warning,"Board %i MonitorRegister failed for 0x%04x with mask 0x%04x and register value 0x%04x, wanted 0x%04x",
          fBID, reg, mask, rval,val);
  return false;
}

std::tuple<int, int, bool, uint32_t> V1724::UnpackEventHeader(std::u32string_view sv) {
  // returns {words this event, channel mask, board fail, header timestamp}
  return {sv[0]&0xFFFFFFF, sv[1]&0xFF, sv[1]&0x4000000, sv[3]&0x7FFFFFFF};
}

std::tuple<int64_t, int, uint16_t, std::u32string_view> V1724::UnpackChannelHeader(std::u32string_view sv, long rollovers, uint32_t header_time, uint32_t, int, int, short ch) {
  // returns {timestamp (ns), words this channel, baseline, waveform}
  long ch_time = sv[1]&0x7FFFFFFF;
  int words = sv[0]&0x7FFFFF;
  // More rollover logic here, because channels are independent and the
  // processing is multithreaded. We leverage the fact that readout windows are
  // short and polled frequently compared to the rollover timescale, so there
  // will never be a large difference in timestamps in one data packet
  if (ch_time > 15e8 && header_time < 5e8 && rollovers != 0) rollovers--;
  else if (ch_time < 5e8 && header_time > 15e8) rollovers++;
  return {((rollovers<<31)+ch_time)*fClockCycle - fDelayPerCh[ch], words, 0, sv.substr(2, words-2)};
}

