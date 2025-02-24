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

extern "C" {
#include <opennsl/port.h>
#include <opennsl/stat.h>
#include <opennsl/types.h>
}

#include <fb303/ThreadCachedServiceData.h>
#include "common/stats/MonotonicCounter.h"

#include "fboss/agent/gen-cpp2/switch_config_types.h"
#include "fboss/agent/hw/bcm/BcmCosQueueManager.h"
#include "fboss/agent/hw/bcm/BcmPlatformPort.h"
#include "fboss/agent/hw/gen-cpp2/hardware_stats_types.h"
#include "fboss/agent/state/Port.h"
#include "fboss/agent/types.h"

#include <folly/Range.h>
#include <folly/Synchronized.h>
#include <mutex>
#include <utility>

namespace facebook {
namespace fboss {

class BcmSwitch;
class BcmPortGroup;
class SwitchState;
enum class MirrorDirection;
enum class MirrorAction;

/**
 * BcmPort is the class to abstract the physical port in BcmSwitch.
 */
class BcmPort {
 public:
  /*
   * Construct the BcmPort object.
   *
   * This method shouldn't make any calls to the Broadcom SDK to query/modify
   * the port yet.  init() will be called soon after construction, and any
   * actual initialization logic should be performed there.
   */
  BcmPort(BcmSwitch* hw, opennsl_port_t port, BcmPlatformPort* platformPort);
  ~BcmPort();

  void init(bool warmBoot);

  void enable(const std::shared_ptr<Port>& swPort);
  void enableLinkscan();
  void disable(const std::shared_ptr<Port>& swPort);
  void disableLinkscan();
  void program(const std::shared_ptr<Port>& swPort);
  void enableStatCollection(const std::shared_ptr<Port>& swPort);
  void disableStatCollection();
  void setupQueue(const PortQueue& queue);

  void attachIngressQosPolicy(const std::string& name);
  void detachIngressQosPolicy();

  /*
   * Getters.
   */
  BcmPlatformPort* getPlatformPort() const {
    return platformPort_;
  }
  BcmSwitch* getHW() const {
    return hw_;
  }
  opennsl_port_t getBcmPortId() const {
    return port_;
  }
  opennsl_gport_t getBcmGport() const {
    return gport_;
  }
  BcmPortGroup* getPortGroup() const {
    return portGroup_;
  }
  uint8_t getPipe() const {
    return pipe_;
  }
  cfg::SampleDestination getSampleDestination() const {
    return sampleDest_;
  }
  QueueConfig getCurrentQueueSettings();

  /*
   * Helpers for retreiving the SwitchState node for a given
   * port. There is no great place for this so I am putting it in here
   * for now.
   */
  std::shared_ptr<Port> getSwitchStatePort(
      const std::shared_ptr<SwitchState>& state) const;
  std::shared_ptr<Port> getSwitchStatePortIf(
      const std::shared_ptr<SwitchState>& state) const;

  PortID getPortID() const;
  std::string getPortName();
  LaneSpeeds supportedLaneSpeeds() const;

  bool supportsSpeed(cfg::PortSpeed speed);

  /*
   * Methods for retreiving the administrative and operational state of the
   * port according to the SDK.
   * Both port state methods (isEnabled and isUp) can throw if there is an
   * error talking to the SDK.
   */
  // Return whether we have enabled the port
  bool isEnabled();
  // Return whether the link status of the port is actually up.
  // Note: if it is not enabled, return false
  bool isUp();

  cfg::PortSpeed getSpeed() const;
  cfg::PortSpeed getMaxSpeed() const;

  /*
   * Setters.
   */
  void registerInPortGroup(BcmPortGroup* portGroup);
  void setIngressVlan(const std::shared_ptr<Port>& swPort);
  void setSpeed(const std::shared_ptr<Port>& swPort);
  void setSflowRates(const std::shared_ptr<Port>& swPort);
  void disableSflow();
  int sampleDestinationToBcmDestFlag(cfg::SampleDestination dest);
  void configureSampleDestination(cfg::SampleDestination sampleDest);
  void setPortResource(const std::shared_ptr<Port>& swPort);
  void setupStatsIfNeeded(const std::shared_ptr<Port>& swPort);

  /*
   * Update this port's statistics.
   */
  void updateStats();
  std::optional<HwPortStats> getPortStats() const;
  std::chrono::seconds getTimeRetrieved() const;

  /**
   * Take actions on this port (especially if it is up), so that it will not
   * flap on warm boot.
   */
  void prepareForGracefulExit();

  /**
   * return true if the port has Forward Error Correction (FEC)
   * enabled or if CL91 FEC is enabled
   */
  bool isFECEnabled();

  /*
   * Take the appropriate actions for reacting to the port's state changing.
   * e.g. optionally remediate, turn on/off LEDs
   */
  void linkStatusChanged(const std::shared_ptr<Port>& port);

