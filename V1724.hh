#ifndef _V1724_HH_
#define _V1724_HH_

#include <cstdint>
#include <vector>
#include <map>
#include <chrono>
#include <memory>
#include <atomic>
#include <tuple>

class MongoLog;
class Options;
class data_packet;

class V1724{

 public:
  V1724(std::shared_ptr<MongoLog>&, std::shared_ptr<Options>&, int, unsigned=0);
  virtual ~V1724();

  virtual int Init(int, int, std::shared_ptr<Options>&);
  virtual int Read(std::unique_ptr<data_packet>&);
  virtual int WriteRegister(unsigned int reg, unsigned int value);
  virtual unsigned int ReadRegister(unsigned int reg);
  virtual int End();

  int bid() {return fBID;}
  uint16_t SampleWidth() {return fSampleWidth;}
  int GetClockWidth() {return fClockCycle;}
  int16_t GetADChannel() {return fArtificialDeadtimeChannel;}

  virtual int LoadDAC(std::vector<uint16_t>&);
  void ClampDACValues(std::vector<uint16_t>&, std::map<std::string, std::vector<double>>&);
  unsigned GetNumChannels() {return fNChannels;}
  int SetThresholds(std::vector<uint16_t> vals);

  virtual std::tuple<int, int, bool, uint32_t> UnpackEventHeader(std::u32string_view);
  virtual std::tuple<int64_t, int, uint16_t, std::u32string_view> UnpackChannelHeader(std::u32string_view, long, uint32_t, uint32_t, int, int, short);

  bool CheckFail(bool val=false) {bool ret = fError; fError = val; return ret;}

  // Acquisition Control

  virtual int SINStart();
  virtual int SoftwareStart();
  virtual int AcquisitionStop(bool=false);
  virtual int SWTrigger();
  virtual int Reset();
  virtual bool EnsureReady(int ntries, int sleep);
  virtual bool EnsureStarted(int ntries, int sleep);
  virtual bool EnsureStopped(int ntries, int sleep);
  virtual int CheckErrors();
  virtual uint32_t GetAcquisitionStatus();

protected:
  // Some values for base classes to override 
  unsigned int fAqCtrlRegister;
  unsigned int fAqStatusRegister;
  unsigned int fSwTrigRegister;
  unsigned int fResetRegister;
  unsigned int fClearRegister;
  unsigned int fChStatusRegister;
  unsigned int fChDACRegister;
  unsigned int fChTrigRegister;
  unsigned int fNChannels;
  unsigned int fSNRegisterMSB;
  unsigned int fSNRegisterLSB;
  unsigned int fBoardFailStatRegister;
  unsigned int fReadoutStatusRegister;
  unsigned int fVMEAlignmentRegister;
  unsigned int fBoardErrRegister;
  unsigned int fInputDelayRegister;
  unsigned int fInputDelayChRegister;
  unsigned int fEventSizeRegister;

  std::vector<int> fBLTalloc;
  std::map<int, int> fBLTCounter;
  std::vector<int> fDelayPerCh;

  bool MonitorRegister(uint32_t reg, uint32_t mask, int ntries, int sleep, uint32_t val=1);
  virtual std::tuple<uint32_t, long> GetClockInfo(std::u32string_view);
  virtual int GetClockCounter(uint32_t);
  int fBoardHandle;
  int fBID;
  unsigned int fBaseAddress;
  int fDefaultDelay;

  // Stuff for clock reset tracking
  int fRolloverCounter;
  uint32_t fLastClock;
  std::chrono::high_resolution_clock::time_point fLastClockTime;
  std::chrono::nanoseconds fClockPeriod;

  std::shared_ptr<MongoLog> fLog;
  std::atomic_bool fError;

  float fBLTSafety;
  int fSampleWidth, fClockCycle;
  int16_t fArtificialDeadtimeChannel;
  std::chrono::nanoseconds fTotReadTime;
};


#endif
