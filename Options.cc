#include "Options.hh"
#include "DAXHelpers.hh"
#include "MongoLog.hh"
#include <algorithm>

#include <bsoncxx/array/view.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/exception/exception.hpp>

Options::Options(std::shared_ptr<MongoLog>& log, std::string options_name, std::string hostname,
          mongocxx::collection* opts_collection, std::shared_ptr<mongocxx::pool>& pool,
          std::string dbname, std::string override_opts) : 
    fLog(log), fHostname(hostname), fPool(pool), fClient(pool->acquire()), 
    fDAC_cache(bsoncxx::document::view()) {
  bson_value = NULL;
  if(Load(options_name, opts_collection, override_opts)!=0)
    throw std::runtime_error("Can't initialize options class");
  fDB = (*fClient)[dbname];
  fDAC_collection = fDB["dac_calibration"];
  int ref = -1;
  bool load_ref = GetString("baseline_dac_mode") == "cached" || GetNestedString("baseline_dac_mode."+fDetector) == "cached" || GetString("baseline_fallback_mode") == "cached";
  if (load_ref && ((ref = std::max(GetInt("baseline_reference_run"), GetNestedInt("baseline_reference_run."+fDetector))) == -1)) {
    // -1 is default return
    fLog->Entry(MongoLog::Error, "Please specify a reference run to use cached baselines");
    throw std::runtime_error("Config invalid");
  }
  if (load_ref && (ref != -1)) {
    auto doc = fDAC_collection.find_one(bsoncxx::builder::stream::document{} << "run" << ref << bsoncxx::builder::stream::finalize);
    if (doc) fDAC_cache = *doc;
    else {
      fLog->Entry(MongoLog::Warning, "Could not load baseline reference run %i", ref);
      throw std::runtime_error("Can't load cached baselines");
    }
    fLog->Entry(MongoLog::Local, "Loaded cached baselines from run %i", ref);
  }
}

Options::~Options(){
  if(bson_value != NULL) {
    delete bson_value;
    bson_value = NULL;
  }
}

int Options::Load(std::string name, mongocxx::collection* opts_collection, std::string override_opts) {
  using namespace bsoncxx::builder::stream;
  auto pl = mongocxx::pipeline();
  pl.match(document{} << "name" << name << finalize);
  pl.lookup(document{} << "from" << "options" << "localField" << "includes" <<
      "foreignField" << "name" << "as" << "subconfig" << finalize);
  pl.add_fields(document{} << "subconfig" << open_document << "$concatArrays" << open_array <<
      "$subconfig" << open_array << "$$ROOT" << close_array << close_array << close_document <<
      finalize);
  pl.unwind("$subconfig");
  pl.group(document{} << "_id" << 0 << "config" << open_document << "$mergeObjects" <<
      "$subconfig" << close_document << finalize);
  pl.replace_root(document{} << "newRoot" << "$config" << finalize);
  pl.project(document{} << "subconfig" << 0 << finalize);
  if (override_opts != "")
    pl.add_fields(bsoncxx::from_json(override_opts));
  for (auto doc : opts_collection->aggregate(pl)) {
    bson_value = new bsoncxx::document::value(doc);
    bson_options = bson_value->view();
    try{
      fDetector = bson_options["detectors"][fHostname].get_utf8().value.to_string();
    }catch(const std::exception& e){
      fLog->Entry(MongoLog::Warning, "No detector specified for this host");
      return -1;
    }
    return 0;
  }
  return -1;
}

long int Options::GetLongInt(std::string path, long int default_value){
  try{
    return bson_options[path.c_str()].get_int64();
  }
  catch (const std::exception &e){
    // Some APIs autoconvert big ints to doubles. Why? I don't know.
    // But we can handle this here rather than chase those silly things
    // around in each implementation.
    try{
      return (long int)(bson_options[path.c_str()].get_double());
    }
    catch(const std::exception &e){
      fLog->Entry(MongoLog::Local, "Using default value for %s", path.c_str());
      return default_value;
    }
  }
  return -1;
}

