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
#include <opennsl/l3.h>
#include <opennsl/types.h>
}

#include <folly/IPAddress.h>
#include <folly/MacAddress.h>
#include <folly/dynamic.h>
#include "fboss/agent/FbossError.h"
#include "fboss/agent/hw/bcm/BcmWarmBootCache.h"
#include "fboss/agent/state/RouteTypes.h"
#include "fboss/agent/types.h"

#include <boost/container/flat_set.hpp>
#include <boost/noncopyable.hpp>

namespace facebook {
namespace fboss {

class BcmSwitchIf;

bool operator==(const opennsl_l3_egress_t& lhs, const opennsl_l3_egress_t& rhs);

class BcmEgressBase : public boost::noncopyable {
 public:
  enum : opennsl_if_t {
    INVALID = -1,
  };
  opennsl_if_t getID() const {
    return id_;
  }
  virtual ~BcmEgressBase() {}
  virtual bool isEcmp() const = 0;
  virtual bool hasLabel() const = 0;
  virtual opennsl_mpls_label_t getLabel() const = 0;
  virtual folly::MacAddress getMac() const = 0;

 protected:
  explicit BcmEgressBase(const BcmSwitchIf* hw) : hw_(hw) {}
  // this is used for unittesting
  BcmEgressBase(const BcmSwitchIf* hw, opennsl_if_t testId)
      : hw_(hw), id_(testId) {}
  const BcmSwitchIf* hw_{nullptr};
  opennsl_if_t id_{INVALID};
};

class BcmEgress : public BcmEgressBase {
 public:
  explicit BcmEgress(const BcmSwitchIf* hw) : BcmEgressBase(hw) {}
  // the following c-tor is used for unit-testing
  BcmEgress(const BcmSwitchIf* hw, opennsl_if_t testId)
      : BcmEgressBase(hw, testId) {}
  ~BcmEgress() override;
  void programToPort(
      opennsl_if_t intfId,
      opennsl_vrf_t vrf,
      const folly::IPAddress& ip,
      folly::MacAddress mac,
      opennsl_port_t port) {
    return program(intfId, vrf, ip, &mac, port, NEXTHOPS);
  }
  void programToCPU(
      opennsl_if_t intfId,
      opennsl_vrf_t vrf,
      const folly::IPAddress& ip) {
    return program(intfId, vrf, ip, nullptr, 0, TO_CPU);
  }
  void programToDrop(
      opennsl_if_t intfId,
      opennsl_vrf_t vrf,
      const folly::IPAddress& ip) {
    return program(intfId, vrf, ip, nullptr, 0, DROP);
  }
  void programToTrunk(
      opennsl_if_t intfId,
      opennsl_vrf_t /* vrf */,
      const folly::IPAddress& /* ip */,
      folly::MacAddress mac,
      opennsl_trunk_t trunk);

  bool isEcmp() const override {
    return false;
  }
  /**
   * Create a TO CPU egress object without any specific interface or address
   *
   * This API is used when a generic TO CPU egress object is needed.
   */
  void programToCPU();

  /*
   * By default, BCM SDK create a drop egress object. It is always the
   * first egress object ID created. If we create a new one, the warm
   * reboot cache code will have trouble to find out which one is supposed
   * to use. Therefore, just use the default one.
   * verifyDropEgressId() is to verify this assumption is correct or not.
   */
  static opennsl_if_t getDropEgressId() {
    return 100000;
  }
  /**
   * Verify if egress ID is programmed as drop or not.
   *
   */
  static void verifyDropEgress(int unit);

  // Returns if the egress object is programmed to drop
  static bool programmedToDrop(const opennsl_l3_egress_t& egr) {
    return egr.flags & OPENNSL_L3_DST_DISCARD;
  }

  bool hasLabel() const override {
    return false;
  }

  opennsl_mpls_label_t getLabel() const override {
    throw FbossError("labeled requested on unlabeled egress");
  }

  folly::MacAddress getMac() const override {
    return mac_;
  }

  opennsl_if_t getIntfId() const {
    return intfId_;
  }

 protected:
  virtual void prepareEgressObject(
      opennsl_if_t intfId,
      opennsl_port_t port,
      const std::optional<folly::MacAddress>& mac,
      RouteForwardAction action,
      opennsl_l3_egress_t* egress) const;

 private:
  virtual BcmWarmBootCache::EgressId2EgressCitr findEgress(
      opennsl_vrf_t vrf,
      opennsl_if_t intfId,
      const folly::IPAddress& ip) const;

  bool alreadyExists(const opennsl_l3_egress_t& newEgress) const;
  void program(
      opennsl_if_t intfId,
      opennsl_vrf_t vrf,
      const folly::IPAddress& ip,
      const folly::MacAddress* mac,
      opennsl_port_t port,
      RouteForwardAction action);

  folly::MacAddress mac_;
  opennsl_if_t intfId_{INVALID};
};

class BcmEcmpEgress : public BcmEgressBase {
 public:
  using EgressId = opennsl_if_t;
  using EgressIdSet = boost::container::flat_set<EgressId>;
  using Paths = boost::container::flat_multiset<EgressId>;
  enum class Action { SHRINK, EXPAND, SKIP };

  BcmEcmpEgress(const BcmSwitchIf* hw, const Paths& paths);
  ~BcmEcmpEgress() override;
  bool pathUnreachableHwLocked(EgressId path);
  bool pathReachableHwLocked(EgressId path);
  const Paths& paths() const {
    return paths_;
  }
  bool isEcmp() const override {
    return true;
  }
  bool hasLabel() const override {
    return false;
  }
  opennsl_mpls_label_t getLabel() const override {
    throw FbossError("labeled requested on multipath egress");
  }
  folly::MacAddress getMac() const override {
    throw FbossError("mac requested on multipath egress");
  }
  /*
   * Update ecmp egress entries in HW
   */
  static bool addEgressIdHwLocked(
      int unit,
      EgressId ecmpId,
      const Paths& egressIdInSw,
      EgressId toAdd);
  static bool
  removeEgressIdHwNotLocked(int unit, EgressId ecmpId, EgressId toRemove);
  static bool
  removeEgressIdHwLocked(int unit, EgressId ecmpId, EgressId toRemove);

 private:
  void program();
  const Paths paths_;
};

bool operator==(const opennsl_l3_egress_t& lhs, const opennsl_l3_egress_t& rhs);
opennsl_mpls_label_t getLabel(const opennsl_l3_egress_t& egress);

} // namespace fboss
} // namespace facebook
