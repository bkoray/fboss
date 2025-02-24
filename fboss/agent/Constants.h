/*
 *  copyright (c) 2004-present, facebook, inc.
 *  all rights reserved.
 *
 *  this source code is licensed under the bsd-style license found in the
 *  license file in the root directory of this source tree. an additional grant
 *  of patent rights can be found in the patents file in the same directory.
 *
 */

#pragma once

#include <folly/Range.h>

namespace facebook {
namespace fboss {

inline constexpr folly::StringPiece kClassID{"classID"};
inline constexpr folly::StringPiece kEcmpEgressId{"ecmpEgressId"};
inline constexpr folly::StringPiece kEcmpEgress{"ecmpEgress"};
inline constexpr folly::StringPiece kEcmpHosts{"ecmpHosts"};
inline constexpr folly::StringPiece kEgressId{"egressId"};
inline constexpr folly::StringPiece kEgress{"egress"};
inline constexpr folly::StringPiece kEntries{"entries"};
inline constexpr folly::StringPiece kExtraFields{"extraFields"};
inline constexpr folly::StringPiece kFlags{"flags"};
inline constexpr folly::StringPiece kFwdInfo{"forwardingInfo"};
inline constexpr folly::StringPiece kHostTable{"hostTable"};
inline constexpr folly::StringPiece kHosts{"hosts"};
inline constexpr folly::StringPiece kHwSwitch{"hwSwitch"};
inline constexpr folly::StringPiece kIntfId{"intfId"};
inline constexpr folly::StringPiece kIntfTable{"intfTable"};
inline constexpr folly::StringPiece kIntf{"intf"};
inline constexpr folly::StringPiece kIp{"ip"};
inline constexpr folly::StringPiece kLabel{"label"};
inline constexpr folly::StringPiece kMac{"mac"};
inline constexpr folly::StringPiece kMplsNextHops{"mplsNextHops"};
inline constexpr folly::StringPiece kMplsTunnel{"mplsTunnel"};
inline constexpr folly::StringPiece kNextHopsMulti{"rib"};
inline constexpr folly::StringPiece kNextHops{"nexthops"};
inline constexpr folly::StringPiece kPaths{"paths"};
inline constexpr folly::StringPiece kPort{"port"};
inline constexpr folly::StringPiece kPrefix{"prefix"};
inline constexpr folly::StringPiece kRibV4{"ribV4"};
inline constexpr folly::StringPiece kRibV6{"ribV6"};
inline constexpr folly::StringPiece kRouterId{"routerId"};
inline constexpr folly::StringPiece kStack{"stack"};
inline constexpr folly::StringPiece kSwSwitch{"swSwitch"};
inline constexpr folly::StringPiece kVlan{"vlan"};
inline constexpr folly::StringPiece kVrf{"vrf"};
inline constexpr folly::StringPiece kWarmBootCache{"warmBootCache"};
inline constexpr folly::StringPiece kWeight{"weight"};

} // namespace fboss
} // namespace facebook
