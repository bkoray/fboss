/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "fboss/agent/hw/test/LoadBalancerUtils.h"

#include <folly/IPAddress.h>

#include "fboss/agent/HwSwitch.h"
#include "fboss/agent/LoadBalancerConfigApplier.h"
#include "fboss/agent/hw/test/HwSwitchEnsemble.h"
#include "fboss/agent/hw/test/HwTestPacketUtils.h"
#include "fboss/agent/packet/PktFactory.h"
#include "fboss/agent/state/LoadBalancer.h"
#include "fboss/agent/state/SwitchState.h"

#include <folly/gen/Base.h>

namespace facebook {
namespace fboss {
namespace utility {

cfg::Fields getHalfHashFields() {
  cfg::Fields hashFields;
  hashFields.set_ipv4Fields(std::set<cfg::IPv4Field>(
      {cfg::IPv4Field::SOURCE_ADDRESS, cfg::IPv4Field::DESTINATION_ADDRESS}));
  hashFields.set_ipv6Fields(std::set<cfg::IPv6Field>(
      {cfg::IPv6Field::SOURCE_ADDRESS, cfg::IPv6Field::DESTINATION_ADDRESS}));

  return hashFields;
}

cfg::Fields getFullHashFields() {
  auto hashFields = getHalfHashFields();
  hashFields.set_transportFields(
      std::set<cfg::TransportField>({cfg::TransportField::SOURCE_PORT,
                                     cfg::TransportField::DESTINATION_PORT}));
  return hashFields;
}

cfg::LoadBalancer getHalfHashConfig(cfg::LoadBalancerID id) {
  cfg::LoadBalancer loadBalancer;
  loadBalancer.id = id;
  loadBalancer.fieldSelection = getHalfHashFields();
  loadBalancer.algorithm = cfg::HashingAlgorithm::CRC16_CCITT;
  return loadBalancer;
}
cfg::LoadBalancer getFullHashConfig(cfg::LoadBalancerID id) {
  cfg::LoadBalancer loadBalancer;
  loadBalancer.id = id;
  loadBalancer.fieldSelection = getFullHashFields();
  loadBalancer.algorithm = cfg::HashingAlgorithm::CRC16_CCITT;
  return loadBalancer;
}

cfg::LoadBalancer getEcmpHalfHashConfig() {
  return getHalfHashConfig(cfg::LoadBalancerID::ECMP);
}
cfg::LoadBalancer getEcmpFullHashConfig() {
  return getFullHashConfig(cfg::LoadBalancerID::ECMP);
}
cfg::LoadBalancer getTrunkHalfHashConfig() {
  return getHalfHashConfig(cfg::LoadBalancerID::AGGREGATE_PORT);
}
cfg::LoadBalancer getTrunkFullHashConfig() {
  return getFullHashConfig(cfg::LoadBalancerID::AGGREGATE_PORT);
}

std::vector<cfg::LoadBalancer> getEcmpFullTrunkHalfHashConfig() {
  return {getEcmpFullHashConfig(), getTrunkHalfHashConfig()};
}
std::vector<cfg::LoadBalancer> getEcmpHalfTrunkFullHashConfig() {
  return {getEcmpHalfHashConfig(), getTrunkFullHashConfig()};
}

std::shared_ptr<SwitchState> addLoadBalancer(
    const Platform* platform,
    const std::shared_ptr<SwitchState>& inputState,
    const cfg::LoadBalancer& loadBalancerCfg) {
  return addLoadBalancers(platform, inputState, {loadBalancerCfg});
}

std::shared_ptr<SwitchState> addLoadBalancers(
    const Platform* platform,
    const std::shared_ptr<SwitchState>& inputState,
    const std::vector<cfg::LoadBalancer>& loadBalancerCfgs) {
  auto newState{inputState->clone()};
  auto lbMap = newState->getLoadBalancers()->clone();
  for (const auto& loadBalancerCfg : loadBalancerCfgs) {
    lbMap->addLoadBalancer(
        LoadBalancerConfigParser(platform).parse(loadBalancerCfg));
  }
  newState->resetLoadBalancers(lbMap);
  return newState;
}

void pumpTraffic(
    bool isV6,
    HwSwitch* hw,
    folly::MacAddress intfMac,
    VlanID vlan,
    std::optional<PortID> frontPanelPortToLoopTraffic) {
  for (auto i = 0; i < 100; ++i) {
    auto srcIp = folly::IPAddress(
        folly::sformat(isV6 ? "1001::{}" : "100.0.0.{}", i + 1));
    for (auto j = 0; j < 100; ++j) {
      auto dstIp = folly::IPAddress(
          folly::sformat(isV6 ? "2001::{}" : "200.0.0.{}", j + 1));
      auto pkt = makeUDPTxPacket(
          hw, vlan, intfMac, intfMac, srcIp, dstIp, 10000 + i, 20000 + j);
      if (frontPanelPortToLoopTraffic) {
        hw->sendPacketOutOfPortSync(
            std::move(pkt), frontPanelPortToLoopTraffic.value());
      } else {
        hw->sendPacketSwitchedSync(std::move(pkt));
      }
    }
  }
}

void pumpMplsTraffic(
    bool isV6,
    HwSwitch* hw,
    uint32_t label,
    folly::MacAddress intfMac,
    std::optional<PortID> frontPanelPortToLoopTraffic) {
  MPLSHdr::Label mplsLabel{label, 0, true, 128};
  std::unique_ptr<TxPacket> pkt;
  for (auto i = 0; i < 100; ++i) {
    auto srcIp = folly::IPAddress(
        folly::sformat(isV6 ? "1001::{}" : "100.0.0.{}", i + 1));
    for (auto j = 0; j < 100; ++j) {
      auto dstIp = folly::IPAddress(
          folly::sformat(isV6 ? "2001::{}" : "200.0.0.{}", j + 1));

      auto frame = isV6 ? utility::getEthFrame(
                              intfMac,
                              intfMac,
                              {mplsLabel},
                              srcIp.asV6(),
                              dstIp.asV6(),
                              10000 + i,
                              20000 + j)
                        : utility::getEthFrame(
                              intfMac,
                              intfMac,
                              {mplsLabel},
                              srcIp.asV4(),
                              dstIp.asV4(),
                              10000 + i,
                              20000 + j);

      if (isV6) {
        pkt = frame.getTxPacket(hw);
      } else {
        pkt = frame.getTxPacket(hw);
      }

      if (frontPanelPortToLoopTraffic) {
        hw->sendPacketOutOfPortSync(
            std::move(pkt), frontPanelPortToLoopTraffic.value());
      } else {
        hw->sendPacketSwitchedSync(std::move(pkt));
      }
    }
  }
}

bool isLoadBalanced(
    HwSwitchEnsemble* hwSwitchEnsemble,
    const std::vector<PortDescriptor>& ecmpPorts,
    const std::vector<NextHopWeight>& weights,
    int maxDeviationPct,
    bool noTrafficOk) {
  auto portIDs = folly::gen::from(ecmpPorts) |
      folly::gen::map([](const auto& portDesc) {
                   CHECK(portDesc.isPhysicalPort());
                   return portDesc.phyPortID();
                 }) |
      folly::gen::as<std::vector<PortID>>();
  auto portIdToStats = hwSwitchEnsemble->getLatestPortStats(portIDs);
  auto portBytes = folly::gen::from(portIdToStats) |
      folly::gen::map([](const auto& portIdAndStats) {
                     return portIdAndStats.second.outBytes_;
                   }) |
      folly::gen::as<std::set<uint64_t>>();

  auto lowest = *portBytes.begin();
  auto highest = *portBytes.rbegin();
  XLOG(DBG0) << " Highest bytes: " << highest << " lowest bytes: " << lowest;
  if (!lowest) {
    return !highest && noTrafficOk;
  }
  if (!weights.empty()) {
    auto maxWeight = *(std::max_element(weights.begin(), weights.end()));
    for (auto i = 0; i < ecmpPorts.size(); ++i) {
      auto portOutBytes = portIdToStats[ecmpPorts[i].phyPortID()].outBytes_;
      auto weightPercent = (static_cast<float>(weights[i]) / maxWeight) * 100.0;
      auto portOutBytesPercent =
          (static_cast<float>(portOutBytes) / highest) * 100.0;
      auto percentDev = std::abs(weightPercent - portOutBytesPercent);
      // Don't tolerate a deviation of more than maxDeviationPct
      XLOG(INFO) << "Percent Deviation: " << percentDev
                 << ", Maximum Deviation: " << maxDeviationPct;
      if (percentDev > maxDeviationPct) {
        return false;
      }
    }
  } else {
    auto percentDev = (static_cast<float>(highest - lowest) / lowest) * 100.0;
    // Don't tolerate a deviation of more than maxDeviationPct
    XLOG(INFO) << "Percent Deviation: " << percentDev
               << ", Maximum Deviation: " << maxDeviationPct;
    if (percentDev > maxDeviationPct) {
      return false;
    }
  }
  return true;
}

bool isLoadBalanced(
    HwSwitchEnsemble* hwSwitchEnsemble,
    const std::vector<PortDescriptor>& ecmpPorts,
    int maxDeviationPct) {
  return isLoadBalanced(
      hwSwitchEnsemble,
      ecmpPorts,
      std::vector<NextHopWeight>(),
      maxDeviationPct);
}

} // namespace utility
} // namespace fboss
} // namespace facebook