double Options::GetDouble(std::string path, double default_value) {
  try{
    return bson_options[path].get_double();
  } catch (const std::exception& e) {
    // maybe it's actually a long?
    try{
      return bson_options[path].get_int64();
    }catch(const std::exception& ee) {
      // an integer???
      try{
        return bson_options[path].get_int32();
      }catch(const std::exception& eee) {
        fLog->Entry(MongoLog::Local, "Using default value for %s", path.c_str());
        return default_value;
      }
    }
  }
}

int Options::GetInt(std::string path, int default_value){

  try{
    return bson_options[path].get_int32();
  }
  catch (const std::exception &e){
    fLog->Entry(MongoLog::Local, "Using default value for %s", path.c_str());
    return default_value;
  }
  return -1;
}

int Options::GetNestedInt(std::string path, int default_value){
  // Parse string
  std::vector<std::string> fields;
  std::stringstream ss(path);
  while( ss.good() ){
    std::string substr;
    getline( ss, substr, '.' );
    fields.push_back( substr );
  }
  try{
    auto val = bson_options[fields[0]];
    for(unsigned int i=1; i<fields.size(); i++)
      val = val[fields[i]];
    return val.get_int32();
  }catch(const std::exception &e){
    fLog->Entry(MongoLog::Local, "Using default value for %s",path.c_str());
    return default_value;
  }
  return 0;
}

std::string Options::GetString(std::string path, std::string default_value){
  try{
    return bson_options[path].get_utf8().value.to_string();
  }
  catch (const std::exception &e){
    //LOG
    fLog->Entry(MongoLog::Local, "Using default value for %s", path.c_str());
    return default_value;
  }
}

std::string Options::GetNestedString(std::string path, std::string default_value){
  // Parse string
  std::vector<std::string> fields;
  std::stringstream ss(path);
  while( ss.good() ){
    std::string substr;
    getline( ss, substr, '.' );
    fields.push_back( substr );
  }
  try{
    auto val = bson_options[fields[0]];
    for(unsigned int i=1; i<fields.size(); i++)
      val = val[fields[i]];
    return val.get_utf8().value.to_string();
  }catch(const std::exception &e){
    fLog->Entry(MongoLog::Local, "Using default value for %s",path.c_str());
    return default_value;
  }
  return 0;
}

std::vector<BoardType> Options::GetBoards(std::string type){
  std::vector<BoardType> ret;
  bsoncxx::array::view subarr = bson_options["boards"].get_array().value;

  std::vector <std::string> types;
  if(type == "V17XX")
    types = {"V1724", "V1730", "V1724_MV", "f1724"};
  else if (type == "V27XX")
    types = {"V2718", "f2718"};
  else if (type == "V1495")
    types = {"V1495", "V1495_TPC"};
  else
    types.push_back(type);

  for(bsoncxx::array::element ele : subarr){
    std::string btype = ele["type"].get_utf8().value.to_string();
    if(!std::count(types.begin(), types.end(), btype))
      continue;
    try{
      if(ele["host"].get_utf8().value.to_string() != fHostname)
        continue;
    }
    catch(const std::exception &e){
      // If there is no host field then no biggie. Assume we have just 1 host.
    };
    BoardType bt;
    bt.link = ele["link"].get_int32();
    bt.crate = ele["crate"].get_int32();
    bt.board = ele["board"].get_int32();
    bt.type = ele["type"].get_utf8().value.to_string();
    bt.vme_address = DAXHelpers::StringToHex(ele["vme_address"].get_utf8().value.to_string());
    ret.push_back(bt);
  }

  return ret;
}

