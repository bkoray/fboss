/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "fboss/agent/ApplyThriftConfig.h"

#include <folly/FileUtil.h>
#include <folly/gen/Base.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include "fboss/agent/FbossError.h"
#include "fboss/agent/LacpTypes.h"
#include "fboss/agent/LoadBalancerConfigApplier.h"
#include "fboss/agent/Platform.h"
#include "fboss/agent/rib/RoutingInformationBase.h"
#include "fboss/agent/state/AclEntry.h"
#include "fboss/agent/state/AclMap.h"
#include "fboss/agent/state/AggregatePort.h"
#include "fboss/agent/state/AggregatePortMap.h"
#include "fboss/agent/state/ArpResponseTable.h"
#include "fboss/agent/state/ControlPlane.h"
#include "fboss/agent/state/Interface.h"
#include "fboss/agent/state/InterfaceMap.h"
#include "fboss/agent/state/Mirror.h"
#include "fboss/agent/state/NdpResponseTable.h"
#include "fboss/agent/state/Port.h"
#include "fboss/agent/state/PortMap.h"
#include "fboss/agent/state/QosPolicyMap.h"
#include "fboss/agent/state/Route.h"
#include "fboss/agent/state/RouteTable.h"
#include "fboss/agent/state/RouteTableMap.h"
#include "fboss/agent/state/RouteTableRib.h"
#include "fboss/agent/state/RouteTypes.h"
#include "fboss/agent/state/RouteUpdater.h"
#include "fboss/agent/state/SflowCollector.h"
#include "fboss/agent/state/SflowCollectorMap.h"
#include "fboss/agent/state/SwitchState.h"
#include "fboss/agent/state/Vlan.h"
#include "fboss/agent/state/VlanMap.h"

#include <boost/container/flat_map.hpp>
#include <boost/container/flat_set.hpp>
#include <folly/Range.h>
#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "fboss/agent/rib/ForwardingInformationBaseUpdater.h"
#include "fboss/agent/rib/NetworkToRouteMap.h"

using boost::container::flat_map;
using boost::container::flat_set;
using folly::CIDRNetwork;
using folly::IPAddress;
using folly::IPAddressFormatException;
using folly::IPAddressV4;
using folly::IPAddressV6;
using folly::MacAddress;
using folly::StringPiece;
using std::make_shared;
using std::shared_ptr;

namespace {

const uint8_t kV6LinkLocalAddrMask{64};
// Needed until CoPP is removed from code and put into config
const int kAclStartPriority = 100000;

void updateFibFromConfig(
    facebook::fboss::RouterID vrf,
    const facebook::fboss::rib::IPv4NetworkToRouteMap& v4NetworkToRoute,
    const facebook::fboss::rib::IPv6NetworkToRouteMap& v6NetworkToRoute,
    void* cookie) {
  facebook::fboss::rib::ForwardingInformationBaseUpdater fibUpdater(
      vrf, v4NetworkToRoute, v6NetworkToRoute);

  auto nextStatePtr =
      static_cast<std::shared_ptr<facebook::fboss::SwitchState>*>(cookie);

  fibUpdater(*nextStatePtr);
}

} // anonymous namespace

namespace facebook {
namespace fboss {

/*
 * A class for implementing applyThriftConfig().
 *
 * This implements a procedural function.  It is defined as a class purely as a
 * convenience for the implementation, to allow easily sharing state between
 * internal helper methods.
 */
class ThriftConfigApplier {
 public:
  ThriftConfigApplier(
      const std::shared_ptr<SwitchState>& orig,
      const cfg::SwitchConfig* config,
      const Platform* platform,
      rib::RoutingInformationBase* rib)
      : orig_(orig), cfg_(config), platform_(platform), rib_(rib) {}

  std::shared_ptr<SwitchState> run();

 private:
  // Forbidden copy constructor and assignment operator
  ThriftConfigApplier(ThriftConfigApplier const&) = delete;
  ThriftConfigApplier& operator=(ThriftConfigApplier const&) = delete;

  template <typename Node, typename NodeMap>
  bool updateMap(
      NodeMap* map,
      std::shared_ptr<Node> origNode,
      std::shared_ptr<Node> newNode) {
    if (newNode) {
      auto ret = map->emplace(std::make_pair(newNode->getID(), newNode));
      if (!ret.second) {
        throw FbossError("duplicate entry ", newNode->getID());
      }
      return true;
    } else {
      auto ret = map->emplace(std::make_pair(origNode->getID(), origNode));
      if (!ret.second) {
        throw FbossError("duplicate entry ", origNode->getID());
      }
      return false;
    }
  }

  // Interface route prefix. IPAddress has mask applied
  typedef std::pair<InterfaceID, folly::IPAddress> IntfAddress;
  typedef boost::container::flat_map<folly::CIDRNetwork, IntfAddress> IntfRoute;
  typedef boost::container::flat_map<RouterID, IntfRoute> IntfRouteTable;
  IntfRouteTable intfRouteTables_;

  /* The ThriftConfigApplier object exposes a single, top-level method "run()".
   * In this method, a previous SwitchState "orig_" is first cloned and the
   * clone modified until it matches the specifications of the SwitchConfig
   * "cfg_". The private methods of ThriftConfigApplier implement the logic
   * necessary to perform these modifications.
   *
   * These methods generally follow a common scheme to do so based on each
   * SwitchState node being uniquely identified by an ID within the set of nodes
   * of the same type. For instance, a VLAN node is uniquely identified by
   * its "const VlanID id" member variable. No other VLAN may have the same
   * ID. But it is entirely possible for there to exist an Interface node with
   * the same numerical ID (ignoring type incompatibility between VlanID and
   * InterfaceID).
   *
   * There are 3 cases to consider:
   *
   * 1) cfg_ and orig_ both have a node with the same ID
   *    If the specifications in cfg_ differ from those of orig_, then the
   *    clone of the node is updated appropriately. This functionality is
   *    provided by methods such as updateAggPort(), updateVlan(), etc.
   * 2) cfg_ has a node with an ID that does not exist in orig_
   *    A node with this ID is added to the cloned SwitchState. This
   *    functionality is provided by methods such as createAggPort(),
   *    createVlan(), etc.
   * 3) orig_ has a node with an ID that does not exist in cfg_
   *    This node is implicity deleted in the clone.
   *
   * Methods such as updateAggregatePorts(), updateVlans(), etc. encapsulate
   * this logic for each type of NodeBase.
   */

  void processVlanPorts();
  void updateVlanInterfaces(const Interface* intf);
  std::shared_ptr<PortMap> updatePorts();
  std::shared_ptr<Port> updatePort(
      const std::shared_ptr<Port>& orig,
      const cfg::Port* cfg);
  QueueConfig updatePortQueues(
      const std::vector<std::shared_ptr<PortQueue>>& origPortQueues,
      const std::vector<cfg::PortQueue>& cfgPortQueues,
      std::optional<cfg::QosMap> qosMap = std::nullopt);
  // update cfg port queue attribute to state port queue object
  void setPortQueue(
      std::shared_ptr<PortQueue> newQueue,
      const cfg::PortQueue* cfg);
  std::shared_ptr<PortQueue> updatePortQueue(
      const std::shared_ptr<PortQueue>& orig,
      const cfg::PortQueue* cfg,
      const std::optional<cfg::QosMap>& qosMap);
  std::shared_ptr<PortQueue> createPortQueue(
      const cfg::PortQueue* cfg,
      std::optional<TrafficClass> trafficClass);
  void checkPortQueueAQMValid(
      const std::vector<cfg::ActiveQueueManagement>& aqms);
  std::shared_ptr<AggregatePortMap> updateAggregatePorts();
  std::shared_ptr<AggregatePort> updateAggPort(
      const std::shared_ptr<AggregatePort>& orig,
      const cfg::AggregatePort& cfg);
  std::shared_ptr<AggregatePort> createAggPort(const cfg::AggregatePort& cfg);
  std::vector<AggregatePort::Subport> getSubportsSorted(
      const cfg::AggregatePort& cfg);
  std::pair<folly::MacAddress, uint16_t> getSystemLacpConfig();
  uint8_t computeMinimumLinkCount(const cfg::AggregatePort& cfg);
  std::shared_ptr<VlanMap> updateVlans();
  std::shared_ptr<Vlan> createVlan(const cfg::Vlan* config);
  std::shared_ptr<Vlan> updateVlan(
      const std::shared_ptr<Vlan>& orig,
      const cfg::Vlan* config);
  std::shared_ptr<AclMap> updateAcls();
  std::shared_ptr<AclEntry> createAcl(
      const cfg::AclEntry* config,
      int priority,
      const MatchAction* action = nullptr);
  std::shared_ptr<AclEntry> updateAcl(
      const cfg::AclEntry& acl,
      int priority,
      int* numExistingProcessed,
      bool* changed,
      const MatchAction* action = nullptr);
  // check the acl provided by config is valid
  void checkAcl(const cfg::AclEntry* config) const;
  std::shared_ptr<QosPolicyMap> updateQosPolicies();
  std::shared_ptr<QosPolicy> updateQosPolicy(
      cfg::QosPolicy& qosPolicy,
      int* numExistingProcessed,
      bool* changed);
  std::optional<std::string> getDefaultDataPlaneQosPolicyName() const;
  std::shared_ptr<QosPolicy> updateDataplaneDefaultQosPolicy();
  shared_ptr<QosPolicy> createQosPolicy(const cfg::QosPolicy& qosPolicy);
  bool updateNeighborResponseTables(Vlan* vlan, const cfg::Vlan* config);
  bool updateDhcpOverrides(Vlan* vlan, const cfg::Vlan* config);
  std::shared_ptr<InterfaceMap> updateInterfaces();
  std::shared_ptr<RouteTableMap> updateInterfaceRoutes();
  std::shared_ptr<RouteTableMap> syncStaticRoutes(
      const std::shared_ptr<RouteTableMap>& routes);
  shared_ptr<Interface> createInterface(
      const cfg::Interface* config,
      const Interface::Addresses& addrs);
  shared_ptr<Interface> updateInterface(
      const shared_ptr<Interface>& orig,
      const cfg::Interface* config,
      const Interface::Addresses& addrs);
  std::string getInterfaceName(const cfg::Interface* config);
  folly::MacAddress getInterfaceMac(const cfg::Interface* config);
  Interface::Addresses getInterfaceAddresses(const cfg::Interface* config);
  shared_ptr<SflowCollectorMap> updateSflowCollectors();
  shared_ptr<SflowCollector> createSflowCollector(
      const cfg::SflowCollector* config);
  shared_ptr<SflowCollector> updateSflowCollector(
      const shared_ptr<SflowCollector>& orig,
      const cfg::SflowCollector* config);
  shared_ptr<SwitchSettings> updateSwitchSettings();
  shared_ptr<ControlPlane> updateControlPlane();
  std::shared_ptr<MirrorMap> updateMirrors();
  std::shared_ptr<Mirror> createMirror(const cfg::Mirror* config);
  std::shared_ptr<Mirror> updateMirror(
      const std::shared_ptr<Mirror>& orig,
      const cfg::Mirror* config);
  std::shared_ptr<ForwardingInformationBaseMap>
  updateForwardingInformationBaseContainers();

  std::shared_ptr<SwitchState> orig_;
  std::shared_ptr<SwitchState> new_;
  const cfg::SwitchConfig* cfg_{nullptr};
  const Platform* platform_{nullptr};
  rib::RoutingInformationBase* rib_{nullptr};

