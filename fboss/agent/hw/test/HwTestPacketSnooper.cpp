// Copyright 2004-present Facebook. All Rights Reserved.

#include "fboss/agent/hw/test/HwTestPacketSnooper.h"
#include "fboss/agent/RxPacket.h"

#include <folly/logging/xlog.h>

namespace facebook {
namespace fboss {

HwTestPacketSnooper::HwTestPacketSnooper(HwSwitchEnsemble* ensemble)
    : ensemble_(ensemble) {
  ensemble_->addHwEventObserver(this);
}

HwTestPacketSnooper::~HwTestPacketSnooper() {
  ensemble_->removeHwEventObserver(this);
}

void HwTestPacketSnooper::packetReceived(RxPacket* pkt) noexcept {
  std::lock_guard<std::mutex> lock(mtx_);
  data_ = pkt->buf()->clone();
  cv_.notify_all();
}

std::optional<utility::EthFrame> HwTestPacketSnooper::waitForPacket() {
  std::unique_lock<std::mutex> lock(mtx_);
  while (!data_) {
    cv_.wait(lock);
  }
  folly::io::Cursor cursor{data_.get()};
  return utility::EthFrame{cursor};
}

} // namespace fboss
} // namespace facebook