std::vector<RegisterType> Options::GetRegisters(int board, bool strict){
  std::vector<RegisterType> ret;
  int ibid = 0;
  std::string sdet = "";
  bsoncxx::array::view regarr = bson_options["registers"].get_array().value;
  for(bsoncxx::array::element ele : regarr){
    try{
      ibid = ele["board"].get_int32();
      sdet = "";
    }catch(const std::exception& e){
      try{
        sdet = ele["board"].get_utf8().value.to_string();
        ibid = -1;
      }catch(const std::exception& ee){
        throw std::runtime_error("Invalid register: board is neither int nor string");
      }
    }
    if ((ibid != board) && strict) continue;
    if ((ibid == board) || (sdet == fDetector) || (sdet == "all")) {
      RegisterType rt;
      rt.reg = ele["reg"].get_utf8().value.to_string();
      rt.val = ele["val"].get_utf8().value.to_string();

      ret.push_back(rt);
    }
  }
  return ret;
}

std::vector<u_int16_t> Options::GetThresholds(int board) {
  std::vector<u_int16_t> thresholds;
  u_int16_t default_threshold = 0xA;
  try{
    for (auto& val : bson_options["thresholds"][std::to_string(board)].get_array().value)
      thresholds.push_back(val.get_int32().value);
    return thresholds;
  }
  catch(std::exception& e){
    fLog->Entry(MongoLog::Local, "Using default thresholds for %i", board);
    return std::vector<u_int16_t>(16, default_threshold);
  }
}

std::vector<int> Options::GetBLTalloc() {
  int default_value = 23;
  std::vector<int> ret;
  try{
    for (auto& val : bson_options["blt_alloc"].get_array().value)
      ret.push_back(val.get_int32().value);
  } catch(std::exception& e) {
    fLog->Entry(MongoLog::Local, "Using default BLT allocs");
    ret.push_back(default_value);
  }
  return ret;
}

int Options::GetV1495Opts(std::map<std::string, int>& ret) {
  if (bson_options.find("V1495") == bson_options.end())
    return 1;
  auto subdoc = bson_options["V1495"].get_document().value;
  if (subdoc.find(fDetector) == subdoc.end())
    return 1;
  try {
    for (auto& value : subdoc[fDetector].get_document().value)
      ret[std::string(value.key())] = value.get_int32().value;  // TODO std::any
    return 0;
  } catch (std::exception& e) {
    fLog->Entry(MongoLog::Local, "Exception getting V1495 opts: %s", e.what());
    return -1;
  }
  return 1;
}

int Options::GetCrateOpt(CrateOptions &ret){
  if ((ret.pulser_freq = GetNestedInt("V2718."+fDetector+".pulser_freq", -1)) == -1) {
    try{
      ret.pulser_freq = bson_options["V2718"][fDetector]["pulser_freq"].get_double().value;
    } catch(std::exception& e) {
      ret.pulser_freq = 0;
    }
  }
  ret.s_in = GetNestedInt("V2718."+fDetector+".s_in", 0);
  ret.muon_veto = GetNestedInt("V2718."+fDetector+".muon_veto", 0);
  ret.neutron_veto = GetNestedInt("V2718."+fDetector+".neutron_veto", 0);
  ret.led_trigger = GetNestedInt("V2718."+fDetector+".led_trigger", 0);
  return 0;
}

int16_t Options::GetChannel(int bid, int cid){
  std::string boardstring = std::to_string(bid);
  try{
    return bson_options["channels"][boardstring][cid].get_int32().value;
  }
  catch(std::exception& e){
    fLog->Entry(MongoLog::Local, "Failed to look up board %i ch %i", bid, cid);
    return -1;
  }
}

