#include "DAQController.hh"
#include "V1724.hh"
#include "V1724_MV.hh"
#include "V1730.hh"
#include "f1724.hh"
#include "DAXHelpers.hh"
#include "Options.hh"
#include "StraxFormatter.hh"
#include "MongoLog.hh"
#include <algorithm>
#include <bitset>
#include <chrono>
#include <cmath>
#include <numeric>

#include <bsoncxx/builder/stream/document.hpp>

// Status:
// 0-idle
// 1-arming
// 2-armed
// 3-running
// 4-error

DAQController::DAQController(std::shared_ptr<MongoLog>& log, std::string hostname){
  fLog=log;
  fOptions = nullptr;
  fStatus = DAXHelpers::Idle;
  fReadLoop = false;
  fNProcessingThreads=8;
  fDataRate=0.;
  fHostname = hostname;
  fPLL = 0;
}

DAQController::~DAQController(){
  if(fProcessingThreads.size()!=0)
    CloseThreads();
}

int DAQController::Arm(std::shared_ptr<Options>& options){
  fOptions = options;
  fNProcessingThreads = fOptions->GetNestedInt("processing_threads."+fHostname, 8);
  fLog->Entry(MongoLog::Local, "Beginning electronics initialization with %i threads",
	      fNProcessingThreads);

  // Initialize digitizers
  fPLL = 0;
  fStatus = DAXHelpers::Arming;
  int num_boards = 0;
  for(auto& d : fOptions->GetBoards("V17XX")){
    fLog->Entry(MongoLog::Local, "Arming new digitizer %i", d.board);

    std::shared_ptr<V1724> digi;
    try{
      if(d.type == "V1724_MV")
        digi = std::make_shared<V1724_MV>(fLog, fOptions, d.board, d.vme_address);
      else if(d.type == "V1730")
        digi = std::make_shared<V1730>(fLog, fOptions, d.board, d.vme_address);
      else if(d.type == "f1724")
        digi = std::make_shared<f1724>(fLog, fOptions, d.board, 0);
      else
        digi = std::make_shared<V1724>(fLog, fOptions, d.board, d.vme_address);
      if (digi->Init(d.link, d.crate))
        throw std::runtime_error("Board init failed");
      fDigitizers[d.link].emplace_back(digi);
      num_boards++;
    }catch(const std::exception& e) {
      fLog->Entry(MongoLog::Warning, "Failed to initialize digitizer %i: %s", d.board,
          e.what());
      fDigitizers.clear();
      return -1;
    }
  }
  fLog->Entry(MongoLog::Local, "This host has %i boards", num_boards);
  fLog->Entry(MongoLog::Local, "Sleeping for two seconds");
  // For the sake of sanity and sleeping through the night,
  // do not remove this statement.
  sleep(2); // <-- this one. Leave it here.
  // Seriously. This sleep statement is absolutely vital.
  fLog->Entry(MongoLog::Local, "That felt great, thanks.");
  std::map<int, std::vector<uint16_t>> dac_values;
  std::vector<std::thread> init_threads;
  init_threads.reserve(fDigitizers.size());
  std::map<int,int> rets;
  // Parallel digitizer programming to speed baselining
  for( auto& link : fDigitizers ) {
    rets[link.first] = 1;
    init_threads.emplace_back(&DAQController::InitLink, this,
	  std::ref(link.second), std::ref(dac_values), std::ref(rets[link.first]));
  }
  for (auto& t : init_threads) if (t.joinable()) t.join();

  if (std::any_of(rets.begin(), rets.end(), [](auto& p) {return p.second != 0;})) {
    fLog->Entry(MongoLog::Warning, "Encountered errors during digitizer programming");
    if (std::any_of(rets.begin(), rets.end(), [](auto& p) {return p.second == -2;}))
      fStatus = DAXHelpers::Error;
    else
      fStatus = DAXHelpers::Idle;
    return -1;
  } else
    fLog->Entry(MongoLog::Debug, "Digitizer programming successful");
  if (fOptions->GetString("baseline_dac_mode") == "fit" || fOptions->GetNestedString("baseline_dac_mode."+fOptions->Detector()) == "fit") fOptions->UpdateDAC(dac_values);

  for(auto& link : fDigitizers ) {
    for(auto& digi : link.second){
      digi->AcquisitionStop();
    }
  }
  if (OpenThreads()) {
    fLog->Entry(MongoLog::Warning, "Error opening threads");
    fStatus = DAXHelpers::Idle;
    return -1;
  }
  sleep(1);
  fStatus = DAXHelpers::Armed;

  fLog->Entry(MongoLog::Local, "Arm command finished, returning to main loop");
  return 0;
}

