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
#include <opennsl/types.h>
}

#include "fboss/agent/Utils.h"
#include "fboss/agent/hw/bcm/BcmPort.h"
#include "fboss/agent/types.h"

#include <boost/container/flat_map.hpp>
#include <mutex>

namespace facebook {
namespace fboss {

class BcmSwitch;
class BcmPortGroup;

class BcmPortTable {
 public:
  explicit BcmPortTable(BcmSwitch* hw);
  ~BcmPortTable();

  typedef boost::container::flat_map<PortID, BcmPort*> FbossPortMap;
  using FilterIterator = MapFilter<PortID, BcmPort*>;
  using Filter = MapFilter<PortID, BcmPort*>::Predicate;
  using FilterEntry = MapFilter<PortID, BcmPort*>::Entry;
  using FilterAction = std::function<void(const FilterEntry&)>;
  /*
   * Initialize the port table from the list of physical switch ports.
   *
   * No other BcmPortTable methods should be accessed before initPorts()
   * completes.
   */
  void initPorts(const opennsl_port_config_t* portConfig, bool warmBoot);

  /*
   * Getters.
   */
  opennsl_port_t getBcmPortId(PortID id) const {
    return static_cast<opennsl_port_t>(id);
  }
  PortID getPortId(opennsl_port_t port) const {
    return PortID(port);
  }

  // throw an error if not found
  BcmPort* getBcmPort(PortID id) const;
  BcmPort* getBcmPort(opennsl_port_t id) const;

  // return nullptr if not found
  BcmPort* getBcmPortIf(PortID id) const;
  BcmPort* getBcmPortIf(opennsl_port_t id) const;

  FbossPortMap::const_iterator begin() const {
    return fbossPhysicalPorts_.begin();
  }

  FbossPortMap::const_iterator end() const {
    return fbossPhysicalPorts_.end();
  }

  /*
   * Update all ports' statistics.
   */
  void updatePortStats();

  bool portExists(PortID port) const {
    return getBcmPortIf(port) != nullptr;
  }
  bool portExists(opennsl_port_t port) const {
    return getBcmPortIf(port) != nullptr;
  }
  void preparePortsForGracefulExit() {
    for (auto& portIdAndBcmPort : bcmPhysicalPorts_) {
      portIdAndBcmPort.second->prepareForGracefulExit();
    }
  }

  /*
   * for every map entry which meets given predicate, execute given action */
  void forFilteredEach(Filter predicate, FilterAction action) const;

  /*
   * For some platform that supports add or remove port after bcm unit init.
   * We need to support BcmPortTable to add and remove such BcmPort objects so
   * that BcmPortTable will still maintain all manageable bcm port objects.
   */
  void addBcmPort(opennsl_port_t logicalPort, bool warmBoot);

 private:
  /* Initialize all the port groups that exist. A port group is a set of ports
   * that can act as either a single port or multiple ports.
   */
  void initPortGroups();

  void initPortGroupLegacy(BcmPort* controllingPort);

  void initPortGroupFromConfig(
      BcmPort* controllingPort,
      const std::map<PortID, std::vector<PortID>>& subsidiaryPortsMap);

  typedef boost::container::flat_map<opennsl_port_t, std::unique_ptr<BcmPort>>
      BcmPortMap;

  typedef std::vector<std::unique_ptr<BcmPortGroup>> BcmPortGroupList;

  BcmSwitch* hw_{nullptr};

  // Mappings for the physical ports.
  // The set of physical ports is defined in initPorts(), and is read-only
  // afterwards (since the physical ports on the switch cannot change).
  // Therefore we don't need any locking on this data structure.
  // (Modifiable data in the BcmPort objects themselves does require locking,
  // though.)

  // A mapping from opennsl_port_t to BcmPort.
  BcmPortMap bcmPhysicalPorts_;
  // A mapping from FBOSS PortID to BcmPort.
  FbossPortMap fbossPhysicalPorts_;

  // A list of all the port groups. We can change this data structure to be
  // two maps (like the portmaps) if we ever have the need to access these
  // outside of the BcmPort objects. This is mainly here to keep a simple
  // ownership model for the port group objects
  BcmPortGroupList bcmPortGroups_;
};

} // namespace fboss
} // namespace facebook