int Options::GetHEVOpt(HEVOptions &ret){
  try{
    ret.signal_threshold = bson_options["DDC10"]["signal_threshold"].get_int32().value;
    ret.sign = bson_options["DDC10"]["sign"].get_int32().value;
    ret.rise_time_cut = bson_options["DDC10"]["rise_time_cut"].get_int32().value;
    ret.dynamic_veto_limit = bson_options["DDC10"]["dynamic_veto_limit"].get_int32().value;
    ret.static_veto_duration = bson_options["DDC10"]["static_veto_duration"].get_int32().value;
    ret.integration_threshold = bson_options["DDC10"]["integration_threshold"].get_int32().value;
    ret.rho_0 = bson_options["DDC10"]["rho_0"].get_int32().value;
    ret.rho_1 = bson_options["DDC10"]["rho_1"].get_int32().value;
    ret.rho_2 = bson_options["DDC10"]["rho_2"].get_int32().value;
    ret.rho_3 = bson_options["DDC10"]["rho_3"].get_int32().value;
    ret.window = bson_options["DDC10"]["window"].get_int32().value;
    ret.prescaling = bson_options["DDC10"]["prescaling"].get_int32().value;
    ret.component_status = bson_options["DDC10"]["component_status"].get_int32().value;
    ret.width_cut = bson_options["DDC10"]["width_cut"].get_int32().value;
    ret.delay = bson_options["DDC10"]["delay"].get_int32().value;
    ret.address = bson_options["DDC10"]["address"].get_utf8().value.to_string();
    ret.required = bson_options["DDC10"]["required"].get_utf8().value.to_string();
  }catch(std::exception &E){
    fLog->Entry(MongoLog::Local, "Exception getting DDC10 opts: %s",E.what());
    return -1;
  }
  return 0;
}

int Options::GetFaxOptions(fax_options_t& opts) {
  try {
    auto doc = bson_options["fax_options"];
    opts.rate = doc["rate"].get_double().value;
    opts.tpc_size = doc["tpc_size"].get_int32().value;
    opts.drift_speed = doc["drift_speed"].get_double().value;
    opts.e_absorbtion_length = doc["e_absorbtion_length"].get_double().value;
  } catch (std::exception& e) {
    fLog->Entry(MongoLog::Warning, "Error getting fax options: %s", e.what());
    return -1;
  }
  return 0;
}

std::vector<uint16_t> Options::GetDAC(int bid, int num_chan, uint16_t default_value) {
  std::vector<uint16_t> ret(num_chan, default_value);
  auto doc = fDAC_cache.view();
  if (doc.find(std::to_string(bid)) == doc.end()) {
    fLog->Entry(MongoLog::Message, "No cached baselines for board %i, using default %04x",
        bid, default_value);
    return ret;
  }
/* doc should look like this:
 *{ run : 42,
 * bid : [ val, val, val, ...],
 *         ...
 * }
 */
  for (int i = 0; i < num_chan; i++)
    ret[i] = doc[std::to_string(bid)][i].get_int32().value;
  return ret;
}

uint16_t Options::GetSingleDAC(int bid, int ch, uint16_t default_value) {
  auto doc = fDAC_cache.view();
  if (doc.find(std::to_string(bid)) == doc.end()) {
    fLog->Entry(MongoLog::Message, "No cached baselines for board %i, using default %04x",
        bid, default_value);
    return default_value;
  }
  return doc[std::to_string(bid)][ch].get_int32().value;
}

void Options::UpdateDAC(std::map<int, std::vector<uint16_t>>& all_dacs){
  using namespace bsoncxx::builder::stream;
  int run_id = GetInt("number", -1);
  fLog->Entry(MongoLog::Local, "Saving DAC calibration");
  auto search_doc = document{} << "run" << run_id << finalize;
  auto update_doc = document{};
  update_doc << "$set" << open_document << "run" << run_id;
  for (auto& bid_map : all_dacs) { // (bid, vector)
    update_doc << std::to_string(bid_map.first) << open_array <<
      [&](array_context<> arr){
        for (auto& val : bid_map.second) arr << val;
      } << close_array;
  }
  update_doc << close_document;
  auto write_doc = update_doc << finalize;
  mongocxx::options::update options;
  options.upsert(true);
  fDAC_collection.update_one(std::move(search_doc), std::move(write_doc), options);
  return;
}