int DAQController::Start(){
  if(fOptions->GetInt("run_start", 0) == 0){
    for(auto& link : fDigitizers ){
      for(auto& digi : link.second){
        if(digi->EnsureReady()!= true || digi->SoftwareStart() || digi->EnsureStarted() != true){
          fLog->Entry(MongoLog::Warning, "Board %i not started?", digi->bid());
          return -1;
        } else
          fLog->Entry(MongoLog::Local, "Board %i started", digi->bid());
      }
    }
  } else {
    for (auto& link : fDigitizers)
      for (auto& digi : link.second)
        if (digi->SINStart() || !digi->EnsureReady())
          fLog->Entry(MongoLog::Warning, "Board %i not ready to start?", digi->bid());
        else
          fLog->Entry(MongoLog::Local, "Board %i is ARMED and DANGEROUS", digi->bid());
  }
  fStatus = DAXHelpers::Running;
  return 0;
}

int DAQController::Stop(){

  fReadLoop = false; // at some point.
  int counter = 0;
  bool one_still_running = false;
  do{
    // wait around for up to 10x100ms for the threads to finish reading
    one_still_running = false;
    for (auto& p : fRunning) one_still_running |= p.second;
    if (one_still_running) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }while(one_still_running && counter++ < 10);
  if (counter >= 10) fLog->Entry(MongoLog::Local, "Boards taking a while to clear");
  fLog->Entry(MongoLog::Local, "Stopping boards");
  for( auto const& link : fDigitizers ){
    for(auto digi : link.second){
      digi->AcquisitionStop(true);

      // Ensure digitizer is stopped
      if(digi->EnsureStopped() != true){
	fLog->Entry(MongoLog::Warning,
		    "Timed out waiting for %i to stop after SW stop sent", digi->bid());
          //return -1;
      }
    }
  }
  fLog->Entry(MongoLog::Debug, "Stopped digitizers, closing threads");
  CloseThreads();
  fLog->Entry(MongoLog::Local, "Closing Digitizers");
  for(auto& link : fDigitizers ){
    for(auto& digi : link.second){
      digi->End();
      digi.reset();
    }
    link.second.clear();
  }
  fDigitizers.clear();

  fPLL = 0;
  fLog->SetRunId(-1);
  fOptions.reset();
  fLog->Entry(MongoLog::Local, "Finished end sequence");
  fStatus = DAXHelpers::Idle;
  return 0;
}

