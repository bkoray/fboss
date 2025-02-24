/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "fboss/agent/hw/test/HwLinkStateToggler.h"

#include "fboss/agent/ApplyThriftConfig.h"
#include "fboss/agent/state/Port.h"

#include <folly/gen/Base.h>

namespace facebook {
namespace fboss {

void HwLinkStateToggler::linkStateChanged(PortID port, bool up) noexcept {
  {
    std::lock_guard<std::mutex> lk(linkEventMutex_);
    if (!portIdToWaitFor_ || port != portIdToWaitFor_ || up != waitForPortUp_) {
      return;
    }
    desiredPortEventOccurred_ = true;
    portIdToWaitFor_ = std::nullopt;
  }
  linkEventCV_.notify_one();
}

void HwLinkStateToggler::setPortIDAndStateToWaitFor(
    PortID port,
    bool waitForPortUp) {
  std::lock_guard<std::mutex> lk(linkEventMutex_);
  portIdToWaitFor_ = port;
  waitForPortUp_ = waitForPortUp;
  desiredPortEventOccurred_ = false;
}

void HwLinkStateToggler::portStateChangeImpl(
    std::shared_ptr<SwitchState> switchState,
    const std::vector<PortID>& ports,
    bool up) {
  auto newState = switchState;
  auto desiredLoopbackMode =
      up ? desiredLoopbackMode_ : cfg::PortLoopbackMode::NONE;
  for (auto port : ports) {
    if (newState->getPorts()->getPort(port)->getLoopbackMode() ==
        desiredLoopbackMode) {
      continue;
    }
    newState = newState->clone();
    auto newPort = newState->getPorts()->getPort(port)->modify(&newState);
    setPortIDAndStateToWaitFor(port, up);
    newPort->setLoopbackMode(desiredLoopbackMode);
    stateUpdateFn_(newState);
    invokeLinkScanIfNeeded(port, up);
    std::unique_lock<std::mutex> lock{linkEventMutex_};
    linkEventCV_.wait(lock, [this] { return desiredPortEventOccurred_; });
  }
}

void HwLinkStateToggler::applyInitialConfig(
    const std::shared_ptr<SwitchState>& curState,
    const Platform* platform,
    const cfg::SwitchConfig& initCfg) {
  auto cfg = initCfg;
  for (auto& port : cfg.ports) {
    // Set all port preemphasis values to 0 so that we can bring ports up and
    // down by setting their loopback mode to PHY and NONE respectively.
    setPortPreemphasis(PortID(port.logicalID), 0);
    // Bring ports down by setting loopback mode to NONE
    port.loopbackMode = cfg::PortLoopbackMode::NONE;
  }
  // i) Set preempahsis to 0, so ports state can be manipulated by just setting
  // loopback mode (lopbackMode::NONE == down), loopbackMode::{MAC, PHY} == up)
  // ii) Apply first config with all ports set to loopback mode as NONE
  // iii) Synchronously bring ports up. By doing this we are guaranteed to have
  // tided over, over the first set of linkscan events that come as a result of
  // init (since there are no portup events in init + initial config
  // application). iii) Start tests.
  auto newState = applyThriftConfig(curState, &cfg, platform);
  stateUpdateFn_(newState);
}

void HwLinkStateToggler::bringUpPorts(
    const std::shared_ptr<SwitchState>& newState,
    const cfg::SwitchConfig& initCfg) {
  std::vector<PortID> portsToBringUp;
  folly::gen::from(initCfg.ports) | folly::gen::filter([](const auto& port) {
    return port.state == cfg::PortState::ENABLED;
  }) |
      folly::gen::map([](const auto& port) { return PortID(port.logicalID); }) |
      folly::gen::appendTo(portsToBringUp);
  bringUpPorts(newState, portsToBringUp);
}

} // namespace fboss
} // namespace facebook