  struct VlanIpInfo {
    VlanIpInfo(uint8_t mask, MacAddress mac, InterfaceID intf)
        : mask(mask), mac(mac), interfaceID(intf) {}

    uint8_t mask;
    MacAddress mac;
    InterfaceID interfaceID;
  };
  struct VlanInterfaceInfo {
    RouterID routerID{0};
    flat_set<InterfaceID> interfaces;
    flat_map<IPAddress, VlanIpInfo> addresses;
  };

  flat_map<PortID, Port::VlanMembership> portVlans_;
  flat_map<VlanID, Vlan::MemberPorts> vlanPorts_;
  flat_map<VlanID, VlanInterfaceInfo> vlanInterfaces_;
};

shared_ptr<SwitchState> ThriftConfigApplier::run() {
  new_ = orig_->clone();
  bool changed = false;

  {
    auto newSwitchSettings = updateSwitchSettings();
    if (newSwitchSettings) {
      new_->resetSwitchSettings(std::move(newSwitchSettings));
      changed = true;
    }
  }

  {
    auto newControlPlane = updateControlPlane();
    if (newControlPlane) {
      new_->resetControlPlane(std::move(newControlPlane));
      changed = true;
    }
  }

  processVlanPorts();

  {
    auto newPorts = updatePorts();
    if (newPorts) {
      new_->resetPorts(std::move(newPorts));
      changed = true;
    }
  }

  {
    auto newAggPorts = updateAggregatePorts();
    if (newAggPorts) {
      new_->resetAggregatePorts(std::move(newAggPorts));
      changed = true;
    }
  }

  // updateMirrors must be called after updatePorts, mirror needs ports!
  {
    auto newMirrors = updateMirrors();
    if (newMirrors) {
      new_->resetMirrors(std::move(newMirrors));
      changed = true;
    }
  }

  // updateAcls must be called after updateMirrors, acls may need mirror!
  {
    auto newAcls = updateAcls();
    if (newAcls) {
      new_->resetAcls(std::move(newAcls));
      changed = true;
    }
  }

  {
    auto newQosPolicies = updateQosPolicies();
    if (newQosPolicies) {
      new_->resetQosPolicies(std::move(newQosPolicies));
      changed = true;
    }
  }

  // reset the default qos policy
  {
    auto newDefaultQosPolicy = updateDataplaneDefaultQosPolicy();
    if (new_->getDefaultDataPlaneQosPolicy() != newDefaultQosPolicy) {
      new_->setDefaultDataPlaneQosPolicy(newDefaultQosPolicy);
    }
  }

  {
    auto newIntfs = updateInterfaces();
    if (newIntfs) {
      new_->resetIntfs(std::move(newIntfs));
      changed = true;
    }
  }

  // Note: updateInterfaces() must be called before updateVlans(),
  // as updateInterfaces() populates the vlanInterfaces_ data structure.
  {
    auto newVlans = updateVlans();
    if (newVlans) {
      new_->resetVlans(std::move(newVlans));
      changed = true;
    }
  }

  if (rib_) {
    auto newFibs = updateForwardingInformationBaseContainers();
    if (newFibs) {
      new_->resetForwardingInformationBases(newFibs);
      changed = true;
    }

    rib_->reconfigure(
        intfRouteTables_,
        cfg_->staticRoutesWithNhops,
        cfg_->staticRoutesToNull,
        cfg_->staticRoutesToCPU,
        &updateFibFromConfig,
        static_cast<void*>(&new_));
  } else {
    // Note: updateInterfaces() must be called before updateInterfaceRoutes(),
    // as updateInterfaces() populates the intfRouteTables_ data structure.
    // Also, updateInterfaceRoutes() should be the first call for updating
    // RouteTable as this will take the RouteTable from orig_ and add Interface
    // routes. Calling this after other RouteTable updates will result in other
    // routes getting removed during updateInterfaceRoutes()

    auto newTables = updateInterfaceRoutes();
    if (newTables) {
      new_->resetRouteTables(newTables);
      changed = true;
    }

    // Retrieve RouteTableMap from new_ as this will have
    // all the routes updated until now. Pass this to syncStaticRoutes
    // so that routes added until now would not be excluded.
    auto updatedRoutes = new_->getRouteTables();
    auto newerTables = syncStaticRoutes(updatedRoutes);
    if (newerTables) {
      new_->resetRouteTables(std::move(newerTables));
      changed = true;
    }
  }

  auto newVlans = new_->getVlans();
  VlanID dfltVlan(cfg_->defaultVlan);
  if (orig_->getDefaultVlan() != dfltVlan) {
    if (newVlans->getVlanIf(dfltVlan) == nullptr) {
      throw FbossError("Default VLAN ", dfltVlan, " does not exist");
    }
    new_->setDefaultVlan(dfltVlan);
    changed = true;
  }

  // Make sure all interfaces refer to valid VLANs.
  for (const auto& vlanInfo : vlanInterfaces_) {
    if (newVlans->getVlanIf(vlanInfo.first) == nullptr) {
      throw FbossError(
          "Interface ",
          *(vlanInfo.second.interfaces.begin()),
          " refers to non-existent VLAN ",
          vlanInfo.first);
    }
    // Make sure there is a one-to-one map between vlan and interface
    // Remove this sanity check if multiple interfaces are allowed per vlans
    auto& entry = vlanInterfaces_[vlanInfo.first];
    if (entry.interfaces.size() != 1) {
      auto cpu_vlan = new_->getDefaultVlan();
      if (vlanInfo.first != cpu_vlan) {
        throw FbossError(
            "Vlan ",
            vlanInfo.first,
            " refers to ",
            entry.interfaces.size(),
            " interfaces ");
      }
    }
  }

  std::chrono::seconds arpAgerInterval(cfg_->arpAgerInterval);
  if (orig_->getArpAgerInterval() != arpAgerInterval) {
    new_->setArpAgerInterval(arpAgerInterval);
    changed = true;
  }

  std::chrono::seconds arpTimeout(cfg_->arpTimeoutSeconds);
  if (orig_->getArpTimeout() != arpTimeout) {
    new_->setArpTimeout(arpTimeout);

    // TODO(aeckert): add ndpTimeout field to SwitchConfig. For now use the same
    // timeout for both ARP and NDP
    new_->setNdpTimeout(arpTimeout);
    changed = true;
  }

  uint32_t maxNeighborProbes(cfg_->maxNeighborProbes);
  if (orig_->getMaxNeighborProbes() != maxNeighborProbes) {
    new_->setMaxNeighborProbes(maxNeighborProbes);
    changed = true;
  }

  auto oldDhcpV4RelaySrc = orig_->getDhcpV4RelaySrc();
  auto newDhcpV4RelaySrc = cfg_->__isset.dhcpRelaySrcOverrideV4
      ? IPAddressV4(cfg_->dhcpRelaySrcOverrideV4_ref().value_unchecked())
      : IPAddressV4();
  if (oldDhcpV4RelaySrc != newDhcpV4RelaySrc) {
    new_->setDhcpV4RelaySrc(newDhcpV4RelaySrc);
    changed = true;
  }

  auto oldDhcpV6RelaySrc = orig_->getDhcpV6RelaySrc();
  auto newDhcpV6RelaySrc = cfg_->__isset.dhcpRelaySrcOverrideV6
      ? IPAddressV6(cfg_->dhcpRelaySrcOverrideV6_ref().value_unchecked())
      : IPAddressV6("::");
  if (oldDhcpV6RelaySrc != newDhcpV6RelaySrc) {
    new_->setDhcpV6RelaySrc(newDhcpV6RelaySrc);
    changed = true;
  }

  auto oldDhcpV4ReplySrc = orig_->getDhcpV4ReplySrc();
  auto newDhcpV4ReplySrc = cfg_->__isset.dhcpReplySrcOverrideV4
      ? IPAddressV4(cfg_->dhcpReplySrcOverrideV4_ref().value_unchecked())
      : IPAddressV4();
  if (oldDhcpV4ReplySrc != newDhcpV4ReplySrc) {
    new_->setDhcpV4ReplySrc(newDhcpV4ReplySrc);
    changed = true;
  }

  auto oldDhcpV6ReplySrc = orig_->getDhcpV6ReplySrc();
  auto newDhcpV6ReplySrc = cfg_->__isset.dhcpReplySrcOverrideV6
      ? IPAddressV6(cfg_->dhcpReplySrcOverrideV6_ref().value_unchecked())
      : IPAddressV6("::");
  if (oldDhcpV6ReplySrc != newDhcpV6ReplySrc) {
    new_->setDhcpV6ReplySrc(newDhcpV6ReplySrc);
    changed = true;
  }

  std::chrono::seconds staleEntryInterval(cfg_->staleEntryInterval);
  if (orig_->getStaleEntryInterval() != staleEntryInterval) {
    new_->setStaleEntryInterval(staleEntryInterval);
    changed = true;
  }

  // Add sFlow collectors
  {
    auto newCollectors = updateSflowCollectors();
    if (newCollectors) {
      new_->resetSflowCollectors(std::move(newCollectors));
      changed = true;
    }
  }

  {
    LoadBalancerConfigApplier loadBalancerConfigApplier(
        orig_->getLoadBalancers(), cfg_->get_loadBalancers(), platform_);
    auto newLoadBalancers = loadBalancerConfigApplier.updateLoadBalancers();
    if (newLoadBalancers) {
      new_->resetLoadBalancers(std::move(newLoadBalancers));
      changed = true;
    }
  }

  if (!changed) {
    return nullptr;
  }
  return new_;
}

void ThriftConfigApplier::processVlanPorts() {
  // Build the Port --> Vlan mappings
  //
  // The config file has a separate list for this data,
  // but it is stored in the state tree as part of both the PortMap and the
  // VlanMap.
  for (const auto& vp : cfg_->vlanPorts) {
    PortID portID(vp.logicalPort);
    VlanID vlanID(vp.vlanID);
    auto ret1 = portVlans_[portID].insert(
        std::make_pair(vlanID, Port::VlanInfo(vp.emitTags)));
    if (!ret1.second) {
      throw FbossError(
          "duplicate VlanPort for port ", portID, ", vlan ", vlanID);
    }
    auto ret2 = vlanPorts_[vlanID].insert(
        std::make_pair(portID, Vlan::PortInfo(vp.emitTags)));
    if (!ret2.second) {
      // This should never fail if the first insert succeeded above.
      throw FbossError(
          "duplicate VlanPort for vlan ", vlanID, ", port ", portID);
    }
  }
}

void ThriftConfigApplier::updateVlanInterfaces(const Interface* intf) {
  auto& entry = vlanInterfaces_[intf->getVlanID()];

  // Each VLAN can only be used with a single virtual router
  if (entry.interfaces.empty()) {
    entry.routerID = intf->getRouterID();
  } else {
    if (intf->getRouterID() != entry.routerID) {
      throw FbossError(
          "VLAN ",
          intf->getVlanID(),
          " configured in multiple "
          "different virtual routers: ",
          entry.routerID,
          " and ",
          intf->getRouterID());
    }
  }

  auto intfRet = entry.interfaces.insert(intf->getID());
  if (!intfRet.second) {
    // This shouldn't happen
    throw FbossError(
        "interface ",
        intf->getID(),
        " processed twice for "
        "VLAN ",
        intf->getVlanID());
  }

  for (const auto& ipMask : intf->getAddresses()) {
    VlanIpInfo info(ipMask.second, intf->getMac(), intf->getID());
    auto ret = entry.addresses.emplace(ipMask.first, info);
    if (ret.second) {
      continue;
    }
    // Allow multiple interfaces on the same VLAN with the same IP,
    // as long as they also share the same mask and MAC address.
    const auto& oldInfo = ret.first->second;
    if (oldInfo.mask != info.mask) {
      throw FbossError(
          "VLAN ",
          intf->getVlanID(),
          " has IP ",
          ipMask.first,
          " configured multiple times with different masks (",
          oldInfo.mask,
          " and ",
          info.mask,
          ")");
    }
    if (oldInfo.mac != info.mac) {
      throw FbossError(
          "VLAN ",
          intf->getVlanID(),
          " has IP ",
          ipMask.first,
          " configured multiple times with different MACs (",
          oldInfo.mac,
          " and ",
          info.mac,
          ")");
    }
  }

  // Also add the link-local IPv6 address
  IPAddressV6 linkLocalAddr(IPAddressV6::LINK_LOCAL, intf->getMac());
  VlanIpInfo linkLocalInfo(64, intf->getMac(), intf->getID());
  entry.addresses.emplace(IPAddress(linkLocalAddr), linkLocalInfo);
}

shared_ptr<PortMap> ThriftConfigApplier::updatePorts() {
  auto origPorts = orig_->getPorts();
  PortMap::NodeContainer newPorts;
  bool changed = false;

  // Process all supplied port configs
  for (const auto& portCfg : cfg_->ports) {
    PortID id(portCfg.logicalID);
    auto origPort = origPorts->getPortIf(id);
    std::shared_ptr<Port> newPort;
    if (!origPort) {
      auto port = std::make_shared<Port>(
          PortID(portCfg.logicalID), portCfg.name_ref().value_unchecked());
      newPort = updatePort(port, &portCfg);
    } else {
      newPort = updatePort(origPort, &portCfg);
    }
    changed |= updateMap(&newPorts, origPort, newPort);
  }

  // Find all ports that didn't have a config listed
  // and reset them to their default (disabled) state.
  for (const auto& origPort : *origPorts) {
    if (newPorts.find(origPort->getID()) != newPorts.end()) {
      // This port was listed in the config, and has already been configured
      continue;
    }
    cfg::Port defaultConfig;
    origPort->initDefaultConfigState(&defaultConfig);
    auto newPort = updatePort(origPort, &defaultConfig);
    changed |= updateMap(&newPorts, origPort, newPort);
  }

  if (!changed) {
    return nullptr;
  }

  return origPorts->clone(newPorts);
}

void ThriftConfigApplier::checkPortQueueAQMValid(
    const std::vector<cfg::ActiveQueueManagement>& aqms) {
  if (aqms.empty()) {
    return;
  }
  std::set<cfg::QueueCongestionBehavior> behaviors;
  for (const auto& aqm : aqms) {
    if (aqm.detection.getType() ==
        cfg::QueueCongestionDetection::Type::__EMPTY__) {
      throw FbossError(
          "Active Queue Management must specify a congestion detection method");
    }
    if (behaviors.find(aqm.behavior) != behaviors.end()) {
      throw FbossError("Same Active Queue Management behavior already exists");
    }
    behaviors.insert(aqm.behavior);
  }
}

std::shared_ptr<PortQueue> ThriftConfigApplier::updatePortQueue(
    const std::shared_ptr<PortQueue>& orig,
    const cfg::PortQueue* cfg,
    const std::optional<cfg::QosMap>& qosMap) {
  CHECK_EQ(orig->getID(), cfg->id);

  std::optional<TrafficClass> trafficClass;
  if (qosMap) {
    for (auto entry : qosMap->trafficClassToQueueId) {
      if (entry.second == cfg->id) {
        trafficClass = static_cast<TrafficClass>(entry.first);
        break;
      }
    }
  }
  if (checkSwConfPortQueueMatch(orig, cfg) &&
      trafficClass == orig->getTrafficClass()) {
    return orig;
  }

  // We should always use the PortQueue settings from config, so that if some of
  // the attributes is removed from config, we can make sure that attribute can
  // set back to default
  return createPortQueue(cfg, trafficClass);
}

std::shared_ptr<PortQueue> ThriftConfigApplier::createPortQueue(
    const cfg::PortQueue* cfg,
    std::optional<TrafficClass> trafficClass) {
  auto queue = std::make_shared<PortQueue>(static_cast<uint8_t>(cfg->id));
  queue->setStreamType(cfg->streamType);
  queue->setScheduling(cfg->scheduling);
  if (cfg->__isset.weight) {
    queue->setWeight(cfg->weight_ref().value_unchecked());
  }
  if (cfg->__isset.reservedBytes) {
    queue->setReservedBytes(cfg->reservedBytes_ref().value_unchecked());
  }
  if (cfg->__isset.scalingFactor) {
    queue->setScalingFactor(cfg->scalingFactor_ref().value_unchecked());
  }
  if (cfg->__isset.aqms) {
    checkPortQueueAQMValid(cfg->aqms_ref().value_unchecked());
    queue->resetAqms(cfg->aqms_ref().value_unchecked());
  }
  if (cfg->__isset.sharedBytes) {
    queue->setSharedBytes(cfg->sharedBytes_ref().value_unchecked());
  }
  if (cfg->__isset.name) {
    queue->setName(cfg->name_ref().value_unchecked());
  }
  if (cfg->__isset.portQueueRate) {
    queue->setPortQueueRate(cfg->portQueueRate_ref().value_unchecked());
  }

  if (cfg->__isset.bandwidthBurstMinKbits) {
    queue->setBandwidthBurstMinKbits(
        cfg->bandwidthBurstMinKbits_ref().value_unchecked());
  }

  if (cfg->__isset.bandwidthBurstMaxKbits) {
    queue->setBandwidthBurstMaxKbits(
        cfg->bandwidthBurstMaxKbits_ref().value_unchecked());
  }
  if (trafficClass) {
    queue->setTrafficClasses(trafficClass.value());
  }
  return queue;
}

QueueConfig ThriftConfigApplier::updatePortQueues(
    const QueueConfig& origPortQueues,
    const std::vector<cfg::PortQueue>& cfgPortQueues,
    std::optional<cfg::QosMap> qosMap) {
  QueueConfig newPortQueues;

  /*
   * By default, queue config is picked from defaultPortQueues. However, per
   * port queue config, if specified, overrides it.
   */
  flat_map<int, const cfg::PortQueue*> newQueues;
  for (const auto& queue : cfgPortQueues) {
    newQueues.emplace(std::make_pair(queue.id, &queue));
  }

  if (newQueues.empty()) {
    for (const auto& queue : cfg_->defaultPortQueues) {
      newQueues.emplace(std::make_pair(queue.id, &queue));
    }
  }

  // Process all supplied queues
  // We retrieve the current port queue values from hardware
  // if there is a config present for any of these queues, we update the
  // PortQueue according to this
  // Otherwise we reset it to the default values for this queue type
  for (int i = 0; i < origPortQueues.size(); i++) {
    auto newQueueIter = newQueues.find(i);
    auto newQueue = std::make_shared<PortQueue>(static_cast<uint8_t>(i));
    newQueue->setStreamType(origPortQueues.at(i)->getStreamType());
    if (newQueueIter != newQueues.end()) {
      newQueue =
          updatePortQueue(origPortQueues.at(i), newQueueIter->second, qosMap);
      newQueues.erase(newQueueIter);
    }
    newPortQueues.push_back(newQueue);
  }

  if (newQueues.size() > 0) {
    throw FbossError(
        "Port queue config listed for invalid queues. Maximum",
        " number of queues on this platform is ",
        origPortQueues.size());
  }
  return newPortQueues;
}

shared_ptr<Port> ThriftConfigApplier::updatePort(
    const shared_ptr<Port>& orig,
    const cfg::Port* portConf) {
  CHECK_EQ(orig->getID(), portConf->logicalID);

  auto vlans = portVlans_[orig->getID()];

  std::vector<cfg::PortQueue> cfgPortQueues;
  if (portConf->__isset.portQueueConfigName) {
    auto it = cfg_->portQueueConfigs.find(
        portConf->portQueueConfigName_ref().value_unchecked());
    if (it == cfg_->portQueueConfigs.end()) {
      throw FbossError(
          "Port queue config name: ",
          portConf->portQueueConfigName_ref().value_unchecked(),
          " does not exist in PortQueueConfig map");
    }

    cfgPortQueues = it->second;
  }

  const auto& oldIngressMirror = orig->getIngressMirror();
  const auto& oldEgressMirror = orig->getEgressMirror();
  auto newIngressMirror = std::optional<std::string>();
  auto newEgressMirror = std::optional<std::string>();
  if (portConf->__isset.ingressMirror) {
    newIngressMirror = portConf->ingressMirror_ref().value_unchecked();
  }
  if (portConf->__isset.egressMirror) {
    newEgressMirror = portConf->egressMirror_ref().value_unchecked();
  }
  bool mirrorsUnChanged = (oldIngressMirror == newIngressMirror) &&
      (oldEgressMirror == newEgressMirror);

  auto newQosPolicy = std::optional<std::string>();
  if (auto dataPlaneTrafficPolicy = cfg_->dataPlaneTrafficPolicy_ref()) {
    if (auto defaultDataPlaneQosPolicy =
            dataPlaneTrafficPolicy->defaultQosPolicy_ref()) {
      newQosPolicy = *defaultDataPlaneQosPolicy;
    }
    if (auto portIdToQosPolicy =
            dataPlaneTrafficPolicy->portIdToQosPolicy_ref()) {
      auto qosPolicyItr = portIdToQosPolicy->find(portConf->logicalID);
      if (qosPolicyItr != portIdToQosPolicy->end()) {
        newQosPolicy = qosPolicyItr->second;
      }
    }
  }

  std::optional<cfg::QosMap> qosMap;
  if (newQosPolicy) {
    for (auto qosPolicy : cfg_->qosPolicies) {
      if (qosPolicy.name == newQosPolicy.value() && qosPolicy.qosMap_ref()) {
        qosMap = qosPolicy.qosMap_ref().value();
      }
    }
  }

  auto portQueues =
      updatePortQueues(orig->getPortQueues(), cfgPortQueues, qosMap);
  bool queuesUnchanged = portQueues.size() == orig->getPortQueues().size();
  for (int i = 0; i < portQueues.size() && queuesUnchanged; i++) {
    if (*(portQueues.at(i)) != *(orig->getPortQueues().at(i))) {
      queuesUnchanged = false;
      break;
    }
  }

  auto newSampleDest = std::optional<cfg::SampleDestination>();
  if (portConf->sampleDest_ref()) {
    newSampleDest = portConf->sampleDest_ref().value();

    if (newSampleDest.value() == cfg::SampleDestination::MIRROR &&
        portConf->sFlowEgressRate > 0) {
      throw FbossError(
          "Port ",
          orig->getID(),
          ": Egress sampling to mirror destination is unsupported");
    }
  }

  // Ensure portConf has actually changed, before applying
  if (portConf->state == orig->getAdminState() &&
      VlanID(portConf->ingressVlan) == orig->getIngressVlan() &&
      portConf->speed == orig->getSpeed() &&
      portConf->profileID == orig->getProfileID() &&
      portConf->pause == orig->getPause() &&
      portConf->sFlowIngressRate == orig->getSflowIngressRate() &&
      portConf->sFlowEgressRate == orig->getSflowEgressRate() &&
      newSampleDest == orig->getSampleDestination() &&
      portConf->name_ref().value_unchecked() == orig->getName() &&
      portConf->description_ref().value_unchecked() == orig->getDescription() &&
      vlans == orig->getVlans() && portConf->fec == orig->getFEC() &&
      queuesUnchanged && portConf->loopbackMode == orig->getLoopbackMode() &&
      mirrorsUnChanged && newQosPolicy == orig->getQosPolicy() &&
      portConf->expectedLLDPValues == orig->getLLDPValidations()) {
    return nullptr;
  }

  auto newPort = orig->clone();

  auto lldpmap = newPort->getLLDPValidations();
  for (const auto& tag : portConf->expectedLLDPValues) {
    lldpmap[tag.first] = tag.second;
  }

  newPort->setAdminState(portConf->state);
  newPort->setIngressVlan(VlanID(portConf->ingressVlan));
  newPort->setVlans(vlans);
  newPort->setSpeed(portConf->speed);
  newPort->setProfileId(portConf->profileID);
  newPort->setPause(portConf->pause);
  newPort->setSflowIngressRate(portConf->sFlowIngressRate);
  newPort->setSflowEgressRate(portConf->sFlowEgressRate);
  newPort->setSampleDestination(newSampleDest);
  newPort->setName(portConf->name_ref().value_unchecked());
  newPort->setDescription(portConf->description_ref().value_unchecked());
  newPort->setFEC(portConf->fec);
  newPort->setLoopbackMode(portConf->loopbackMode);
  newPort->resetPortQueues(portQueues);
  newPort->setIngressMirror(newIngressMirror);
  newPort->setEgressMirror(newEgressMirror);
  newPort->setQosPolicy(newQosPolicy);
  newPort->setExpectedLLDPValues(lldpmap);
  newPort->setLookupClassesToDistributeTrafficOn(portConf->lookupClasses);
  return newPort;
}

shared_ptr<AggregatePortMap> ThriftConfigApplier::updateAggregatePorts() {
  auto origAggPorts = orig_->getAggregatePorts();
  AggregatePortMap::NodeContainer newAggPorts;
  bool changed = false;

  size_t numExistingProcessed = 0;
  for (const auto& portCfg : cfg_->aggregatePorts) {
    AggregatePortID id(portCfg.key);
    auto origAggPort = origAggPorts->getAggregatePortIf(id);

    shared_ptr<AggregatePort> newAggPort;
    if (origAggPort) {
      newAggPort = updateAggPort(origAggPort, portCfg);
      ++numExistingProcessed;
    } else {
      newAggPort = createAggPort(portCfg);
    }

    changed |= updateMap(&newAggPorts, origAggPort, newAggPort);
  }

  if (numExistingProcessed != origAggPorts->size()) {
    // Some existing aggregate ports were removed.
    CHECK_LE(numExistingProcessed, origAggPorts->size());
    changed = true;
  }

  if (!changed) {
    return nullptr;
  }

  return origAggPorts->clone(newAggPorts);
}

shared_ptr<AggregatePort> ThriftConfigApplier::updateAggPort(
    const shared_ptr<AggregatePort>& origAggPort,
    const cfg::AggregatePort& cfg) {
  CHECK_EQ(origAggPort->getID(), cfg.key);

  auto cfgSubports = getSubportsSorted(cfg);
  auto origSubports = origAggPort->sortedSubports();

  uint16_t cfgSystemPriority;
  folly::MacAddress cfgSystemID;
  std::tie(cfgSystemID, cfgSystemPriority) = getSystemLacpConfig();

  auto cfgMinLinkCount = computeMinimumLinkCount(cfg);

  if (origAggPort->getName() == cfg.name &&
      origAggPort->getDescription() == cfg.description &&
      origAggPort->getSystemPriority() == cfgSystemPriority &&
      origAggPort->getSystemID() == cfgSystemID &&
      origAggPort->getMinimumLinkCount() == cfgMinLinkCount &&
      std::equal(
          origSubports.begin(), origSubports.end(), cfgSubports.begin())) {
    return nullptr;
  }

  auto newAggPort = origAggPort->clone();
  newAggPort->setName(cfg.name);
  newAggPort->setDescription(cfg.description);
  newAggPort->setSystemPriority(cfgSystemPriority);
  newAggPort->setSystemID(cfgSystemID);
  newAggPort->setMinimumLinkCount(cfgMinLinkCount);
  newAggPort->setSubports(folly::range(cfgSubports.begin(), cfgSubports.end()));

  return newAggPort;
}

shared_ptr<AggregatePort> ThriftConfigApplier::createAggPort(
    const cfg::AggregatePort& cfg) {
  auto subports = getSubportsSorted(cfg);

  uint16_t cfgSystemPriority;
  folly::MacAddress cfgSystemID;
  std::tie(cfgSystemID, cfgSystemPriority) = getSystemLacpConfig();

  auto cfgMinLinkCount = computeMinimumLinkCount(cfg);

  return AggregatePort::fromSubportRange(
      AggregatePortID(cfg.key),
      cfg.name,
      cfg.description,
      cfgSystemPriority,
      cfgSystemID,
      cfgMinLinkCount,
      folly::range(subports.begin(), subports.end()));
}

std::vector<AggregatePort::Subport> ThriftConfigApplier::getSubportsSorted(
    const cfg::AggregatePort& cfg) {
  std::vector<AggregatePort::Subport> subports(
      std::distance(cfg.memberPorts.begin(), cfg.memberPorts.end()));

  for (int i = 0; i < subports.size(); ++i) {
    if (cfg.memberPorts[i].priority < 0 ||
        cfg.memberPorts[i].priority >= 1 << 16) {
      throw FbossError("Member port ", i, " has priority outside of [0, 2^16)");
    }

    auto id = PortID(cfg.memberPorts[i].memberPortID);
    auto priority = static_cast<uint16_t>(cfg.memberPorts[i].priority);
    auto rate = cfg.memberPorts[i].rate;
    auto activity = cfg.memberPorts[i].activity;

    subports[i] = AggregatePort::Subport(id, priority, rate, activity);
  }

  std::sort(subports.begin(), subports.end());

  return subports;
}

std::pair<folly::MacAddress, uint16_t>
ThriftConfigApplier::getSystemLacpConfig() {
  folly::MacAddress systemID;
  uint16_t systemPriority;

  if (cfg_->__isset.lacp) {
    systemID = MacAddress(cfg_->lacp_ref().value_unchecked().systemID);
    systemPriority = cfg_->lacp_ref().value_unchecked().systemPriority;
  } else {
    // If the system LACP configuration parameters were not specified,
    // we fall back to default parameters. Since the default system ID
    // is not a compile-time constant (it is derived from the CPU mac),
    // the default value is defined here, instead of, say,
    // AggregatePortFields::kDefaultSystemID.
    systemID = platform_->getLocalMac();
    systemPriority = kDefaultSystemPriority;
  }

  return std::make_pair(systemID, systemPriority);
}

uint8_t ThriftConfigApplier::computeMinimumLinkCount(
    const cfg::AggregatePort& cfg) {
  uint8_t minLinkCount = 1;

  auto minCapacity = cfg.minimumCapacity;
  switch (minCapacity.getType()) {
    case cfg::MinimumCapacity::Type::linkCount:
      // Thrift's byte type is an int8_t
      CHECK_GE(minCapacity.get_linkCount(), 1);

      minLinkCount = minCapacity.get_linkCount();
      break;
    case cfg::MinimumCapacity::Type::linkPercentage:
      CHECK_GT(minCapacity.get_linkPercentage(), 0);
      CHECK_LE(minCapacity.get_linkPercentage(), 1);

      minLinkCount = std::ceil(
          minCapacity.get_linkPercentage() *
          std::distance(cfg.memberPorts.begin(), cfg.memberPorts.end()));
      if (std::distance(cfg.memberPorts.begin(), cfg.memberPorts.end()) != 0) {
        CHECK_GE(minLinkCount, 1);
      }

      break;
    case cfg::MinimumCapacity::Type::__EMPTY__:
    // needed to handle error from -Werror=switch
    default:
      folly::assume_unreachable();
      break;
  }

  return minLinkCount;
}

shared_ptr<VlanMap> ThriftConfigApplier::updateVlans() {
  auto origVlans = orig_->getVlans();
  VlanMap::NodeContainer newVlans;
  bool changed = false;

  // Process all supplied VLAN configs
  size_t numExistingProcessed = 0;
  for (const auto& vlanCfg : cfg_->vlans) {
    VlanID id(vlanCfg.id);
    auto origVlan = origVlans->getVlanIf(id);
    shared_ptr<Vlan> newVlan;
    if (origVlan) {
      newVlan = updateVlan(origVlan, &vlanCfg);
      ++numExistingProcessed;
    } else {
      newVlan = createVlan(&vlanCfg);
    }
    changed |= updateMap(&newVlans, origVlan, newVlan);
  }

  if (numExistingProcessed != origVlans->size()) {
    // Some existing VLANs were removed.
    CHECK_LT(numExistingProcessed, origVlans->size());
    changed = true;
  }

  if (!changed) {
    return nullptr;
  }

  return origVlans->clone(std::move(newVlans));
}

shared_ptr<Vlan> ThriftConfigApplier::createVlan(const cfg::Vlan* config) {
  const auto& ports = vlanPorts_[VlanID(config->id)];
  auto vlan = make_shared<Vlan>(config, ports);
  updateNeighborResponseTables(vlan.get(), config);
  updateDhcpOverrides(vlan.get(), config);

  /* TODO t7153326: Following code is added for backward compatibility
  Remove it once coop generates config with */
  if (config->__isset.intfID) {
    vlan->setInterfaceID(InterfaceID(config->intfID_ref().value_unchecked()));
  } else {
    auto& entry = vlanInterfaces_[VlanID(config->id)];
    if (!entry.interfaces.empty()) {
      vlan->setInterfaceID(*(entry.interfaces.begin()));
    }
  }
  return vlan;
}

shared_ptr<Vlan> ThriftConfigApplier::updateVlan(
    const shared_ptr<Vlan>& orig,
    const cfg::Vlan* config) {
  CHECK_EQ(orig->getID(), config->id);
  const auto& ports = vlanPorts_[orig->getID()];

  auto newVlan = orig->clone();
  bool changed_neighbor_table =
      updateNeighborResponseTables(newVlan.get(), config);
  bool changed_dhcp_overrides = updateDhcpOverrides(newVlan.get(), config);
  auto oldDhcpV4Relay = orig->getDhcpV4Relay();
  auto newDhcpV4Relay = config->__isset.dhcpRelayAddressV4
      ? IPAddressV4(config->dhcpRelayAddressV4_ref().value_unchecked())
      : IPAddressV4();

  auto oldDhcpV6Relay = orig->getDhcpV6Relay();
  auto newDhcpV6Relay = config->__isset.dhcpRelayAddressV6
      ? IPAddressV6(config->dhcpRelayAddressV6_ref().value_unchecked())
      : IPAddressV6("::");
  /* TODO t7153326: Following code is added for backward compatibility
  Remove it once coop generates config with intfID */
  auto oldIntfID = orig->getInterfaceID();
  auto newIntfID = InterfaceID(0);
  if (config->__isset.intfID) {
    newIntfID = InterfaceID(config->intfID_ref().value_unchecked());
  } else {
    auto& entry = vlanInterfaces_[VlanID(config->id)];
    if (!entry.interfaces.empty()) {
      newIntfID = *(entry.interfaces.begin());
    }
  }

  if (orig->getName() == config->name && oldIntfID == newIntfID &&
      orig->getPorts() == ports && oldDhcpV4Relay == newDhcpV4Relay &&
      oldDhcpV6Relay == newDhcpV6Relay && !changed_neighbor_table &&
      !changed_dhcp_overrides) {
    return nullptr;
  }

  newVlan->setName(config->name);
  newVlan->setInterfaceID(newIntfID);
  newVlan->setPorts(ports);
  newVlan->setDhcpV4Relay(newDhcpV4Relay);
  newVlan->setDhcpV6Relay(newDhcpV6Relay);
  return newVlan;
}

std::shared_ptr<QosPolicyMap> ThriftConfigApplier::updateQosPolicies() {
  QosPolicyMap::NodeContainer newQosPolicies;
  bool changed = false;
  int numExistingProcessed = 0;
  auto defaultDataPlaneQosPolicyName = getDefaultDataPlaneQosPolicyName();

  auto qosPolicies = cfg_->qosPolicies;
  for (auto& qosPolicy : qosPolicies) {
    if (defaultDataPlaneQosPolicyName.has_value() &&
        defaultDataPlaneQosPolicyName.value() == qosPolicy.name) {
      // skip default QosPolicy as it will be maintained in switch state
      continue;
    }
    auto newQosPolicy =
        updateQosPolicy(qosPolicy, &numExistingProcessed, &changed);
    if (!newQosPolicies.emplace(qosPolicy.name, newQosPolicy).second) {
      throw FbossError(
          "Invalid config: Qos Policy \"", qosPolicy.name, "\" already exists");
    }
  }
  if (numExistingProcessed != orig_->getQosPolicies()->size()) {
    // Some existing Qos Policies were removed.
    changed = true;
  }
  if (!changed) {
    return nullptr;
  }
  return orig_->getQosPolicies()->clone(std::move(newQosPolicies));
}

std::shared_ptr<QosPolicy> ThriftConfigApplier::updateQosPolicy(
    cfg::QosPolicy& qosPolicy,
    int* numExistingProcessed,
    bool* changed) {
  auto origQosPolicy = orig_->getQosPolicies()->getQosPolicyIf(qosPolicy.name);
  auto newQosPolicy = createQosPolicy(qosPolicy);
  if (origQosPolicy) {
    ++(*numExistingProcessed);
    if (*origQosPolicy == *newQosPolicy) {
      return origQosPolicy;
    }
  }
  *changed = true;
  return newQosPolicy;
}

std::optional<std::string>
ThriftConfigApplier::getDefaultDataPlaneQosPolicyName() const {
  if (auto dataPlaneTrafficPolicy = cfg_->dataPlaneTrafficPolicy_ref()) {
    if (auto defaultDataPlaneQosPolicy =
            dataPlaneTrafficPolicy->defaultQosPolicy_ref()) {
      return *defaultDataPlaneQosPolicy;
    }
  }
  return std::nullopt;
}

std::shared_ptr<QosPolicy>
ThriftConfigApplier::updateDataplaneDefaultQosPolicy() {
  auto defaultDataPlaneQosPolicyName = getDefaultDataPlaneQosPolicyName();
  if (!defaultDataPlaneQosPolicyName) {
    return nullptr;
  }
  std::shared_ptr<QosPolicy> newQosPolicy = nullptr;
  auto qosPolicies = cfg_->qosPolicies;
  for (auto& qosPolicy : qosPolicies) {
    if (defaultDataPlaneQosPolicyName == qosPolicy.name) {
      newQosPolicy = createQosPolicy(qosPolicy);
      break;
    }
  }
  auto oldQosPolicy = orig_->getDefaultDataPlaneQosPolicy();
  if (oldQosPolicy && newQosPolicy && *oldQosPolicy == *newQosPolicy) {
    return oldQosPolicy;
  }
  return newQosPolicy;
}

shared_ptr<QosPolicy> ThriftConfigApplier::createQosPolicy(
    const cfg::QosPolicy& qosPolicy) {
  if (qosPolicy.rules.empty() == !qosPolicy.qosMap_ref().has_value()) {
    throw FbossError(
        "either the qos rules or qos maps must be provided but not both!");
  }

  DscpMap ingressDscpMap;
  for (const auto& qosRule : qosPolicy.rules) {
    if (qosRule.dscp.empty()) {
      throw FbossError("Invalid config: qosPolicy: empty dscp list");
    }
    for (const auto& dscpValue : qosRule.dscp) {
      if (dscpValue < 0 || dscpValue > 63) {
        throw FbossError("dscp value is invalid (must be [0, 63])");
      }
      ingressDscpMap.addFromEntry(
          static_cast<TrafficClass>(qosRule.queueId),
          static_cast<DSCP>(dscpValue));
    }
  }

  if (auto qosMap = qosPolicy.qosMap_ref()) {
    DscpMap dscpMap(qosMap->dscpMaps);
    ExpMap expMap(qosMap->expMaps);
    QosPolicy::TrafficClassToQueueId trafficClassToQueueId;
    for (auto cfgTrafficClassToQueueId : qosMap->trafficClassToQueueId) {
      trafficClassToQueueId.emplace(
          cfgTrafficClassToQueueId.first, cfgTrafficClassToQueueId.second);
    }
    return make_shared<QosPolicy>(
        qosPolicy.name, dscpMap, expMap, trafficClassToQueueId);
  }
  return make_shared<QosPolicy>(
      qosPolicy.name,
      ingressDscpMap,
      ExpMap(TrafficClassToQosAttributeMap<EXP>()),
      QosPolicy::TrafficClassToQueueId());
}

std::shared_ptr<AclMap> ThriftConfigApplier::updateAcls() {
  AclMap::NodeContainer newAcls;
  bool changed = false;
  int numExistingProcessed = 0;
  int priority = kAclStartPriority;
  int cpuPriority = 1;

  // Start with the DROP acls, these should have highest priority
  auto acls = folly::gen::from(cfg_->acls) |
      folly::gen::filter([](const cfg::AclEntry& entry) {
                return entry.actionType == cfg::AclActionType::DENY;
              }) |
      folly::gen::map([&](const cfg::AclEntry& entry) {
                auto acl = updateAcl(
                    entry, priority++, &numExistingProcessed, &changed);
                return std::make_pair(acl->getID(), acl);
              }) |
      folly::gen::appendTo(newAcls);

  // Let's get a map of acls to name so we don't have to search the acl list
  // for every new use
  flat_map<std::string, const cfg::AclEntry*> aclByName;
  folly::gen::from(cfg_->acls) | folly::gen::map([](const cfg::AclEntry& acl) {
    return std::make_pair(acl.name, &acl);
  }) | folly::gen::appendTo(aclByName);

  flat_map<std::string, const cfg::TrafficCounter*> counterByName;
  folly::gen::from(cfg_->trafficCounters) |
      folly::gen::map([](const cfg::TrafficCounter& counter) {
        return std::make_pair(counter.name, &counter);
      }) |
      folly::gen::appendTo(counterByName);

  // Generates new acls from template
  auto addToAcls = [&](const cfg::TrafficPolicyConfig& policy,
                       bool isCoppAcl = false)
      -> const std::vector<std::pair<std::string, std::shared_ptr<AclEntry>>> {
    std::vector<std::pair<std::string, std::shared_ptr<AclEntry>>> entries;
    for (const auto& mta : policy.matchToAction) {
      auto a = aclByName.find(mta.matcher);
      if (a == aclByName.end()) {
        throw FbossError(
            "Invalid config: No acl named ", mta.matcher, " found.");
      }

      auto aclCfg = *(a->second);

      // We've already added any DENY acls
      if (aclCfg.actionType == cfg::AclActionType::DENY) {
        continue;
      }

      // Here is sending to regular port queue action
      MatchAction matchAction = MatchAction();
      if (mta.action.__isset.sendToQueue) {
        matchAction.setSendToQueue(std::make_pair(
            mta.action.sendToQueue_ref().value_unchecked(), isCoppAcl));
      }
      if (mta.action.__isset.counter) {
        auto counter =
            counterByName.find(mta.action.counter_ref().value_unchecked());
        if (counter == counterByName.end()) {
          throw FbossError(
              "Invalid config: No counter named ",
              mta.action.counter_ref().value_unchecked(),
              " found.");
        }
        matchAction.setTrafficCounter(*(counter->second));
      }
      if (mta.action.__isset.setDscp) {
        matchAction.setSetDscp(mta.action.setDscp_ref().value_unchecked());
      }
      if (mta.action.__isset.ingressMirror) {
        matchAction.setIngressMirror(
            mta.action.ingressMirror_ref().value_unchecked());
      }
      if (mta.action.__isset.egressMirror) {
        matchAction.setEgressMirror(
            mta.action.egressMirror_ref().value_unchecked());
      }

      auto acl = updateAcl(
          aclCfg,
          isCoppAcl ? cpuPriority++ : priority++,
          &numExistingProcessed,
          &changed,
          &matchAction);

      if (acl->getAclAction().has_value()) {
        const auto& inMirror = acl->getAclAction().value().getIngressMirror();
        const auto& egMirror = acl->getAclAction().value().getIngressMirror();
        if (inMirror.has_value() &&
            !new_->getMirrors()->getMirrorIf(inMirror.value())) {
          throw FbossError("Mirror ", inMirror.value(), " is undefined");
        }
        if (egMirror.has_value() &&
            !new_->getMirrors()->getMirrorIf(egMirror.value())) {
          throw FbossError("Mirror ", egMirror.value(), " is undefined");
        }
      }
      entries.push_back(std::make_pair(acl->getID(), acl));
    }
    return entries;
  };

  // Add controlPlane traffic acls
  if (cfg_->cpuTrafficPolicy_ref() &&
      cfg_->cpuTrafficPolicy_ref()->trafficPolicy_ref()) {
    folly::gen::from(
        addToAcls(*cfg_->cpuTrafficPolicy_ref()->trafficPolicy_ref(), true)) |
        folly::gen::appendTo(newAcls);
  }

  // Add dataPlane traffic acls
  if (cfg_->__isset.dataPlaneTrafficPolicy) {
    folly::gen::from(
        addToAcls(cfg_->dataPlaneTrafficPolicy_ref().value_unchecked())) |
        folly::gen::appendTo(newAcls);
  }
  if (numExistingProcessed != orig_->getAcls()->size()) {
    // Some existing ACLs were removed.
    changed = true;
  }

  if (!changed) {
    return nullptr;
  }
  return orig_->getAcls()->clone(std::move(newAcls));
}

std::shared_ptr<AclEntry> ThriftConfigApplier::updateAcl(
    const cfg::AclEntry& acl,
    int priority,
    int* numExistingProcessed,
    bool* changed,
    const MatchAction* action) {
  auto origAcl = orig_->getAcls()->getEntryIf(acl.name);
  auto newAcl = createAcl(&acl, priority, action);
  if (origAcl) {
    ++(*numExistingProcessed);
    if (*origAcl == *newAcl) {
      return origAcl;
    }
  }
  *changed = true;
  return newAcl;
}

void ThriftConfigApplier::checkAcl(const cfg::AclEntry* config) const {
  // TODO(joseph5wu) The following RangeCheck needs to be deprecated once we
  // have coop rolled out to use the exact match l4 port struct everywhere.
  // check l4 port range
  if (config->__isset.srcL4PortRange &&
      (config->srcL4PortRange_ref().value_unchecked().min >
       AclEntryFields::kMaxL4Port)) {
    throw FbossError("src's L4 port range has a min value larger than 65535");
  }
  if (config->__isset.srcL4PortRange &&
      (config->srcL4PortRange_ref().value_unchecked().max >
       AclEntryFields::kMaxL4Port)) {
    throw FbossError("src's L4 port range has a max value larger than 65535");
  }
  if (config->__isset.srcL4PortRange &&
      (config->srcL4PortRange_ref().value_unchecked().min !=
       config->srcL4PortRange_ref().value_unchecked().max)) {
    throw FbossError("Only support src L4 port range min value == max value");
  }
  if (config->__isset.dstL4PortRange &&
      (config->dstL4PortRange_ref().value_unchecked().min >
       AclEntryFields::kMaxL4Port)) {
    throw FbossError("dst's L4 port range has a min value larger than 65535");
  }
  if (config->__isset.dstL4PortRange &&
      (config->dstL4PortRange_ref().value_unchecked().max >
       AclEntryFields::kMaxL4Port)) {
    throw FbossError("dst's L4 port range has a max value larger than 65535");
  }
  if (config->__isset.dstL4PortRange &&
      (config->dstL4PortRange_ref().value_unchecked().min !=
       config->dstL4PortRange_ref().value_unchecked().max)) {
    throw FbossError("Only support dst L4 port range min value == max value");
  }
  // check l4 port
  if (config->__isset.l4SrcPort &&
      (config->l4SrcPort_ref().value_unchecked() < 0 ||
       config->l4SrcPort_ref().value_unchecked() >
           AclEntryFields::kMaxL4Port)) {
    throw FbossError(
        "L4 source port must be between 0 and ",
        std::to_string(AclEntryFields::kMaxL4Port));
  }
  if (config->__isset.l4DstPort &&
      (config->l4DstPort_ref().value_unchecked() < 0 ||
       config->l4DstPort_ref().value_unchecked() >
           AclEntryFields::kMaxL4Port)) {
    throw FbossError(
        "L4 destination port must be between 0 and ",
        std::to_string(AclEntryFields::kMaxL4Port));
  }
  if (config->__isset.icmpCode && !config->__isset.icmpType) {
    throw FbossError("icmp type must be set when icmp code is set");
  }
  if (config->__isset.icmpType &&
      (config->icmpType_ref().value_unchecked() < 0 ||
       config->icmpType_ref().value_unchecked() >
           AclEntryFields::kMaxIcmpType)) {
    throw FbossError(
        "icmp type value must be between 0 and ",
        std::to_string(AclEntryFields::kMaxIcmpType));
  }
  if (config->__isset.icmpCode &&
      (config->icmpCode_ref().value_unchecked() < 0 ||
       config->icmpCode_ref().value_unchecked() >
           AclEntryFields::kMaxIcmpCode)) {
    throw FbossError(
        "icmp type value must be between 0 and ",
        std::to_string(AclEntryFields::kMaxIcmpCode));
  }
  if (config->__isset.icmpType &&
      (!config->__isset.proto ||
       !(config->proto_ref().value_unchecked() == AclEntryFields::kProtoIcmp ||
         config->proto_ref().value_unchecked() ==
             AclEntryFields::kProtoIcmpv6))) {
    throw FbossError(
        "proto must be either icmp or icmpv6 ", "if icmp type is set");
  }
  if (config->__isset.ttl && config->ttl_ref().value_unchecked().value > 255) {
    throw FbossError("ttl value is larger than 255");
  }
  if (config->__isset.ttl && config->ttl_ref().value_unchecked().value < 0) {
    throw FbossError("ttl value is less than 0");
  }
  if (config->__isset.ttl && config->ttl_ref().value_unchecked().mask > 255) {
    throw FbossError("ttl mask is larger than 255");
  }
  if (config->__isset.ttl && config->ttl_ref().value_unchecked().mask < 0) {
    throw FbossError("ttl mask is less than 0");
  }
}

shared_ptr<AclEntry> ThriftConfigApplier::createAcl(
    const cfg::AclEntry* config,
    int priority,
    const MatchAction* action) {
  checkAcl(config);
  auto newAcl = make_shared<AclEntry>(priority, config->name);
  newAcl->setActionType(config->actionType);
  if (action) {
    newAcl->setAclAction(*action);
  }
  if (config->__isset.srcIp) {
    newAcl->setSrcIp(
        IPAddress::createNetwork(config->srcIp_ref().value_unchecked()));
  }
  if (config->__isset.dstIp) {
    newAcl->setDstIp(
        IPAddress::createNetwork(config->dstIp_ref().value_unchecked()));
  }
  if (config->__isset.proto) {
    newAcl->setProto(config->proto_ref().value_unchecked());
  }
  if (config->__isset.tcpFlagsBitMap) {
    newAcl->setTcpFlagsBitMap(config->tcpFlagsBitMap_ref().value_unchecked());
  }
  if (config->__isset.srcPort) {
    newAcl->setSrcPort(config->srcPort_ref().value_unchecked());
  }
  if (config->__isset.dstPort) {
    newAcl->setDstPort(config->dstPort_ref().value_unchecked());
  }
  // TODO(joseph5wu) The following RangeCheck needs to be deprecated once we
  // have coop rolled out to use the exact match l4 port struct everywhere.
  if (config->__isset.srcL4PortRange) {
    newAcl->setL4SrcPort(config->srcL4PortRange_ref().value_unchecked().min);
  }
  if (config->__isset.dstL4PortRange) {
    newAcl->setL4DstPort(config->dstL4PortRange_ref().value_unchecked().min);
  }
  if (config->__isset.l4SrcPort) {
    newAcl->setL4SrcPort(config->l4SrcPort_ref().value_unchecked());
  }
  if (config->__isset.l4DstPort) {
    newAcl->setL4DstPort(config->l4DstPort_ref().value_unchecked());
  }
  if (config->__isset.ipFrag) {
    newAcl->setIpFrag(config->ipFrag_ref().value_unchecked());
  }
  if (config->__isset.icmpType) {
    newAcl->setIcmpType(config->icmpType_ref().value_unchecked());
  }
  if (config->__isset.icmpCode) {
    newAcl->setIcmpCode(config->icmpCode_ref().value_unchecked());
  }
  if (config->__isset.dscp) {
    newAcl->setDscp(config->dscp_ref().value_unchecked());
  }
  if (config->__isset.dstMac) {
    newAcl->setDstMac(MacAddress(config->dstMac_ref().value_unchecked()));
  }
  if (config->__isset.ipType) {
    newAcl->setIpType(config->ipType_ref().value_unchecked());
  }
  if (config->__isset.ttl) {
    newAcl->setTtl(AclTtl(
        config->ttl_ref().value_unchecked().value,
        config->ttl_ref().value_unchecked().mask));
  }
  if (config->__isset.lookupClass) {
    newAcl->setLookupClass(config->lookupClass_ref().value_unchecked());
  }
  if (config->__isset.lookupClassL2) {
    newAcl->setLookupClassL2(config->lookupClassL2_ref().value_unchecked());
  }
  return newAcl;
}

bool ThriftConfigApplier::updateDhcpOverrides(
    Vlan* vlan,
    const cfg::Vlan* config) {
  DhcpV4OverrideMap newDhcpV4OverrideMap;
  for (const auto& pair :
       config->dhcpRelayOverridesV4_ref().value_unchecked()) {
    try {
      newDhcpV4OverrideMap[MacAddress(pair.first)] = IPAddressV4(pair.second);
    } catch (const IPAddressFormatException& ex) {
      throw FbossError(
          "Invalid IPv4 address in DHCPv4 relay override map: ", ex.what());
    }
  }

  DhcpV6OverrideMap newDhcpV6OverrideMap;
  for (const auto& pair :
       config->dhcpRelayOverridesV6_ref().value_unchecked()) {
    try {
      newDhcpV6OverrideMap[MacAddress(pair.first)] = IPAddressV6(pair.second);
    } catch (const IPAddressFormatException& ex) {
      throw FbossError(
          "Invalid IPv4 address in DHCPv4 relay override map: ", ex.what());
    }
  }

  bool changed = false;
  auto oldDhcpV4OverrideMap = vlan->getDhcpV4RelayOverrides();
  if (oldDhcpV4OverrideMap != newDhcpV4OverrideMap) {
    vlan->setDhcpV4RelayOverrides(newDhcpV4OverrideMap);
    changed = true;
  }
  auto oldDhcpV6OverrideMap = vlan->getDhcpV6RelayOverrides();
  if (oldDhcpV6OverrideMap != newDhcpV6OverrideMap) {
    vlan->setDhcpV6RelayOverrides(newDhcpV6OverrideMap);
    changed = true;
  }
  return changed;
}

bool ThriftConfigApplier::updateNeighborResponseTables(
    Vlan* vlan,
    const cfg::Vlan* config) {
  auto origArp = vlan->getArpResponseTable();
  auto origNdp = vlan->getNdpResponseTable();
  ArpResponseTable::Table arpTable;
  NdpResponseTable::Table ndpTable;

  VlanID vlanID(config->id);
  auto it = vlanInterfaces_.find(vlanID);
  if (it != vlanInterfaces_.end()) {
    for (const auto& addrInfo : it->second.addresses) {
      NeighborResponseEntry entry(
          addrInfo.second.mac, addrInfo.second.interfaceID);
      if (addrInfo.first.isV4()) {
        arpTable.insert(std::make_pair(addrInfo.first.asV4(), entry));
      } else {
        ndpTable.insert(std::make_pair(addrInfo.first.asV6(), entry));
      }
    }
  }

  bool changed = false;
  if (origArp->getTable() != arpTable) {
    changed = true;
    vlan->setArpResponseTable(origArp->clone(std::move(arpTable)));
  }
  if (origNdp->getTable() != ndpTable) {
    changed = true;
    vlan->setNdpResponseTable(origNdp->clone(std::move(ndpTable)));
  }
  return changed;
}

shared_ptr<RouteTableMap> ThriftConfigApplier::updateInterfaceRoutes() {
  flat_set<RouterID> newToAddTables;
  flat_set<RouterID> oldToDeleteTables;
  RouteUpdater updater(orig_->getRouteTables());
  // add or update the interface routes
  for (const auto& table : intfRouteTables_) {
    for (const auto& entry : table.second) {
      auto intf = entry.second.first;
      const auto& addr = entry.second.second;
      auto len = entry.first.second;
      auto nhop = ResolvedNextHop(addr, intf, UCMP_DEFAULT_WEIGHT);
      updater.addRoute(
          table.first,
          addr,
          len,
          ClientID::INTERFACE_ROUTE,
          RouteNextHopEntry(
              std::move(nhop), AdminDistance::DIRECTLY_CONNECTED));
    }
    newToAddTables.insert(table.first);
  }

  // need to go through all existing connected routes and delete those
  // not there anymore
  for (const auto& intf : orig_->getInterfaces()->getAllNodes()) {
    auto id = intf.second->getRouterID();
    auto iter = intfRouteTables_.find(id);
    if (iter == intfRouteTables_.end()) {
      // if the old router ID does not exist any more, need to remove the
      // v6 link local route from it.
      oldToDeleteTables.insert(id);
    }
    for (const auto& addr : intf.second->getAddresses()) {
      auto prefix = std::make_pair(addr.first.mask(addr.second), addr.second);
      bool found = false;
      if (iter != intfRouteTables_.end()) {
        const auto& newAddrs = iter->second;
        auto iter2 = newAddrs.find(prefix);
        if (iter2 != newAddrs.end()) {
          found = true;
        }
      }
      if (!found) {
        updater.delRoute(
            id, addr.first, addr.second, ClientID::INTERFACE_ROUTE);
      }
    }
  }
  // delete v6 link route from no long existing router ID
  for (auto id : oldToDeleteTables) {
    updater.delLinkLocalRoutes(id);
  }
  // add v6 link route to the new router
  for (auto id : newToAddTables) {
    updater.addLinkLocalRoutes(id);
  }
  return updater.updateDone();
}

/**
 * syncStaticRoutes:
 *
 * A long note about why we "sync" static routes from config file.
 * To set the stage, we come here in one of two ways:
 * (a) Switch is coming up after a warm or cold boot and is loading config
 * (b) "reloadConfig" has been issued from thrift API
 *
 * In both cases, there may already exist static routes in our SwitchState.
 * (In the case of warm boot, we read and reload our state from the warm
 * boot file.)
 *
 * The intent of this function is that after we applyUpdate(), the static
 * routes in our new SwitchState will be exactly what is in the new config,
 * no more no less.  Note that this means that any static routes added using
 * addUnicastRoute() API will be removed after we reloadConfig, or when we
 * come up after restart.  This is by design.
 *
 * There are two ways to do the above.  One way would be to go through the
 * existing static routes in SwitchState and "reconcile" with that in
 * config.  I.e., add, delete, modify, or leave unchanged, as necessary.
 *
 * The second approach, the one we adopt here, not very intuitive but a lot
 * cleaner to code, is to simply delete all static routes in current state,
 * and to add back static routes from config file.  This works because the
 * "delete" in this step does not take immediate effect.  It is only the
 * state delta, after all processing is done, that is sent to the hardware
 * switch.
 *
 * As a side note, there is a third (incorrect) approach that was tried, but
 * does not work.  The old approach was to compute the delta between old and
 * new config files, and to only apply that delta.  This would work for
 * "reloadConfig", but does not work when the switch restarts, because we do
 * not save the old config.  In particular, it does not work in the case of
 * "delete", i.e., the new config does not have an entry that was there in
 * the old config, because there is no old config to compare with.
 */
std::shared_ptr<RouteTableMap> ThriftConfigApplier::syncStaticRoutes(
    const std::shared_ptr<RouteTableMap>& routes) {
  CHECK(routes != nullptr) << "RouteTableMap can not be null";
  // RouteUpdater should be able to handle nullptr and convert that into
  // into RouteTableMap. Investigate why we shouldn't do that TODO(krishnakn)
  RouteUpdater updater(routes);
  auto staticClientId = ClientID::STATIC_ROUTE;
  auto staticAdminDistance = AdminDistance::STATIC_ROUTE;
  updater.removeAllRoutesForClient(RouterID(0), staticClientId);

  if (cfg_->__isset.staticRoutesToNull) {
    for (const auto& route : cfg_->staticRoutesToNull) {
      auto prefix = folly::IPAddress::createNetwork(route.prefix);
      updater.addRoute(
          RouterID(route.routerID),
          prefix.first,
          prefix.second,
          staticClientId,
          RouteNextHopEntry(RouteForwardAction::DROP, staticAdminDistance));
    }
  }
  if (cfg_->__isset.staticRoutesToCPU) {
    for (const auto& route : cfg_->staticRoutesToCPU) {
      auto prefix = folly::IPAddress::createNetwork(route.prefix);
      updater.addRoute(
          RouterID(route.routerID),
          prefix.first,
          prefix.second,
          staticClientId,
          RouteNextHopEntry(RouteForwardAction::TO_CPU, staticAdminDistance));
    }
  }
  if (cfg_->__isset.staticRoutesWithNhops) {
    for (const auto& route : cfg_->staticRoutesWithNhops) {
      auto prefix = folly::IPAddress::createNetwork(route.prefix);
      RouteNextHopSet nhops;
      // NOTE: Static routes use the default UCMP weight so that they can be
      // compatible with UCMP, i.e., so that we can do ucmp where the next
      // hops resolve to a static route.  If we define recursive static
      // routes, that may lead to unexpected behavior where some interface
      // gets more traffic.  If necessary, in the future, we can make it
      // possible to configure strictly ECMP static routes
      for (auto& nhopStr : route.nexthops) {
        nhops.emplace(
            UnresolvedNextHop(folly::IPAddress(nhopStr), UCMP_DEFAULT_WEIGHT));
      }
      updater.addRoute(
          RouterID(route.routerID),
          prefix.first,
          prefix.second,
          staticClientId,
          RouteNextHopEntry(std::move(nhops), staticAdminDistance));
    }
  }
  return updater.updateDone();
}

std::shared_ptr<InterfaceMap> ThriftConfigApplier::updateInterfaces() {
  auto origIntfs = orig_->getInterfaces();
  InterfaceMap::NodeContainer newIntfs;
  bool changed = false;

  // Process all supplied interface configs
  size_t numExistingProcessed = 0;

  for (const auto& interfaceCfg : cfg_->interfaces) {
    InterfaceID id(interfaceCfg.intfID);
    auto origIntf = origIntfs->getInterfaceIf(id);
    shared_ptr<Interface> newIntf;
    auto newAddrs = getInterfaceAddresses(&interfaceCfg);
    if (origIntf) {
      newIntf = updateInterface(origIntf, &interfaceCfg, newAddrs);
      ++numExistingProcessed;
    } else {
      newIntf = createInterface(&interfaceCfg, newAddrs);
    }
    updateVlanInterfaces(newIntf ? newIntf.get() : origIntf.get());
    changed |= updateMap(&newIntfs, origIntf, newIntf);
  }

  if (numExistingProcessed != origIntfs->size()) {
    // Some existing interfaces were removed.
    CHECK_LT(numExistingProcessed, origIntfs->size());
    changed = true;
  }

  if (!changed) {
    return nullptr;
  }

  return origIntfs->clone(std::move(newIntfs));
}

shared_ptr<Interface> ThriftConfigApplier::createInterface(
    const cfg::Interface* config,
    const Interface::Addresses& addrs) {
  auto name = getInterfaceName(config);
  auto mac = getInterfaceMac(config);
  auto mtu = config->__isset.mtu ? config->mtu_ref().value_unchecked()
                                 : Interface::kDefaultMtu;
  auto intf = make_shared<Interface>(
      InterfaceID(config->intfID),
      RouterID(config->routerID),
      VlanID(config->vlanID),
      name,
      mac,
      mtu,
      config->isVirtual,
      config->isStateSyncDisabled);
  intf->setAddresses(addrs);
  if (config->__isset.ndp) {
    intf->setNdpConfig(config->ndp_ref().value_unchecked());
  }
  return intf;
}

shared_ptr<SflowCollectorMap> ThriftConfigApplier::updateSflowCollectors() {
  auto origCollectors = orig_->getSflowCollectors();
  SflowCollectorMap::NodeContainer newCollectors;
  bool changed = false;

  // Process all supplied collectors
  size_t numExistingProcessed = 0;
  for (const auto& collector : cfg_->sFlowCollectors) {
    folly::IPAddress address(collector.ip);
    auto id = address.toFullyQualified() + ':' +
        folly::to<std::string>(collector.port);
    auto origCollector = origCollectors->getNodeIf(id);
    shared_ptr<SflowCollector> newCollector;

    if (origCollector) {
      newCollector = updateSflowCollector(origCollector, &collector);
      ++numExistingProcessed;
    } else {
      newCollector = createSflowCollector(&collector);
    }
    changed |= updateMap(&newCollectors, origCollector, newCollector);
  }

  if (numExistingProcessed != origCollectors->size()) {
    // Some existing SflowCollectors were removed.
    CHECK_LT(numExistingProcessed, origCollectors->size());
    changed = true;
  }

  if (!changed) {
    return nullptr;
  }

  return origCollectors->clone(std::move(newCollectors));
}

shared_ptr<SflowCollector> ThriftConfigApplier::createSflowCollector(
    const cfg::SflowCollector* config) {
  return make_shared<SflowCollector>(config->ip, config->port);
}

shared_ptr<SflowCollector> ThriftConfigApplier::updateSflowCollector(
    const shared_ptr<SflowCollector>& orig,
    const cfg::SflowCollector* config) {
  auto newCollector = createSflowCollector(config);

  if (orig->getAddress() == newCollector->getAddress()) {
    return nullptr;
  }

  return newCollector;
}

shared_ptr<Interface> ThriftConfigApplier::updateInterface(
    const shared_ptr<Interface>& orig,
    const cfg::Interface* config,
    const Interface::Addresses& addrs) {
  CHECK_EQ(orig->getID(), config->intfID);

  cfg::NdpConfig ndp;
  if (config->__isset.ndp) {
    ndp = config->ndp_ref().value_unchecked();
  }
  auto name = getInterfaceName(config);
  auto mac = getInterfaceMac(config);
  auto mtu = config->__isset.mtu ? config->mtu_ref().value_unchecked()
                                 : Interface::kDefaultMtu;
  if (orig->getRouterID() == RouterID(config->routerID) &&
      orig->getVlanID() == VlanID(config->vlanID) && orig->getName() == name &&
      orig->getMac() == mac && orig->getAddresses() == addrs &&
      orig->getNdpConfig() == ndp && orig->getMtu() == mtu &&
      orig->isVirtual() == config->isVirtual &&
      orig->isStateSyncDisabled() == config->isStateSyncDisabled) {
    // No change
    return nullptr;
  }

  auto newIntf = orig->clone();
  newIntf->setRouterID(RouterID(config->routerID));
  newIntf->setVlanID(VlanID(config->vlanID));
  newIntf->setName(name);
  newIntf->setMac(mac);
  newIntf->setAddresses(addrs);
  newIntf->setNdpConfig(ndp);
  newIntf->setMtu(mtu);
  newIntf->setIsVirtual(config->isVirtual);
  newIntf->setIsStateSyncDisabled(config->isStateSyncDisabled);
  return newIntf;
}

shared_ptr<SwitchSettings> ThriftConfigApplier::updateSwitchSettings() {
  auto origSwitchSettings = orig_->getSwitchSettings();

  if (origSwitchSettings->getL2LearningMode() ==
      cfg_->switchSettings.l2LearningMode) {
    return nullptr;
  }

  auto newSwitchSettings = origSwitchSettings->clone();
  newSwitchSettings->setL2LearningMode(cfg_->switchSettings.l2LearningMode);
  return newSwitchSettings;
}

shared_ptr<ControlPlane> ThriftConfigApplier::updateControlPlane() {
  auto origCPU = orig_->getControlPlane();
  std::optional<std::string> qosPolicy;
  ControlPlane::RxReasonToQueue newRxReasonToQueue;
  bool rxReasonToQueueUnchanged = true;
  if (auto cpuTrafficPolicy = cfg_->cpuTrafficPolicy_ref()) {
    if (auto trafficPolicy = cpuTrafficPolicy->trafficPolicy_ref()) {
      if (auto defaultQosPolicy = trafficPolicy->defaultQosPolicy_ref()) {
        qosPolicy = *defaultQosPolicy;
      }
    }
    if (auto rxReasonToQueue = cpuTrafficPolicy->rxReasonToCPUQueue_ref()) {
      for (auto rxEntry : *rxReasonToQueue) {
        newRxReasonToQueue.emplace(rxEntry);
      }
      if (newRxReasonToQueue != origCPU->getRxReasonToQueue()) {
        rxReasonToQueueUnchanged = false;
      }
    }
  } else {
    /*
     * if cpuTrafficPolicy is not configured default to dataPlaneTrafficPolicy
     * default i.e. with regards to QoS map configuration, treat CPU port like
     * any front panel port.
     */
    if (auto dataPlaneTrafficPolicy = cfg_->dataPlaneTrafficPolicy_ref()) {
      if (auto defaultDataPlaneQosPolicy =
              dataPlaneTrafficPolicy->defaultQosPolicy_ref()) {
        qosPolicy = *defaultDataPlaneQosPolicy;
      }
    }
  }

  bool qosPolicyUnchanged = qosPolicy == origCPU->getQosPolicy();

  std::optional<cfg::QosMap> qosMap;
  if (qosPolicy) {
    for (auto policy : cfg_->qosPolicies) {
      if (policy.name == qosPolicy.value() && policy.qosMap_ref()) {
        qosMap = policy.qosMap_ref().value();
        break;
      }
    }
  }

  // check whether queue setting changed
  auto newQueues =
      updatePortQueues(origCPU->getQueues(), cfg_->cpuQueues, qosMap);
  bool queuesUnchanged = newQueues.size() == origCPU->getQueues().size();
  for (int i = 0; i < newQueues.size() && queuesUnchanged; i++) {
    if (*(newQueues.at(i)) != *(origCPU->getQueues().at(i))) {
      queuesUnchanged = false;
      break;
    }
  }

  if (queuesUnchanged && qosPolicyUnchanged && rxReasonToQueueUnchanged) {
    return nullptr;
  }

  auto newCPU = origCPU->clone();
  newCPU->resetQueues(newQueues);
  newCPU->resetQosPolicy(qosPolicy);
  newCPU->resetRxReasonToQueue(newRxReasonToQueue);
  return newCPU;
}

std::string ThriftConfigApplier::getInterfaceName(
    const cfg::Interface* config) {
  if (config->__isset.name) {
    return config->name_ref().value_unchecked();
  }
  return folly::to<std::string>("Interface ", config->intfID);
}

MacAddress ThriftConfigApplier::getInterfaceMac(const cfg::Interface* config) {
  if (config->__isset.mac) {
    return MacAddress(config->mac_ref().value_unchecked());
  }
  return platform_->getLocalMac();
}

Interface::Addresses ThriftConfigApplier::getInterfaceAddresses(
    const cfg::Interface* config) {
  Interface::Addresses addrs;

  // Assign auto-generate v6 link-local address to interface. Config can
  // have more link-local addresses if needed.
  folly::MacAddress macAddr;
  if (config->__isset.mac) {
    macAddr = folly::MacAddress(config->mac_ref().value_unchecked());
  } else {
    macAddr = platform_->getLocalMac();
  }
  const folly::IPAddressV6 v6llAddr(folly::IPAddressV6::LINK_LOCAL, macAddr);
  addrs.emplace(v6llAddr, kV6LinkLocalAddrMask);

  // Add all interface addresses from config
  for (const auto& addr : config->ipAddresses) {
    auto intfAddr = IPAddress::createNetwork(addr, -1, false);
    auto ret = addrs.insert(intfAddr);
    if (!ret.second) {
      throw FbossError(
          "Duplicate network IP address ",
          addr,
          " in interface ",
          config->intfID);
    }

    // NOTE: We do not want to leak link-local address into intfRouteTables_
    // TODO: For now we are allowing v4 LLs to be programmed because they are
    // used within Galaxy for LL routing. This hack should go away once we
    // move BGP sessions over non LL addresses
    if (intfAddr.first.isV6() and intfAddr.first.isLinkLocal()) {
      continue;
    }
    auto ret2 = intfRouteTables_[RouterID(config->routerID)].emplace(
        IPAddress::createNetwork(addr),
        std::make_pair(InterfaceID(config->intfID), intfAddr.first));
    if (!ret2.second) {
      // we get same network, only allow it if that is from the same interface
      auto other = ret2.first->second.first;
      if (other != InterfaceID(config->intfID)) {
        throw FbossError(
            "Duplicate network address ",
            addr,
            " of interface ",
            config->intfID,
            " as interface ",
            other,
            " in VRF ",
            config->routerID);
      }
      // For consistency with interface routes as added by RouteUpdater,
      // use the last address we see rather than the first. Otherwise,
      // we see pointless route updates on syncFib()
      *ret2.first = std::make_pair(
          IPAddress::createNetwork(addr),
          std::make_pair(InterfaceID(config->intfID), intfAddr.first));
    }
  }

  return addrs;
}

std::shared_ptr<MirrorMap> ThriftConfigApplier::updateMirrors() {
  const auto& origMirrors = orig_->getMirrors();
  MirrorMap::NodeContainer newMirrors;
  bool changed = false;

  size_t numExistingProcessed = 0;
  for (const auto& mirrorCfg : cfg_->mirrors) {
    auto origMirror = origMirrors->getMirrorIf(mirrorCfg.name);
    std::shared_ptr<Mirror> newMirror;
    if (origMirror) {
      newMirror = updateMirror(origMirror, &mirrorCfg);
      ++numExistingProcessed;
    } else {
      newMirror = createMirror(&mirrorCfg);
    }
    changed |= updateMap(&newMirrors, origMirror, newMirror);
  }

  if (numExistingProcessed != origMirrors->size()) {
    // Some existing Mirrors were removed.
    CHECK_LT(numExistingProcessed, origMirrors->size());
    changed = true;
  }

  for (auto& port : *(new_->getPorts())) {
    auto portInMirror = port->getIngressMirror();
    auto portEgMirror = port->getEgressMirror();
    if (portInMirror.has_value() &&
        newMirrors.find(portInMirror.value()) == newMirrors.end()) {
      throw FbossError(
          "Mirror ", portInMirror.value(), " for port is not found");
    }
    if (portEgMirror.has_value() &&
        newMirrors.find(portEgMirror.value()) == newMirrors.end()) {
      throw FbossError(
          "Mirror ", portEgMirror.value(), " for port is not found");
    }
  }

  if (!changed) {
    return nullptr;
  }

  return origMirrors->clone(std::move(newMirrors));
}

std::shared_ptr<Mirror> ThriftConfigApplier::createMirror(
    const cfg::Mirror* mirrorConfig) {
  if (!mirrorConfig->destination.egressPort_ref() &&
      !mirrorConfig->destination.tunnel_ref()) {
    /*
     * At least one of the egress port or tunnel is needed.
     */
    throw FbossError(
        "Must provide either egressPort or tunnel with endpoint ip for mirror");
  }

  std::optional<PortID> mirrorEgressPort;
  std::optional<folly::IPAddress> destinationIp;
  std::optional<folly::IPAddress> srcIp;
  std::optional<TunnelUdpPorts> udpPorts;

  if (mirrorConfig->destination.__isset.egressPort) {
    std::shared_ptr<Port> mirrorToPort{nullptr};
    auto egressPort =
        mirrorConfig->destination.egressPort_ref().value_unchecked();
    switch (egressPort.getType()) {
      case cfg::MirrorEgressPort::Type::name:
        for (auto& port : *(new_->getPorts())) {
          if (port->getName() == egressPort.get_name()) {
            mirrorToPort = port;
            break;
          }
        }
        break;
      case cfg::MirrorEgressPort::Type::logicalID:
        mirrorToPort =
            new_->getPorts()->getPortIf(PortID(egressPort.get_logicalID()));
        break;
      case cfg::MirrorEgressPort::Type::__EMPTY__:
        throw FbossError(
            "Must set either name or logicalID for MirrorEgressPort");
    }
    if (mirrorToPort &&
        mirrorToPort->getIngressMirror() != mirrorConfig->name &&
        mirrorToPort->getEgressMirror() != mirrorConfig->name) {
      mirrorEgressPort = PortID(mirrorToPort->getID());
    } else {
      throw FbossError("Invalid port name or ID");
    }
  }

  if (mirrorConfig->destination.tunnel_ref()) {
    cfg::MirrorTunnel tunnel = mirrorConfig->destination.tunnel_ref().value();
    if (tunnel.sflowTunnel_ref()) {
      destinationIp = folly::IPAddress(tunnel.sflowTunnel_ref().value().ip);
      cfg::SflowTunnel sflowTunnel = tunnel.sflowTunnel_ref().value();
      if (!sflowTunnel.udpSrcPort_ref() || !sflowTunnel.udpDstPort_ref()) {
        throw FbossError(
            "Both UDP source and UDP destination ports must be provided for \
            sFlow tunneling.");
      }
      udpPorts = TunnelUdpPorts(
          sflowTunnel.udpSrcPort_ref().value(),
          sflowTunnel.udpDstPort_ref().value());
    } else if (tunnel.greTunnel_ref()) {
      destinationIp = folly::IPAddress(tunnel.greTunnel_ref().value().ip);
    }

    if (tunnel.srcIp_ref()) {
      srcIp = folly::IPAddress(tunnel.srcIp_ref().value());
    }
  }

  uint8_t dscpMark = mirrorConfig->get_dscp();
  bool truncate = mirrorConfig->get_truncate();

  auto mirror = make_shared<Mirror>(
      mirrorConfig->name,
      mirrorEgressPort,
      destinationIp,
      srcIp,
      udpPorts,
      dscpMark,
      truncate);
  return mirror;
}

std::shared_ptr<Mirror> ThriftConfigApplier::updateMirror(
    const std::shared_ptr<Mirror>& orig,
    const cfg::Mirror* mirrorConfig) {
  auto newMirror = createMirror(mirrorConfig);
  if (*newMirror == *orig) {
    return nullptr;
  }
  if (!orig->isResolved() || !newMirror->getDestinationIp().has_value()) {
    return newMirror;
  }
  if (newMirror->getDestinationIp() == orig->getDestinationIp() &&
      newMirror->getSrcIp() == orig->getSrcIp() &&
      newMirror->getDscp() == orig->getDscp() &&
      newMirror->getTruncate() == orig->getTruncate() &&
      (!newMirror->configHasEgressPort() ||
       newMirror->getEgressPort() == orig->getEgressPort())) {
    newMirror->setMirrorTunnel(orig->getMirrorTunnel().value());
    newMirror->setEgressPort(orig->getEgressPort().value());
  }
  return newMirror;
}

std::shared_ptr<ForwardingInformationBaseMap>
ThriftConfigApplier::updateForwardingInformationBaseContainers() {
  auto origForwardingInformationBaseMap = orig_->getFibs();
  ForwardingInformationBaseMap::NodeContainer newFibContainers;
  bool changed = false;

  std::size_t numExistingProcessed = 0;

  for (const auto& interfaceCfg : cfg_->interfaces) {
    RouterID vrf(interfaceCfg.routerID);
    if (newFibContainers.find(vrf) != newFibContainers.end()) {
      continue;
    }

    auto origFibContainer = orig_->getFibs()->getFibContainerIf(vrf);

    std::shared_ptr<ForwardingInformationBaseContainer> newFibContainer{
        nullptr};
    if (origFibContainer) {
      newFibContainer = origFibContainer;
      ++numExistingProcessed;
    } else {
      newFibContainer =
          std::make_shared<ForwardingInformationBaseContainer>(vrf);
    }

    changed |= updateMap(&newFibContainers, origFibContainer, newFibContainer);
  }

  if (numExistingProcessed != orig_->getFibs()->size()) {
    CHECK_LE(numExistingProcessed, orig_->getFibs()->size());
    changed = true;
  }

  if (!changed) {
    return nullptr;
  }

  return origForwardingInformationBaseMap->clone(newFibContainers);
}

shared_ptr<SwitchState> applyThriftConfig(
    const shared_ptr<SwitchState>& state,
    const cfg::SwitchConfig* config,
    const Platform* platform,
    rib::RoutingInformationBase* rib) {
  cfg::SwitchConfig emptyConfig;
  return ThriftConfigApplier(state, config, platform, rib).run();
}
} // namespace fboss
} // namespace facebook
