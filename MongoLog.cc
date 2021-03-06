#include "MongoLog.hh"
#include <iostream>
#include <bsoncxx/builder/stream/document.hpp>

#ifndef REDAX_BUILD_COMMIT
#define REDAX_BUILD_COMMIT "UNKNOWN"
#endif

namespace fs=std::experimental::filesystem;

MongoLog::MongoLog(int DeleteAfterDays, std::shared_ptr<mongocxx::pool>& pool, std::string dbname, std::string log_dir, std::string host) : 
  fPool(pool), fClient(pool->acquire()) {
  fLogLevel = 0;
  fHostname = host;
  fDeleteAfterDays = DeleteAfterDays;
  fFlushPeriod = 5; // seconds
  fOutputDir = log_dir;
  fDB = (*fClient)[dbname];
  fCollection = fDB["log"];

  std::cout << "Local file logging to " << log_dir << std::endl;;
  fFlush = true;
  fFlushThread = std::thread(&MongoLog::Flusher, this);
  fRunId = -1;

  fLogLevel = 1;
}

MongoLog::~MongoLog(){
  fFlush = false;
  fFlushThread.join();
  fOutfile.close();
}

std::tuple<struct tm, int> MongoLog::Now() {
  using namespace std::chrono;
  auto now = system_clock::now();
  auto t = system_clock::to_time_t(now);
  int ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
  return {*std::gmtime(&t), ms};
}

void MongoLog::Flusher() {
  while (fFlush == true) {
    std::this_thread::sleep_for(std::chrono::seconds(fFlushPeriod));
    fMutex.lock();
    if (fOutfile.is_open()) fOutfile << std::flush;
    fMutex.unlock();
  }
}

std::string MongoLog::FormatTime(struct tm* date, int ms) {
  std::string out("YYYY-MM-DD HH:mm:SS.SSS");
  // this is kinda awkward but we can't use c++20's time-formatting gubbins so :(
  sprintf(out.data(), "%04i-%02i-%02i %02i:%02i:%02i.%03i", date->tm_year+1900,
      date->tm_mon+1, date->tm_mday, date->tm_hour, date->tm_min, date->tm_sec, ms);
  return out;
}

int MongoLog::Today(struct tm* date) {
  return (date->tm_year+1900)*10000 + (date->tm_mon+1)*100 + (date->tm_mday);
}

std::string MongoLog::LogFileName(struct tm* date) {
  return std::to_string(Today(date)) + "_" + fHostname + ".log";
}

fs::path MongoLog::LogFilePath(struct tm* date) {
  return OutputDirectory(date)/LogFileName(date);
}

fs::path MongoLog::OutputDirectory(struct tm*) {
  return fOutputDir;
}

int MongoLog::RotateLogFile() {
  if (fOutfile.is_open()) fOutfile.close();
  auto [today, ms] = Now();
  auto filename = LogFilePath(&today);
  std::cout<<"Logging to " << filename << std::endl;
  auto pp = filename.parent_path();
  if (!fs::exists(pp) && !fs::create_directories(pp)) {
    std::cout << "Could not create output directories for logging!" << std::endl;
    return -1;
  }
  fOutfile.open(filename, std::ofstream::out | std::ofstream::app);
  if (!fOutfile.is_open()) {
    std::cout << "Could not rotate logfile!\n";
    return -1;
  }
  fOutfile << FormatTime(&today, ms) << " [INIT]: logfile initialized: commit " << REDAX_BUILD_COMMIT << "\n";
  fToday = Today(&today);
  if (fDeleteAfterDays == 0) return 0;
  std::vector<int> days_per_month = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (today.tm_year%4 == 0) days_per_month[1] += 1; // the edge-case is SEP
  struct tm last_week = today;
  last_week.tm_mday -= fDeleteAfterDays;
  if (last_week.tm_mday <= 0) { // new month
    last_week.tm_mon--;
    if (last_week.tm_mon < 0) { // new year
      last_week.tm_year--;
      last_week.tm_mon = 11;
    }
    last_week.tm_mday += days_per_month[last_week.tm_mon]; // off by one error???
  }
  auto p = LogFileName(&last_week);
  if (std::experimental::filesystem::exists(p)) {
    fOutfile << FormatTime(&today, ms) << " [INIT]: Deleting " << p << '\n';
    std::experimental::filesystem::remove(p);
  } else {
    fOutfile << FormatTime(&today, ms) << " [INIT]: No older logfile to delete :(\n";
  }
  return 0;
}

int MongoLog::Entry(int priority, std::string message, ...){
  auto [today, ms] = Now();

  // Thanks Martin
  // http://www.martinbroadhurst.com/string-formatting-in-c.html
  va_list args;
  va_start (args, message); // First pass just gets what the length will be
  size_t len = std::vsnprintf(NULL, 0, message.c_str(), args);
  va_end (args);
  std::vector<char> vec(len + 1); // Declare with proper length
  va_start (args, message);  // Fill the vector we just made
  std::vsnprintf(&vec[0], len + 1, message.c_str(), args);
  va_end (args);
  message = &vec[0];

  std::stringstream msg;
  msg<<FormatTime(&today, ms)<<" ["<<fPriorities[priority+1] <<"]: "<<message<<std::endl;
  std::unique_lock<std::mutex> lg(fMutex);
  std::cout << msg.str();
  if (Today(&today) != fToday) RotateLogFile();
  fOutfile<<msg.str();
  if(priority >= fLogLevel){
    try{
      auto d = bsoncxx::builder::stream::document{} <<
        "user" << fHostname <<
        "message" << message <<
        "priority" << priority <<
        "runid" << fRunId <<
        bsoncxx::builder::stream::finalize;
      fCollection.insert_one(std::move(d));
    }
    catch(const std::exception &e){
      std::cout<<"Failed to insert log message "<<message<<" ("<<
	priority<<")"<<std::endl;
      return -1;
    }
  }
  return 0;
}


fs::path MongoLog_nT::OutputDirectory(struct tm* date) {
  char temp[6];
  std::sprintf(temp, "%02d.%02d", date->tm_mon+1, date->tm_mday);
  return fOutputDir / std::to_string(date->tm_year+1900) / std::string(temp);
}

std::string MongoLog_nT::LogFileName(struct tm*) {
  return fHostname + ".log";
}
