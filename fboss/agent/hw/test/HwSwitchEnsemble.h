/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#pragma once

#include "fboss/agent/HwSwitch.h"
#include "fboss/agent/hw/gen-cpp2/hardware_stats_types.h"
#include "fboss/agent/platforms/test_platforms/TestPlatformTypes.h"
#include "fboss/agent/rib/RoutingInformationBase.h"
#include "fboss/agent/types.h"

#include <folly/Synchronized.h>

#include <memory>
#include <thread>

namespace facebook {
namespace fboss {

class Platform;
class SwitchState;
class HwLinkStateToggler;

class HwSwitchEnsemble : public HwSwitch::Callback {
 public:
  class HwSwitchEventObserverIf {
   public:
    virtual ~HwSwitchEventObserverIf() = default;
    virtual void packetReceived(RxPacket* pkt) noexcept = 0;
    virtual void linkStateChanged(PortID port, bool up) = 0;
    virtual void l2LearningUpdateReceived(
        L2Entry l2Entry,
        L2EntryUpdateType l2EntryUpdateType) = 0;
  };
  explicit HwSwitchEnsemble(uint32_t featuresDesired);
  ~HwSwitchEnsemble() override;
  /*
   * Revert back to post init state. Can be used by each
   * benchmark iteration to start with a clean slate.
   */
  void revertToInitCfgState();
  void setAllowPartialStateApplication(bool allow) {
    allowPartialStateApplication_ = allow;
  }
  std::shared_ptr<SwitchState> applyNewState(
      std::shared_ptr<SwitchState> newState);
  void applyInitialConfigAndBringUpPorts(const cfg::SwitchConfig& cfg);

  std::shared_ptr<SwitchState> getProgrammedState() const;
  HwLinkStateToggler* getLinkToggler() {
    return linkToggler_.get();
  }
  const rib::RoutingInformationBase* getRib() const {
    return routingInformationBase_.get();
  }
  rib::RoutingInformationBase* getRib() {
    return routingInformationBase_.get();
  }
  virtual Platform* getPlatform() {
    return platform_.get();
  }
  virtual const Platform* getPlatform() const {
    return platform_.get();
  }
  virtual HwSwitch* getHwSwitch() {
    return hwSwitch_.get();
  }
  virtual const HwSwitch* getHwSwitch() const {
    return hwSwitch_.get();
  }
  void packetReceived(std::unique_ptr<RxPacket> pkt) noexcept override;
  void linkStateChanged(PortID port, bool up) override;
  void l2LearningUpdateReceived(
      L2Entry l2Entry,
      L2EntryUpdateType l2EntryUpdateType) override;
  void exitFatal() const noexcept override {}
  void addHwEventObserver(HwSwitchEventObserverIf* observer);
  void removeHwEventObserver(HwSwitchEventObserverIf* observer);

  virtual std::vector<PortID> logicalPortIds() const = 0;
  virtual std::vector<PortID> masterLogicalPortIds() const = 0;
  virtual std::vector<PortID> getAllPortsinGroup(PortID portID) const = 0;
  virtual std::vector<FlexPortMode> getSupportedFlexPortModes() const = 0;

  virtual void dumpHwCounters() const = 0;
  /*
   * Get latest port stats for given ports
   */
  virtual std::map<PortID, HwPortStats> getLatestPortStats(
      const std::vector<PortID>& ports) = 0;
  /*
   * For platforms that support hw call logging, API to stop this logging
   */
  virtual void stopHwCallLogging() const = 0;
  /*
   * Initiate graceful exit
   */
  void gracefulExit();

 protected:
  /*
   * Setup ensemble
   */
  void setupEnsemble(
      std::unique_ptr<Platform> platform,
      std::unique_ptr<HwSwitch> hwSwitch,
      std::unique_ptr<HwLinkStateToggler> linkToggler,
      std::unique_ptr<std::thread> thriftThread);

 private:
  std::shared_ptr<SwitchState> programmedState_{nullptr};
  std::shared_ptr<SwitchState> initCfgState_;
  std::unique_ptr<rib::RoutingInformationBase> routingInformationBase_;
  std::unique_ptr<HwLinkStateToggler> linkToggler_;
  std::unique_ptr<Platform> platform_;
  std::unique_ptr<HwSwitch> hwSwitch_;
  uint32_t featuresDesired_{0};
  folly::Synchronized<std::set<HwSwitchEventObserverIf*>> hwEventObservers_;
  std::unique_ptr<std::thread> thriftThread_;
  bool allowPartialStateApplication_{false};
  bool initComplete_{false};
};

} // namespace fboss
} // namespace facebook
