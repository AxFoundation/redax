#ifndef _DAXHELPERS_HH_
#define _DAXHELPERS_HH_

#include <string>
#include <sstream>

class DAXHelpers{
  /* 
     Class to include simple helper functions such as
     unit conversions and the like.
  */
  
public:
  DAXHelpers(){};
  ~DAXHelpers(){};

static unsigned int StringToHexOld(std::string str){
  // This function takes ~360ns to run
    std::stringstream ss(str);
    u_int32_t result;
    return ss >> std::hex >> result ? result : 0;
};

static unsigned int StringToHex(const std::string& str) {
  // this function takes ~12ns to run
  uint32_t result = 0;
  int i = 0;
  for (auto it = str.rbegin(); it != str.rend(); ++it) {
    bool is_letter = *it & 0x40; // see ascii
    int val = (!is_letter)*(*it-'0') + is_letter*(9+(*it & 0xF)); // branchless is 2x faster
    result += (val << 4*i++); // nibble by nibble
  }
  return result;
}

  const static int Idle    = 0;
  const static int Arming  = 1;
  const static int Armed   = 2;
  const static int Running = 3;
  const static int Error   = 4;
  const static int Unknown = 5;
  
private:

};

#endif
