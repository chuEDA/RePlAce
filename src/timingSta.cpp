
#include "Machine.hh"
#include "Network.hh"
#include "Parasitics.hh"
#include "Corner.hh"
#include "timing.h"
#include "timingSta.h"

#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <chrono>

#include <tcl.h>

using namespace sta;
using namespace std;

//static const char *
//escapeDividers(const char *token,
//             const sta::Network *network);
static float 
GetMaxResistor(sta::Sta* sta, sta::Net* net);

namespace Timing { 

inline string Timing::GetPinName(PIN* curPin, bool isEscape) {
  // itself is PINS in def.
  if(curPin->term && _terms[curPin->moduleID].isTerminalNI) {
    return string(_terms[curPin->moduleID].Name());
  }

  // below is common
  string name = (curPin->term) ? string(_terms[curPin->moduleID].Name())
                               : string(_modules[curPin->moduleID].Name());

  if( isEscape ) {
    SetEscapedStr(name);
  }


  // bookshelf cases, it must be empty
  if(_mPinName.size() == 0 && _tPinName.size() == 0) {
    string pinPrefix = (curPin->IO == 0) ? "I" : "O";
    return name + "/" + pinPrefix + to_string(curPin->pinIDinModule);
  }
  // other LEF/DEF/VERILOG cases.
  else {
    return (curPin->term == 0)
               ? name + "/" + _mPinName[curPin->moduleID][curPin->pinIDinModule]
               : name + "/" +
                     _tPinName[curPin->moduleID][curPin->pinIDinModule];
  }
}

inline string Timing::GetPinName(PinInfo& curPin, bool isEscape) {
  if(curPin.isSteiner()) {
    return curPin.GetStnPinName(isEscape);
  }

  return (curPin.isModule()) ? 
    curPin.GetPinName((void*)_modules, _mPinName, isEscape)
    : curPin.GetPinName((void*)_terms, _tPinName, isEscape);
}


void Timing::ExecuteStaFirst() {
  MakeParasiticsForSta(); 

  // find_timing -full_update (true->full, false-> incremental)
  UpdateTimingSta();

  UpdateNetWeightSta();

  // WNS / TNS report
  const MinMax* cnst_min_max;
  cnst_min_max = MinMax::max();

  Slack wns; 
  Vertex *worstVertex;
  _sta->worstSlack(cnst_min_max, wns, worstVertex);

  Slack tns = _sta->totalNegativeSlack(cnst_min_max);
  PrintInfoPrecSignificant("Timing: WNS", wns);
  PrintInfoPrecSignificant("Timing: TNS", tns);
  globalWns = wns;
  globalTns = tns;

  float tol = 0.0;
  _sta->setIncrementalDelayTolerance(tol);
}

void Timing::ExecuteStaLater() {
  for(int i = 0; i < _netCnt; i++) {
    _nets[i].timingWeight = 0;
  }
  // _sta->parasitics()->deleteParasitics();
  // _sta->network()->clear();

  auto start = std::chrono::steady_clock::now();
  MakeParasiticsForSta(); 
  auto finish = std::chrono::steady_clock::now();
  double elapsed_seconds =
      std::chrono::duration_cast< std::chrono::duration< double > >(finish -
                                                                    start)
          .count();

  PrintInfoRuntime("Timing: FillParasitcsForSta", elapsed_seconds, 1);

  _sta->setIncrementalDelayTolerance(1e-6);

  start = std::chrono::steady_clock::now();
  UpdateTimingSta();
  finish = std::chrono::steady_clock::now();

  elapsed_seconds =
      std::chrono::duration_cast< std::chrono::duration< double > >(finish -
                                                                    start)
          .count();

  PrintInfoRuntime("Timing: UpdateTimingSta", elapsed_seconds, 1);

  start = std::chrono::steady_clock::now();
  UpdateNetWeightSta();
  finish = std::chrono::steady_clock::now();
  elapsed_seconds =
      std::chrono::duration_cast< std::chrono::duration< double > >(finish -
                                                                    start)
          .count();
  PrintInfoRuntime("Timing: UpdateNetWeight", elapsed_seconds, 1);

  // WNS / TNS report
  const MinMax* cnst_min_max;
  cnst_min_max = MinMax::max();
  Slack wns; 
  Vertex *worstVertex;
  _sta->worstSlack(cnst_min_max, wns, worstVertex);
  Slack tns = _sta->totalNegativeSlack(cnst_min_max);
  
  PrintInfoPrecSignificant("Timing: WNS", wns);
  PrintInfoPrecSignificant("Timing: TNS", tns);
  globalWns = wns;
  globalTns = tns;
}

char* GetNewStr(const char* inp) {
  char* ret = new char[strlen(inp) + 1];
  strcpy(ret, inp);
  return ret;
}

// 
// Fill OpenSTA's parasitic models to have Cap / Res from FLUTE.
//
void Timing::MakeParasiticsForSta() {
  sta::Network* network = _sta->network();
  sta::Parasitics* parasitics = _sta->parasitics();

  std::unordered_map< PinInfo, bool, PinInfoHash, PinInfoEqual > pin_cap_written;
  // map from pin name -> cap
  std::unordered_map< PinInfo, double, PinInfoHash, PinInfoEqual > lumped_cap_at_pin;


  // 1. calc. lump sum caps from wire segments (PI2-model) + load caps
  for(int i = 0; i < _netCnt; i++) {
    for(auto& curWireSeg : wireSegStor[i]) {
      lumpedCapStor[i] += curWireSeg.length / (double)(GetDefDbu())*capPerMicron;
      lumped_cap_at_pin[curWireSeg.iPin] +=
          curWireSeg.length / (double)(GetDefDbu())*capPerMicron * 0.5;
      lumped_cap_at_pin[curWireSeg.oPin] +=
          curWireSeg.length / (double)(GetDefDbu())*capPerMicron * 0.5;
      pin_cap_written[curWireSeg.iPin] = false;
      pin_cap_written[curWireSeg.oPin] = false;
    }

    for(int j = 0; j < _nets[i].pinCNTinObject; j++) {
      PIN* curPin = _nets[i].pin[j];
      if(curPin->term && _terms[curPin->moduleID].isTerminalNI &&
         curPin->IO == 1) {
        // This must be read from SDC file,
        // but in ICCAD contest, only outPin have PIN_CAP
        // as 4e-15
        lumpedCapStor[i] += TIMING_PIN_CAP;
        lumped_cap_at_pin[PinInfo(curPin)] += TIMING_PIN_CAP;
      }
    }
  }

  
  // get maximum pin counts in current sta network structure:
  int newPinIdx = network->pinCount()+1;

  const sta::Corner* corner = _sta->corners()->findCorner(0);
  _sta->corners()->makeParasiticAnalysisPtsMinMax();

  const sta::MinMax* min_max = MinMax::max();
  const sta::ParasiticAnalysisPt *ap = corner->findParasiticAnalysisPt(min_max); 
  
//  cout << "corner: " << corner << endl;
//  cout << "min_max: " << min_max << endl;
//  cout << "ap: " << ap << endl;

  // for each net
  for(int i=0; i<_netCnt; i++) {
    NET* curNet = &netInstance[i];
//    cout << "run: " << i << " " << curNet->Name() << endl;
    sta::Net* curStaNet = network->findNet( curNet->Name() );
    if( !curStaNet ) {
      cout << "cannot find: " << curNet->Name() << endl;
      cout << "Verilog and DEF are mismatched. Please check your input" << endl;
      exit(1);
    }

    Parasitic* parasitic = parasitics->makeParasiticNetwork(curStaNet, false, ap);
    for(auto& curSeg : wireSegStor[i]) {
      // check for IPin cap
      
      ParasiticNode *n1 = NULL, *n2 = NULL;

      // existed pin cases
      if( !curSeg.iPin.isSteiner() ) {
        string pinName = GetPinName(curSeg.iPin, false);
        sta::Pin* pin = network->findPin(pinName.c_str());
        if( !pin ) {
          cout << "cannot find: " << pinName << " in " 
            << curNet->Name() << " net." << endl;
          cout << "Verilog and DEF are mismatched. Please check your input" << endl;
          exit(1);
        }
        n1 = parasitics->ensureParasiticNode(parasitic, pin);
      }
      // virtual steinerPin cases
      // Set steiner pins' index as newPinIdx+1, newPinIdx+2, ....
      else {
        n1 = parasitics->ensureParasiticNode(parasitic, curStaNet, 
            newPinIdx + curSeg.iPin.GetPinNum());
      }

      // insert cap
      if(!pin_cap_written[curSeg.iPin]) {
        parasitics->incrCap(n1, lumped_cap_at_pin[curSeg.iPin], ap);
        pin_cap_written[curSeg.iPin] = true;
      }
      
      // existed pin cases
      if( !curSeg.oPin.isSteiner() ) {
        string pinName = GetPinName(curSeg.oPin, false);
        sta::Pin* pin = network->findPin(pinName.c_str());
        if( !pin ) {
          cout << "cannot find: " << curNet->Name() << ":" << pinName << endl;
          cout << "Verilog and DEF are mismatched. Please check your input" << endl;
          exit(1);
        }
        n2 = parasitics->ensureParasiticNode(parasitic, pin);
      }
      // virtual steinerPin cases
      // Set steiner pins' index as newPinIdx+1, newPinIdx+2, ....
      else {
        n2 = parasitics->ensureParasiticNode(parasitic, curStaNet, 
            newPinIdx + curSeg.oPin.GetPinNum());
      }

      if(!pin_cap_written[curSeg.oPin]) {
        parasitics->incrCap(n2, lumped_cap_at_pin[curSeg.oPin], ap);
        pin_cap_written[curSeg.oPin] = true;
      }

      // insert resistor.
      parasitics->makeResistor(nullptr, n1, n2, 
          curSeg.length / static_cast<double>(GetDefDbu()) * resPerMicron, ap);
      
    }
  }
  _sta->delaysInvalid();
}


void Timing::UpdateTimingSta() {
  _sta->updateTiming(true);
}


void Timing::UpdateNetWeightSta() {
  // To enable scaling 
  // boundary values
  netWeightMin = FLT_MAX;
  netWeightMax = FLT_MIN;
 
  // extract WNS 
  Slack wns; 
  Vertex* worstVertex = NULL;
  
  const MinMax* cnst_min_max = MinMax::max();
  _sta->worstSlack(cnst_min_max, wns, worstVertex);

//  cout << "WNS: " << wns << endl;

  float minRes = FLT_MAX;
  float maxRes = FLT_MIN;

  // for normalize
  for(int i=0; i<_netCnt; i++) {
    NET* curNet = &_nets[i];
    sta::Net* curStaNet = _sta->network()->findNet(curNet->Name());
    if( !curStaNet ) {
      cout << "cannot find: " << curNet->Name() << endl;
      cout << "Verilog and DEF are mismatched. Please check your input" << endl;
      exit(1);
    }
    float netRes = GetMaxResistor(_sta, curStaNet);
    minRes = (minRes > netRes)? netRes : minRes;
    maxRes = (maxRes < netRes)? netRes : maxRes;
  }
    

  // for all nets
  for(int i=0; i<_netCnt; i++) {
    NET* curNet = &_nets[i];
    sta::Net* curStaNet = _sta->network()->findNet(curNet->Name());
    if( !curStaNet ) {
      cout << "cannot find: " << curNet->Name() << endl;
      cout << "Verilog and DEF are mismatched. Please check your input" << endl;
      exit(1);
    }

    
    float netSlack = _sta->netSlack(curStaNet, cnst_min_max);
    netSlack = (fabs(netSlack - MinMax::min()->initValue()) <= FLT_EPSILON) ? 
      0 : netSlack;

    float criticality = (wns>0)? 0 : max(0.0f, netSlack / wns);

//    cout << "diff: " << fabs(netSlack - MinMax::min()->initValue()) << endl;
//    cout << curNet->Name() << " netSlack: " << netSlack << " crit: " << criticality;

    // get normalized resistor
    float netRes = GetMaxResistor(_sta, curStaNet);
    float normRes = (netRes - minRes)/(maxRes - minRes);

    int netDegree = max(2, netInstance[i].pinCNTinObject);
    float netWeight = 1 + normRes * (1 + criticality) / (netDegree - 1);


    // TODO
    // following two lines are temporal magic codes at this moment.
    // Need to be replaced/tuned later
    netWeight = (netWeight >= 1.9)? 1.9 : netWeight;
    netWeight = (netSlack < 0)? 1.8 : 1;

//    cout << " normRes: " << normRes << " deg: " << netDegree 
//      << " nw: " << netWeight << endl;

    // update timingWeight 
    netInstance[i].timingWeight = netWeight;

    // update netWeightMin / netWeightMax    
    netWeightMin = (netWeightMin < netWeight) ? netWeightMin : netWeight;
    netWeightMax = (netWeightMax > netWeight) ? netWeightMax : netWeight;
  }
}

}

