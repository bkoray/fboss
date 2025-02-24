#include "fboss/qsfp_service/platforms/wedge/WedgeManager.h"

#include <folly/gen/Base.h>

#include <folly/logging/xlog.h>
#include "fboss/qsfp_service/platforms/wedge/WedgeQsfp.h"
#include "fboss/qsfp_service/sff/QsfpModule.h"

namespace facebook { namespace fboss {

WedgeManager::WedgeManager(std::unique_ptr<TransceiverPlatformApi> api) :
  qsfpPlatApi_(std::move(api)) {
  /* Constructor for WedgeManager class:
   * Get the TransceiverPlatformApi object from the creator of this object,
   * this object will be used for controlling the QSFP devices on board.
   * Going foward the qsfpPlatApi_ will be used to controll the QSFP devices
   * on FPGA managed platforms and the wedgeI2cBus_ will be used to control
   * the QSFP devices on I2C/CPLD managed platforms
   */
}

void WedgeManager::initTransceiverMap() {
  // If we can't get access to the USB devices, don't bother to
  // create the QSFP objects;  this is likely to be a permanent
  // error.
  try {
    wedgeI2cBus_ = getI2CBus();
  } catch (const I2cError& ex) {
    XLOG(ERR) << "failed to initialize I2C interface: " << ex.what();
    return;
  }

  // Wedge port 0 is the CPU port, so the first port associated with
  // a QSFP+ is port 1.  We start the transceiver IDs with 0, though.
  for (int idx = 0; idx < getNumQsfpModules(); idx++) {
    auto qsfpImpl = std::make_unique<WedgeQsfp>(idx, wedgeI2cBus_.get());
    auto qsfp = std::make_unique<QsfpModule>(
        std::move(qsfpImpl), numPortsPerTransceiver());
    transceivers_.push_back(move(qsfp));
    XLOG(INFO) << "making QSFP for " << idx;
  }

  refreshTransceivers();
}

void WedgeManager::getTransceiversInfo(std::map<int32_t, TransceiverInfo>& info,
    std::unique_ptr<std::vector<int32_t>> ids) {
  XLOG(INFO) << "Received request for getTransceiverInfo, with ids: "
             << (ids->size() > 0 ? folly::join(",", *ids) : "None");
  if (ids->empty()) {
    folly::gen::range(0, getNumQsfpModules()) |
      folly::gen::appendTo(*ids);
  }

  for (const auto& i : *ids) {
    TransceiverInfo trans;
    if (isValidTransceiver(i)) {
      try {
        trans = transceivers_[TransceiverID(i)]->getTransceiverInfo();
      } catch (const std::exception& ex) {
        XLOG(ERR) << "Transceiver " << i
                  << ": Error calling getTransceiverInfo(): " << ex.what();
      }
    }
    info[i] = trans;
  }
}

void WedgeManager::getTransceiversRawDOMData(
    std::map<int32_t, RawDOMData>& info,
    std::unique_ptr<std::vector<int32_t>> ids) {
  XLOG(INFO) << "Received request for getTransceiversRawDOMData, with ids: "
             << (ids->size() > 0 ? folly::join(",", *ids) : "None");
  if (ids->empty()) {
    folly::gen::range(0, getNumQsfpModules()) |
      folly::gen::appendTo(*ids);
  }
  for (const auto& i : *ids) {
    RawDOMData data;
    if (isValidTransceiver(i)) {
      try {
        data = transceivers_[TransceiverID(i)]->getRawDOMData();
      } catch (const std::exception& ex) {
        XLOG(ERR) << "Transceiver " << i
                  << ": Error calling getRawDOMData(): " << ex.what();
      }
    }
    info[i] = data;
  }
}

void WedgeManager::customizeTransceiver(int32_t idx, cfg::PortSpeed speed) {
  transceivers_.at(idx)->customizeTransceiver(speed);
}

void WedgeManager::syncPorts(
    std::map<int32_t, TransceiverInfo>& info,
    std::unique_ptr<std::map<int32_t, PortStatus>> ports) {
  auto groups =
      folly::gen::from(*ports) |
      folly::gen::filter([](const std::pair<int32_t, PortStatus>& item) {
        return item.second.__isset.transceiverIdx;
      }) |
      folly::gen::groupBy([](const std::pair<int32_t, PortStatus>& item) {
        return item.second.transceiverIdx_ref().value_unchecked().transceiverId;
      }) |
      folly::gen::as<std::vector>();

  for (auto& group : groups) {
    int32_t transceiverIdx = group.key();
    XLOG(INFO) << "Syncing ports of transceiver " << transceiverIdx;

    try {
      auto transceiver = transceivers_.at(transceiverIdx).get();
      transceiver->transceiverPortsChanged(group.values());
      info[transceiverIdx] = transceiver->getTransceiverInfo();
    } catch (const std::exception& ex) {
      XLOG(ERR) << "Transceiver " << transceiverIdx
                << ": Error calling syncPorts(): " << ex.what();
    }
  }
}

void WedgeManager::refreshTransceivers() {
  try {
    wedgeI2cBus_->verifyBus(false);
  } catch (const std::exception& ex) {
    XLOG(ERR) << "Error calling verifyBus(): " << ex.what();
    return;
  }

  std::vector<folly::Future<folly::Unit>> futs;
  XLOG(INFO) << "Start refreshing all transceivers...";

  for (const auto& transceiver : transceivers_) {
    XLOG(DBG3) << "Fired to refresh transceiver " << transceiver->getID();
    futs.push_back(transceiver->futureRefresh());
  }

  folly::collectAll(futs.begin(), futs.end()).wait();
  XLOG(INFO) << "Finished refreshing all transceivers";
}

int WedgeManager::scanTransceiverPresence(
    std::unique_ptr<std::vector<int32_t>> ids) {
  // If the id list is empty, we default to scan the presence of all the
  // transcievers.
  if (ids->empty()) {
    folly::gen::range(0, getNumQsfpModules()) | folly::gen::appendTo(*ids);
  }

  std::map<int32_t, ModulePresence> presenceUpdate;
  for (auto id : *ids) {
    presenceUpdate[id] = ModulePresence::UNKNOWN;
  }

  wedgeI2cBus_->scanPresence(presenceUpdate);

  int numTransceiversUp = 0;
  for (const auto& presence : presenceUpdate) {
    if (presence.second == ModulePresence::PRESENT) {
      numTransceiversUp++;
    }
  }
  return numTransceiversUp;
}

std::unique_ptr<TransceiverI2CApi> WedgeManager::getI2CBus() {
  return std::make_unique<WedgeI2CBusLock>(std::make_unique<WedgeI2CBus>());
}

}} // facebook::fboss