void DAQController::ReadData(int link){
  fReadLoop = true;

  fDataRate = 0;

  uint32_t board_status = 0;
  int readcycler = 0;
  int err_val = 0;
  std::list<std::unique_ptr<data_packet>> local_buffer;
  std::unique_ptr<data_packet> dp;
  std::vector<int> mutex_wait_times;
  mutex_wait_times.reserve(1<<20);
  int words = 0;
  unsigned transfer_batch = fOptions->GetInt("transfer_batch", 8);
  int bytes_this_loop(0);
  fRunning[link] = true;
  std::chrono::microseconds sleep_time(fOptions->GetInt("us_between_reads", 10));
  int c = 0;
  const int num_threads = fNProcessingThreads;
  while(fReadLoop){
    for(auto& digi : fDigitizers[link]) {

      // periodically report board status
      if(readcycler == 0){
        board_status = digi->GetAcquisitionStatus();
        fLog->Entry(MongoLog::Local, "Board %i has status 0x%04x",
            digi->bid(), board_status);
      }
      if (digi->CheckFail()) {
        err_val = digi->CheckErrors();
        fLog->Entry(MongoLog::Local, "Error %i from board %i", err_val, digi->bid());
        if (err_val == -1 || err_val == 0) {

        } else {
          fStatus = DAXHelpers::Error; // stop command will be issued soon
          if (err_val & 0x1) {
            fLog->Entry(MongoLog::Local, "Board %i has PLL unlock", digi->bid());
            fPLL++;
          }
          if (err_val & 0x2) fLog->Entry(MongoLog::Local, "Board %i has VME bus error", digi->bid());
        }
      }
      if((words = digi->Read(dp))<0){
        dp.reset();
        fStatus = DAXHelpers::Error;
        break;
      } else if(words>0){
        dp->digi = digi;
        local_buffer.emplace_back(std::move(dp));
        bytes_this_loop += words*sizeof(char32_t);
      }
    } // for digi in digitizers
    if (local_buffer.size() && (readcycler % transfer_batch == 0)) {
      fDataRate += bytes_this_loop;
      auto t_start = std::chrono::high_resolution_clock::now();
      while (fFormatters[(++c)%num_threads]->ReceiveDatapackets(local_buffer, bytes_this_loop)) {}
      auto t_end = std::chrono::high_resolution_clock::now();
      mutex_wait_times.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(
            t_end-t_start).count());
      bytes_this_loop = 0;
    }
    if (++readcycler > 10000) readcycler = 0;
    std::this_thread::sleep_for(sleep_time);
  } // while run
  if (mutex_wait_times.size() > 0) {
    std::sort(mutex_wait_times.begin(), mutex_wait_times.end());
    fLog->Entry(MongoLog::Local, "RO thread %i mutex report: min %i max %i mean %i median %i num %i",
        link, mutex_wait_times.front(), mutex_wait_times.back(),
        std::accumulate(mutex_wait_times.begin(), mutex_wait_times.end(), 0l)/mutex_wait_times.size(),
        mutex_wait_times[mutex_wait_times.size()/2], mutex_wait_times.size());
  }
  fRunning[link] = false;
  fLog->Entry(MongoLog::Local, "RO thread %i returning", link);
}

int DAQController::OpenThreads(){
  const std::lock_guard<std::mutex> lg(fMutex);
  fProcessingThreads.reserve(fNProcessingThreads);
  for(int i=0; i<fNProcessingThreads; i++){
    try {
      fFormatters.emplace_back(std::make_unique<StraxFormatter>(fOptions, fLog, fDigitizers));
      fProcessingThreads.emplace_back(&StraxFormatter::Process, fFormatters.back().get());
    } catch(const std::exception& e) {
      fLog->Entry(MongoLog::Warning, "Error opening processing threads: %s",
          e.what());
      return -1;
    }
  }
  fReadoutThreads.reserve(fDigitizers.size());
  for (auto& p : fDigitizers)
    fReadoutThreads.emplace_back(&DAQController::ReadData, this, p.first);
  return 0;
}

void DAQController::CloseThreads(){
  const std::lock_guard<std::mutex> lg(fMutex);
  fLog->Entry(MongoLog::Local, "Ending RO threads");
  for (auto& t : fReadoutThreads) if (t.joinable()) t.join();
  fLog->Entry(MongoLog::Local, "Joining processing threads");
  std::map<int,int> board_fails;
  for (auto& sf : fFormatters) {
    while (sf->GetBufferSize().first > 0)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    sf->Close(board_fails);
  }
  for (auto& t : fProcessingThreads) if (t.joinable()) t.join();
  fProcessingThreads.clear();
  fLog->Entry(MongoLog::Local, "Destroying formatters");
  for (auto& sf : fFormatters) sf.reset();
  fFormatters.clear();

  if (std::accumulate(board_fails.begin(), board_fails.end(), 0,
	[=](int tot, auto& iter) {return std::move(tot) + iter.second;})) {
    std::stringstream msg;
    msg << "Found board failures: ";
    for (auto& iter : board_fails) msg << iter.first << ":" << iter.second << " | ";
    fLog->Entry(MongoLog::Warning, msg.str());
  }
}

