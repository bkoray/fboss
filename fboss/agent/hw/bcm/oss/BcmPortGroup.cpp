/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "fboss/agent/hw/bcm/BcmPortGroup.h"

namespace facebook {
namespace fboss {

// stubbed out
int BcmPortGroup::retrieveActiveLanes() const {
  return 0;
}

void BcmPortGroup::setActiveLanes(
    const std::vector<std::shared_ptr<Port>>& /*ports*/,
    LaneMode /*desiredLaneMode*/) {}
} // namespace fboss
} // namespace facebook
