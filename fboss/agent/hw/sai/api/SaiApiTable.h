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

#include "fboss/agent/hw/sai/api/BridgeApi.h"
#include "fboss/agent/hw/sai/api/FdbApi.h"
#include "fboss/agent/hw/sai/api/HashApi.h"
#include "fboss/agent/hw/sai/api/HostifApi.h"
#include "fboss/agent/hw/sai/api/NeighborApi.h"
#include "fboss/agent/hw/sai/api/NextHopApi.h"
#include "fboss/agent/hw/sai/api/NextHopGroupApi.h"
#include "fboss/agent/hw/sai/api/PortApi.h"
#include "fboss/agent/hw/sai/api/QueueApi.h"
#include "fboss/agent/hw/sai/api/RouteApi.h"
#include "fboss/agent/hw/sai/api/RouterInterfaceApi.h"
#include "fboss/agent/hw/sai/api/SchedulerApi.h"
#include "fboss/agent/hw/sai/api/SwitchApi.h"
#include "fboss/agent/hw/sai/api/VirtualRouterApi.h"
#include "fboss/agent/hw/sai/api/VlanApi.h"

#include <memory>

namespace facebook::fboss {

class SaiApiTable {
 public:
  SaiApiTable() = default;
  ~SaiApiTable() = default;
  SaiApiTable(const SaiApiTable& other) = delete;
  SaiApiTable& operator=(const SaiApiTable& other) = delete;

  // Static function for getting the SaiApiTable folly::Singleton
  static std::shared_ptr<SaiApiTable> getInstance();

  /*
   * This constructs each individual SAI API object which queries the Adapter
   * for the api implementation and thus renders them ready for use by the
   * Adapter Host.
   */
  void queryApis();

  BridgeApi& bridgeApi();
  const BridgeApi& bridgeApi() const;

  FdbApi& fdbApi();
  const FdbApi& fdbApi() const;

  HostifApi& hostifApi();
  const HostifApi& hostifApi() const;

  NextHopApi& nextHopApi();
  const NextHopApi& nextHopApi() const;

  NextHopGroupApi& nextHopGroupApi();
  const NextHopGroupApi& nextHopGroupApi() const;

  NeighborApi& neighborApi();
  const NeighborApi& neighborApi() const;

  PortApi& portApi();
  const PortApi& portApi() const;

  QueueApi& queueApi();
  const QueueApi& queueApi() const;

  RouteApi& routeApi();
  const RouteApi& routeApi() const;

  RouterInterfaceApi& routerInterfaceApi();
  const RouterInterfaceApi& routerInterfaceApi() const;

  SchedulerApi& schedulerApi();
  const SchedulerApi& schedulerApi() const;

  SwitchApi& switchApi();
  const SwitchApi& switchApi() const;

  VirtualRouterApi& virtualRouterApi();
  const VirtualRouterApi& virtualRouterApi() const;

  VlanApi& vlanApi();
  const VlanApi& vlanApi() const;

  template <typename SaiApiT>
  SaiApiT& getApi() {
    return *std::get<std::unique_ptr<SaiApiT>>(apis_);
  }

  template <typename SaiApiT>
  const SaiApiT& getApi() const {
    return *std::get<std::unique_ptr<SaiApiT>>(apis_);
  }

 private:
  std::tuple<
      std::unique_ptr<BridgeApi>,
      std::unique_ptr<FdbApi>,
      std::unique_ptr<HostifApi>,
      std::unique_ptr<NextHopApi>,
      std::unique_ptr<NextHopGroupApi>,
      std::unique_ptr<NeighborApi>,
      std::unique_ptr<PortApi>,
      std::unique_ptr<QueueApi>,
      std::unique_ptr<RouteApi>,
      std::unique_ptr<RouterInterfaceApi>,
      std::unique_ptr<SchedulerApi>,
      std::unique_ptr<SwitchApi>,
      std::unique_ptr<VirtualRouterApi>,
      std::unique_ptr<VlanApi>>
      apis_;
  bool apisQueried_{false};
};

} // namespace facebook::fboss