void DAQController::StatusUpdate(mongocxx::collection* collection) {
  using namespace bsoncxx::builder::stream;
  auto insert_doc = document{};
  std::map<int, int> retmap;
  std::pair<long, long> buf{0,0};
  int rate = fDataRate;
  fDataRate -= fDataRate; // atomic nonsense, increment and decrement ops are better than assignment
  {
    const std::lock_guard<std::mutex> lg(fMutex);
    for (auto& p : fFormatters) {
      p->GetDataPerChan(retmap);
      auto x = p->GetBufferSize();
      buf.first += x.first;
      buf.second += x.second;
    }
  }
  int rate_alt = std::accumulate(retmap.begin(), retmap.end(), 0,
      [&](int tot, const std::pair<int, int>& p) {return std::move(tot) + p.second;});
  auto doc = document{} <<
    "host" << fHostname <<
    "time" << bsoncxx::types::b_date(std::chrono::system_clock::now())<<
    "rate_old" << rate*1e-6 <<
    "rate" << rate_alt*1e-6 <<
    "status" << fStatus <<
    "buffer_size" << (buf.first + buf.second)/1e6 <<
    "mode" << (fOptions ? fOptions->GetString("name", "none") : "none") <<
    "number" << (fOptions ? fOptions->GetInt("number", -1) : -1) <<
    "pll" << fPLL.load() <<
    "channels" << open_document <<
      [&](key_context<> doc){
      for( auto const& pair : retmap)
        doc << std::to_string(pair.first) << short(pair.second>>10); // KB not MB
      } << close_document << 
    finalize;
  collection->insert_one(std::move(doc));
  return;
}

void DAQController::InitLink(std::vector<std::shared_ptr<V1724>>& digis,
    std::map<int, std::vector<uint16_t>>& dac_values, int& ret) {
  std::string baseline_mode = fOptions->GetString("baseline_dac_mode", "n/a");
  if (baseline_mode == "n/a")
    baseline_mode = fOptions->GetNestedString("baseline_dac_mode."+fOptions->Detector(), "fixed");
  int nominal_dac = fOptions->GetInt("baseline_fixed_value", 7000);
  if (baseline_mode == "fit") {
    if ((ret = FitBaselines(digis, dac_values)) < 0) {
      fLog->Entry(MongoLog::Warning, "Errors during baseline fitting");
      return;
    } else if (ret > 0) {
      fLog->Entry(MongoLog::Debug, "Baselines didn't converge so we'll use Plan B");
    }
  }

  for(auto& digi : digis){
    fLog->Entry(MongoLog::Local, "Board %i beginning specific init", digi->bid());
    digi->ResetFlags();

    // Multiple options here
    int bid = digi->bid(), success(0);
    if (baseline_mode == "fit") {
    } else if(baseline_mode == "cached") {
      dac_values[bid] = fOptions->GetDAC(bid, digi->GetNumChannels(), nominal_dac);
      fLog->Entry(MongoLog::Local, "Board %i using cached baselines", bid);
    } else if(baseline_mode == "fixed"){
      fLog->Entry(MongoLog::Local, "Loading fixed baselines with value 0x%04x", nominal_dac);
      dac_values[bid].assign(digi->GetNumChannels(), nominal_dac);
    } else {
      fLog->Entry(MongoLog::Warning, "Received unknown baseline mode '%s', valid options are 'fit', 'cached', and 'fixed'", baseline_mode.c_str());
      ret = -1;
      return;
    }

    for(auto& regi : fOptions->GetRegisters(bid)){
      unsigned int reg = DAXHelpers::StringToHex(regi.reg);
      unsigned int val = DAXHelpers::StringToHex(regi.val);
      success+=digi->WriteRegister(reg, val);
    }
    success += digi->LoadDAC(dac_values[bid]);
    // Load all the other fancy stuff
    success += digi->SetThresholds(fOptions->GetThresholds(bid));

    fLog->Entry(MongoLog::Local, "Board %i programmed", digi->bid());
    if(success!=0){
      fLog->Entry(MongoLog::Warning, "Failed to configure digitizers.");
      ret = -1;
      return;
    }
  } // loop over digis per link

  ret = 0;
  return;
}

