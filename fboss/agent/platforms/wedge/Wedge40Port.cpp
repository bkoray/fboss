/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "fboss/agent/platforms/wedge/Wedge40Port.h"

#include "fboss/agent/platforms/wedge/Wedge40Platform.h"

namespace facebook {
namespace fboss {
Wedge40Port::Wedge40Port(
    PortID id,
    Wedge40Platform* platform,
    std::optional<FrontPanelResources> frontPanel)
    : WedgePort(id, platform, frontPanel) {}
} // namespace fboss
} // namespace facebook
