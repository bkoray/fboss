/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "fboss/agent/state/PortQueue.h"
#include <folly/Conv.h>
#include <sstream>
#include "fboss/agent/state/NodeBase-defs.h"

namespace {
template <typename Param>
bool isPortQueueOptionalAttributeSame(
    const std::optional<Param>& swValue,
    bool isConfSet,
    const Param& confValue) {
  if (!swValue.has_value() && !isConfSet) {
    return true;
  }
  if (swValue.has_value() && isConfSet && swValue.value() == confValue) {
    return true;
  }
  return false;
}

bool comparePortQueueAQMs(
    const facebook::fboss::PortQueue::AQMMap& aqmMap,
    const std::vector<facebook::fboss::cfg::ActiveQueueManagement>& aqms) {
  auto sortedAqms = aqms;
  std::sort(
      sortedAqms.begin(),
      sortedAqms.end(),
      [](const auto& lhs, const auto& rhs) {
        return lhs.behavior < rhs.behavior;
      });
  return std::equal(
      aqmMap.begin(),
      aqmMap.end(),
      sortedAqms.begin(),
      sortedAqms.end(),
      [](const auto& behaviorAndAqm, const auto& aqm) {
        return behaviorAndAqm.second == aqm;
      });
}
} // unnamed namespace