  static opennsl_gport_t asGPort(opennsl_port_t port);
  static bool isValidLocalPort(opennsl_gport_t gport);
  BcmCosQueueManager* getQueueManager() const {
    return queueManager_.get();
  }

  std::optional<std::string> getIngressPortMirror() const {
    return ingressMirror_;
  }
  std::optional<std::string> getEgressPortMirror() const {
    return egressMirror_;
  }

  void setIngressPortMirror(const std::string& mirrorName);
  void setEgressPortMirror(const std::string& mirrorName);

 private:
  class BcmPortStats {
    // All actions or instantiations of this class need to be done in a
    // thread-safe way (for example, the way that locking is done on
    // lastPortStats_) - the class itself does not guarantee this on it's own
   public:
    BcmPortStats() {
      portStats_.inDiscards_ = 0;
    }
    explicit BcmPortStats(int numUnicastQueues);
    BcmPortStats(HwPortStats portStats, std::chrono::seconds seconds);
    HwPortStats portStats() const;
    std::chrono::seconds timeRetrieved() const;

   private:
    HwPortStats portStats_;
    std::chrono::seconds timeRetrieved_{0};
  };

  uint32_t getCL91FECStatus() const;
  bool isCL91FECApplicable() const;
  // no copy or assignment
  BcmPort(BcmPort const&) = delete;
  BcmPort& operator=(BcmPort const&) = delete;

  stats::MonotonicCounter* getPortCounterIf(folly::StringPiece statName);
  bool shouldReportStats() const;
  void reinitPortStats(const std::shared_ptr<Port>& swPort);
  void reinitPortStat(folly::StringPiece newName, folly::StringPiece portName);
  void destroyAllPortStats();
  void updateStat(
      std::chrono::seconds now,
      folly::StringPiece statName,
      opennsl_stat_val_t type,
      int64_t* portStatVal);
  void updateBcmStats(std::chrono::seconds now, HwPortStats* curPortStats);
  void updatePktLenHist(
      std::chrono::seconds now,
      fb303::ExportedHistogramMapImpl::LockableHistogram* hist,
      const std::vector<opennsl_stat_val_t>& stats);
  void initCustomStats() const;
  // Set stats that are either FB specific, not available in
  // open source opennsl release.
  void setAdditionalStats(std::chrono::seconds now, HwPortStats* curPortStats);
  std::string statName(folly::StringPiece statName, folly::StringPiece portName)
      const;

  cfg::PortSpeed getDesiredPortSpeed(const std::shared_ptr<Port>& swPort);
  opennsl_port_if_t getDesiredInterfaceMode(
      cfg::PortSpeed speed,
      PortID id,
      const std::string& name);
  bool getDesiredFECEnabledStatus(const std::shared_ptr<Port>& swPort);
  TransmitterTechnology getTransmitterTechnology(const std::string& name);
  void updateMirror(
      const std::optional<std::string>& swMirrorName,
      MirrorDirection direction,
      cfg::SampleDestination newDestination);

  opennsl_pbmp_t getPbmp();

  void setInterfaceMode(const std::shared_ptr<Port>& swPort);
  void setFEC(const std::shared_ptr<Port>& swPort);
  void setPause(const std::shared_ptr<Port>& swPort);
  void setTxSetting(const std::shared_ptr<Port>& swPort);
  void setLoopbackMode(const std::shared_ptr<Port>& swPort);

  bool isMmuLossy() const;
  uint8_t determinePipe() const;

  void applyMirrorAction(
      MirrorAction action,
      MirrorDirection direction,
      cfg::SampleDestination newDestination);

  BcmSwitch* const hw_{nullptr};
  const opennsl_port_t port_; // Broadcom physical port number
  // The gport_ is logically a const, but needs to be initialized as a parameter
  // to SDK call.
  opennsl_gport_t gport_; // Broadcom global port number
  uint8_t pipe_;
  BcmPlatformPort* const platformPort_{nullptr};
  int unit_{-1};
  std::optional<std::string> ingressMirror_;
  std::optional<std::string> egressMirror_;
  cfg::SampleDestination sampleDest_;
  TransmitterTechnology transmitterTechnology_{TransmitterTechnology::UNKNOWN};

  // The port group this port is a part of
  BcmPortGroup* portGroup_{nullptr};

  std::map<std::string, stats::MonotonicCounter> portCounters_;
  std::unique_ptr<BcmCosQueueManager> queueManager_;

  fb303::ExportedStatMapImpl::LockableStat outQueueLen_;
  fb303::ExportedHistogramMapImpl::LockableHistogram inPktLengths_;
  fb303::ExportedHistogramMapImpl::LockableHistogram outPktLengths_;

  folly::Synchronized<std::optional<BcmPortStats>> lastPortStats_;
  folly::Synchronized<std::shared_ptr<Port>> programmedSettings_;

  std::atomic<bool> statCollectionEnabled_{false};
};

} // namespace fboss
} // namespace facebook