int DAQController::FitBaselines(std::vector<std::shared_ptr<V1724>> &digis,
    std::map<int, std::vector<u_int16_t>> &dac_values) {
  /* This function has caused a lot of problems in the past for a wide variety of reasons.
   * What it's trying to do isn't complex: figure out iteratively what value to write to the DAC so the
   * baselines show up where you want them to. Usually the boards cooperate, sometimes they don't.
   * A large fraction of the code is dealing with when they don't.
   */
  int max_steps = fOptions->GetInt("baseline_max_steps", 30);
  int convergence = fOptions->GetInt("baseline_convergence_threshold", 3);
  uint16_t start_dac = fOptions->GetInt("baseline_start_dac", 10000);
  std::map<int, std::vector<int>> channel_finished;
  std::map<int, bool> board_done;
  std::map<int, std::vector<double>> bl_per_channel;
  int bid;

  for (auto digi : digis) { // alloc ALL the things!
    bid = digi->bid();
    dac_values[bid] = std::vector<uint16_t>(digi->GetNumChannels(), start_dac); // start higher than we need to
    channel_finished[bid] = std::vector<int>(digi->GetNumChannels(), 0);
    board_done[bid] = false;
    bl_per_channel[bid] = std::vector<double>(digi->GetNumChannels(), 0);
    digi->SetFlags(2);
  }

  for (int step = 0; step < max_steps; step++) {
    fLog->Entry(MongoLog::Local, "Beginning baseline step %i/%i", step, max_steps);
    // prep
    for (auto& d : digis) {
      bid = d->bid();
      if (board_done[bid])
        continue;
      if (d->LoadDAC(dac_values[d->bid()])) {
        fLog->Entry(MongoLog::Warning, "Board %i failed to load DAC", d->bid());
        return -2;
      }
    }
    // "After writing, the user is recommended to wait for a few seconds before
    // a new RUN to let the DAC output get stabilized" - CAEN documentation
    std::this_thread::sleep_for(std::chrono::seconds(1));
    // sleep(2) seems unnecessary after preliminary testing

    for (auto& d : digis) {
      int bid = d->bid();
      if (board_done[bid])
        continue;
      if (d->BaselineStep(dac_values[bid], channel_finished[bid], bl_per_channel[bid], step) < 0) {
        fLog->Entry(MongoLog::Error, "Error fitting baselines");
        return -2;
      }
      board_done[bid] = std::all_of(channel_finished[bid].begin(), channel_finished[bid].end(), [=](int v){return v >= convergence;});
    }

    if (std::all_of(board_done.begin(), board_done.end(), [](auto& p){return p.second;})) return 0;
  } // end steps
  std::string backup_bl = fOptions->GetString("baseline_fallback_mode", "fail");
  if (backup_bl == "fail") {
    fLog->Entry(MongoLog::Warning, "Baseline fallback mode is 'fail'");
    return -3;
  }
  int fixed = fOptions->GetInt("baseline_fixed_value", 7000);
  for (auto& p : channel_finished) // (bid, vector)
    for (unsigned i = 0; i < p.second.size(); i++)
      if (p.second[i] < convergence) {
        fLog->Entry(MongoLog::Local, "%i.%i didn't converge, last value %.1f, last offset 0x%x",
            p.first, i, bl_per_channel[p.first][i], dac_values[p.first][i]);
        if (backup_bl == "cached")
          dac_values[p.first][i] = fOptions->GetSingleDAC(p.first, i, fixed);
        else if (backup_bl == "fixed")
          dac_values[p.first][i] = fixed;
      }
  return 1;
}