// static void ExecuteCommand( const char* inp ){
//    return system(inp);
//}

// static std::string ExecuteCommand(const char* cmd) {
//   cout << "COMMAND: " << cmd << endl;
//   std::array< char, 128 > buffer;
//   std::string result;
//   std::shared_ptr< FILE > pipe(popen(cmd, "r"), pclose);
//   if(!pipe)
//     throw std::runtime_error("popen() failed!");
//   while(!feof(pipe.get())) {
//     if(fgets(buffer.data(), 128, pipe.get()) != nullptr)
//       result += buffer.data();
//   }
//   return result;
// }

static float 
GetMaxResistor(sta::Sta* sta, sta::Net* net) {
  float retRes = 0.0f;

  const MinMax* cnst_min_max = MinMaxAll::max()->asMinMax();
  ParasiticAnalysisPt* ap =
      sta->corners()->findCorner(0)->findParasiticAnalysisPt(cnst_min_max);

  // find parasitic object from Net
  Parasitics* parasitics = sta->parasitics();
  Parasitic* parasitic = parasitics->findParasiticNetwork(net, ap);

  // Resistor Traverse from Net
  ParasiticDeviceIterator* paraDevIter = parasitics->deviceIterator(parasitic);

  while(paraDevIter->hasNext()) {
    ParasiticDevice* paraDev = paraDevIter->next();
    if(!parasitics->isResistor(paraDev)) {
      continue;
    }

    float newRes = parasitics->value(paraDev, ap);
    retRes = (retRes < newRes) ? newRes : retRes;
  }
  return retRes;
}

//static const char *
//escapeDividers(const char *token,
//             const sta::Network *network)
//{
//  return sta::escapeChars(token, network->pathDivider(), '\0',
//               network->pathEscape());
//}

