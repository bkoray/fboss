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

#include "fboss/agent/state/MacEntry.h"
#include "fboss/agent/state/NodeMap.h"
#include "fboss/agent/state/NodeMapDelta.h"
#include "fboss/agent/state/Vlan.h"
#include "fboss/agent/types.h"

#include <folly/MacAddress.h>

namespace facebook {
namespace fboss {

using MacTableTraits = NodeMapTraits<folly::MacAddress, MacEntry>;

class MacTable : public NodeMapT<MacTable, MacTableTraits> {
 public:
  MacTable();
  ~MacTable() override;

  std::shared_ptr<MacEntry> getMacIf(const folly::MacAddress& mac) const {
    return getNodeIf(mac);
  }

  MacTable* modify(Vlan** vlan, std::shared_ptr<SwitchState>* state);
  MacTable* modify(VlanID vlanID, std::shared_ptr<SwitchState>* state);

  void addEntry(const std::shared_ptr<MacEntry>& macEntry) {
    CHECK(!isPublished());
    addNode(macEntry);
  }

  void removeEntry(const folly::MacAddress& mac) {
    CHECK(!isPublished());
    removeNode(mac);
  }

  void updateEntry(
      folly::MacAddress mac,
      PortDescriptor portDescr,
      std::optional<cfg::AclLookupClass> classID) {
    CHECK(!this->isPublished());
    auto& nodes = this->writableNodes();
    auto it = nodes.find(mac);
    if (it == nodes.end()) {
      throw FbossError("Mac entry for ", mac.toString(), " does not exist");
    }
    auto entry = it->second->clone();

    entry->setMac(mac);
    entry->setPort(portDescr);
    entry->setClassID(classID);
    it->second = entry;
  }

 private:
  // Inherit the constructors required for clone()
  using NodeMapT::NodeMapT;
  friend class CloneAllocator;
};

using MacTableDelta = NodeMapDelta<MacTable>;

} // namespace fboss
} // namespace facebook