namespace facebook {
namespace fboss {

state::PortQueueFields PortQueueFields::toThrift() const {
  state::PortQueueFields queue;
  queue.id = id;
  queue.weight = weight;
  if (reservedBytes) {
    queue.reserved = reservedBytes.value();
  }
  if (scalingFactor) {
    queue.scalingFactor =
        cfg::_MMUScalingFactor_VALUES_TO_NAMES.at(*scalingFactor);
  }
  queue.scheduling = cfg::_QueueScheduling_VALUES_TO_NAMES.at(scheduling);
  queue.streamType = cfg::_StreamType_VALUES_TO_NAMES.at(streamType);
  if (name) {
    queue.name = name.value();
  }
  if (sharedBytes) {
    queue.sharedBytes = sharedBytes.value();
  }
  if (!aqms.empty()) {
    std::vector<cfg::ActiveQueueManagement> aqmList;
    for (const auto& aqm : aqms) {
      aqmList.push_back(aqm.second);
    }
    queue.aqms = aqmList;
  }

  if (portQueueRate) {
    queue.portQueueRate = portQueueRate.value();
  }

  if (bandwidthBurstMinKbits) {
    queue.bandwidthBurstMinKbits = bandwidthBurstMinKbits.value();
  }

  if (bandwidthBurstMaxKbits) {
    queue.bandwidthBurstMaxKbits = bandwidthBurstMaxKbits.value();
  }

  if (trafficClass) {
    queue.trafficClass = static_cast<int16_t>(trafficClass.value());
  }

  return queue;
}

// static, public
PortQueueFields PortQueueFields::fromThrift(
    state::PortQueueFields const& queueThrift) {
  PortQueueFields queue;
  queue.id = static_cast<uint8_t>(queueThrift.id);

  auto const itrStreamType =
      cfg::_StreamType_NAMES_TO_VALUES.find(queueThrift.streamType.c_str());
  CHECK(itrStreamType != cfg::_StreamType_NAMES_TO_VALUES.end());
  queue.streamType = itrStreamType->second;

  auto const itrSched = cfg::_QueueScheduling_NAMES_TO_VALUES.find(
      queueThrift.scheduling.c_str());
  CHECK(itrSched != cfg::_QueueScheduling_NAMES_TO_VALUES.end());
  queue.scheduling = itrSched->second;

  queue.weight = queueThrift.weight;
  if (queueThrift.reserved) {
    queue.reservedBytes = queueThrift.reserved.value();
  }
  if (queueThrift.scalingFactor) {
    auto itrScalingFactor = cfg::_MMUScalingFactor_NAMES_TO_VALUES.find(
        queueThrift.scalingFactor->c_str());
    CHECK(itrScalingFactor != cfg::_MMUScalingFactor_NAMES_TO_VALUES.end());
    queue.scalingFactor = itrScalingFactor->second;
  }
  if (queueThrift.name) {
    queue.name = queueThrift.name.value();
  }
  if (queueThrift.sharedBytes) {
    queue.sharedBytes = queueThrift.sharedBytes.value();
  }
  if (queueThrift.aqms) {
    for (const auto& aqm : queueThrift.aqms.value()) {
      queue.aqms.emplace(aqm.behavior, aqm);
    }
  }

  if (queueThrift.portQueueRate) {
    queue.portQueueRate = queueThrift.portQueueRate.value();
  }

  if (queueThrift.bandwidthBurstMinKbits) {
    queue.bandwidthBurstMinKbits = queueThrift.bandwidthBurstMinKbits.value();
  }

  if (queueThrift.bandwidthBurstMaxKbits) {
    queue.bandwidthBurstMaxKbits = queueThrift.bandwidthBurstMaxKbits.value();
  }

  if (queueThrift.trafficClass) {
    queue.trafficClass.emplace(
        static_cast<TrafficClass>(queueThrift.trafficClass.value()));
  }

  return queue;
}

std::string PortQueue::toString() const {
  std::stringstream ss;
  ss << "Queue id=" << static_cast<int>(getID())
     << ", streamType=" << cfg::_StreamType_VALUES_TO_NAMES.at(getStreamType())
     << ", scheduling="
     << cfg::_QueueScheduling_VALUES_TO_NAMES.at(getScheduling())
     << ", weight=" << getWeight();
  if (getReservedBytes()) {
    ss << ", reservedBytes=" << getReservedBytes().value();
  }
  if (getSharedBytes()) {
    ss << ", sharedBytes=" << getSharedBytes().value();
  }
  if (getScalingFactor()) {
    ss << ", scalingFactor="
       << cfg::_MMUScalingFactor_VALUES_TO_NAMES.at(getScalingFactor().value());
  }
  if (!getAqms().empty()) {
    ss << ", aqms=[";
    for (const auto& aqm : getAqms()) {
      ss << "(behavior="
         << cfg::_QueueCongestionBehavior_VALUES_TO_NAMES.at(aqm.first)
         << ", detection=[min="
         << aqm.second.get_detection().get_linear().minimumLength
         << ", max=" << aqm.second.get_detection().get_linear().maximumLength
         << "]), ";
    }
    ss << "]";
  }
  if (getName()) {
    ss << ", name=" << getName().value();
  }

  if (getPortQueueRate()) {
    uint32_t rateMin, rateMax;
    std::string type;
    auto portQueueRate = getPortQueueRate().value();

    switch (portQueueRate.getType()) {
      case cfg::PortQueueRate::Type::pktsPerSec:
        type = "pps";
        rateMin = portQueueRate.get_pktsPerSec().minimum;
        rateMax = portQueueRate.get_pktsPerSec().maximum;
        break;
      case cfg::PortQueueRate::Type::kbitsPerSec:
        type = "pps";
        rateMin = portQueueRate.get_kbitsPerSec().minimum;
        rateMax = portQueueRate.get_kbitsPerSec().maximum;
        break;
      case cfg::PortQueueRate::Type::__EMPTY__:
        // needed to handle error from -Werror=switch, fall through
        FOLLY_FALLTHROUGH;
      default:
        type = "unknown";
        rateMin = 0;
        rateMax = 0;
        break;
    }

    ss << ", bandwidth " << type << " min: " << rateMin << " max: " << rateMax;
  }

  if (getBandwidthBurstMinKbits()) {
    ss << ", bandwidthBurstMinKbits: " << getBandwidthBurstMinKbits().value();
  }

  if (getBandwidthBurstMaxKbits()) {
    ss << ", bandwidthBurstMaxKbits: " << getBandwidthBurstMaxKbits().value();
  }

  return ss.str();
}

bool checkSwConfPortQueueMatch(
    const std::shared_ptr<PortQueue>& swQueue,
    const cfg::PortQueue* cfgQueue) {
  return swQueue->getID() == cfgQueue->id &&
      swQueue->getStreamType() == cfgQueue->streamType &&
      swQueue->getScheduling() == cfgQueue->scheduling &&
      (cfgQueue->scheduling == cfg::QueueScheduling::STRICT_PRIORITY ||
       swQueue->getWeight() == cfgQueue->weight_ref().value_unchecked()) &&
      isPortQueueOptionalAttributeSame(
             swQueue->getReservedBytes(),
             cfgQueue->__isset.reservedBytes,
             cfgQueue->reservedBytes_ref().value_unchecked()) &&
      isPortQueueOptionalAttributeSame(
             swQueue->getScalingFactor(),
             cfgQueue->__isset.scalingFactor,
             cfgQueue->scalingFactor_ref().value_unchecked()) &&
      isPortQueueOptionalAttributeSame(
             swQueue->getSharedBytes(),
             cfgQueue->__isset.sharedBytes,
             cfgQueue->sharedBytes_ref().value_unchecked()) &&
      comparePortQueueAQMs(
             swQueue->getAqms(), cfgQueue->aqms_ref().value_unchecked()) &&
      swQueue->getName() == cfgQueue->name_ref().value_unchecked() &&
      isPortQueueOptionalAttributeSame(
             swQueue->getPortQueueRate(),
             cfgQueue->__isset.portQueueRate,
             cfgQueue->portQueueRate_ref().value_unchecked()) &&
      isPortQueueOptionalAttributeSame(
             swQueue->getBandwidthBurstMinKbits(),
             cfgQueue->__isset.bandwidthBurstMinKbits,
             cfgQueue->bandwidthBurstMinKbits_ref().value_unchecked()) &&
      isPortQueueOptionalAttributeSame(
             swQueue->getBandwidthBurstMaxKbits(),
             cfgQueue->__isset.bandwidthBurstMaxKbits,
             cfgQueue->bandwidthBurstMaxKbits_ref().value_unchecked());
}

template class NodeBaseT<PortQueue, PortQueueFields>;

} // namespace fboss
} // namespace facebook
