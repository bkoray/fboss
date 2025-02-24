/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "FakeSaiSwitch.h"
#include "fboss/agent/hw/sai/fake/FakeSai.h"

#include <folly/logging/xlog.h>

using facebook::fboss::FakeSai;

namespace {
static constexpr uint64_t kDefaultVlanId = 0;
static constexpr uint64_t kDefaultVirtualRouterId = 0;
static constexpr uint64_t kCpuPort = 0;
static constexpr uint32_t kMaxPortUnicastQueues = 8;
static constexpr uint32_t kMaxPortMulticastQueues = 8;
static constexpr uint32_t kMaxPortQueues =
    kMaxPortUnicastQueues + kMaxPortMulticastQueues;
static constexpr uint32_t kMaxCpuQueues = 8;
static constexpr sai_object_id_t kEcmpHashId = 1234;
static constexpr sai_object_id_t kLagHashId = 1234;
} // namespace

sai_status_t set_switch_attribute_fn(
    sai_object_id_t switch_id,
    const sai_attribute_t* attr) {
  auto fs = FakeSai::getInstance();
  auto& sw = fs->swm.get(switch_id);
  sai_status_t res;
  if (!attr) {
    return SAI_STATUS_INVALID_PARAMETER;
  }
  res = SAI_STATUS_SUCCESS;
  switch (attr->id) {
    case SAI_SWITCH_ATTR_SRC_MAC_ADDRESS:
      sw.setSrcMac(attr->value.mac);
      break;
    case SAI_SWITCH_ATTR_INIT_SWITCH:
      sw.setInitStatus(attr->value.booldata);
      break;
    case SAI_SWITCH_ATTR_SWITCH_HARDWARE_INFO:
      sw.setHwInfo(std::vector<int8_t>(
          attr->value.s8list.list,
          attr->value.s8list.list + attr->value.s8list.count));
      break;
    case SAI_SWITCH_ATTR_SWITCH_SHELL_ENABLE:
      sw.setShellStatus(attr->value.booldata);
      break;
    case SAI_SWITCH_ATTR_ECMP_DEFAULT_HASH_SEED:
      sw.setEcmpSeed(attr->value.u32);
      break;
    case SAI_SWITCH_ATTR_LAG_DEFAULT_HASH_SEED:
      sw.setLagSeed(attr->value.u32);
      break;
    case SAI_SWITCH_ATTR_ECMP_DEFAULT_HASH_ALGORITHM:
      sw.setEcmpAlgorithm(attr->value.s32);
      break;
    case SAI_SWITCH_ATTR_LAG_DEFAULT_HASH_ALGORITHM:
      sw.setLagAlgorithm(attr->value.s32);
      break;
    default:
      res = SAI_STATUS_INVALID_PARAMETER;
      break;
  }
  return res;
}

sai_status_t create_switch_fn(
    sai_object_id_t* switch_id,
    uint32_t attr_count,
    const sai_attribute_t* attr_list) {
  auto fs = FakeSai::getInstance();
  *switch_id = fs->swm.create();
  for (int i = 0; i < attr_count; ++i) {
    set_switch_attribute_fn(*switch_id, &attr_list[i]);
  }
  return SAI_STATUS_SUCCESS;
}

sai_status_t remove_switch_fn(sai_object_id_t switch_id) {
  auto fs = FakeSai::getInstance();
  fs->swm.remove(switch_id);
  return SAI_STATUS_SUCCESS;
}

sai_status_t get_switch_attribute_fn(
    sai_object_id_t switch_id,
    uint32_t attr_count,
    sai_attribute_t* attr) {
  auto fs = FakeSai::getInstance();
  auto& sw = fs->swm.get(switch_id);
  for (int i = 0; i < attr_count; ++i) {
    switch (attr[i].id) {
      case SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID:
        attr[i].value.oid = kDefaultVirtualRouterId;
        break;
      case SAI_SWITCH_ATTR_DEFAULT_VLAN_ID:
        attr[i].value.oid = kDefaultVlanId;
        break;
      case SAI_SWITCH_ATTR_CPU_PORT:
        attr[i].value.oid = fs->getCpuPort();
        break;
      case SAI_SWITCH_ATTR_PORT_NUMBER:
        attr[i].value.u32 = fs->pm.map().size();
        break;
      case SAI_SWITCH_ATTR_PORT_LIST: {
        if (fs->pm.map().size() > attr[i].value.objlist.count) {
          attr[i].value.objlist.count = fs->pm.map().size();
          return SAI_STATUS_BUFFER_OVERFLOW;
        }
        attr[i].value.objlist.count = fs->pm.map().size();
        int j = 0;
        for (const auto& p : fs->pm.map()) {
          attr[i].value.objlist.list[j++] = p.first;
        }
      } break;
      case SAI_SWITCH_ATTR_SRC_MAC_ADDRESS:
        std::copy_n(sw.srcMac().bytes(), 6, std::begin(attr[i].value.mac));
        break;
      case SAI_SWITCH_ATTR_INIT_SWITCH:
        attr[i].value.booldata = sw.isInitialized();
        break;
      case SAI_SWITCH_ATTR_SWITCH_HARDWARE_INFO:
        attr[i].value.s8list.count = sw.hwInfo().size();
        attr[i].value.s8list.list = sw.hwInfoData();
        break;
      case SAI_SWITCH_ATTR_SWITCH_SHELL_ENABLE:
        attr[i].value.booldata = sw.isShellEnabled();
        break;
      case SAI_SWITCH_ATTR_NUMBER_OF_UNICAST_QUEUES:
        attr[i].value.u32 = kMaxPortUnicastQueues;
        break;
      case SAI_SWITCH_ATTR_NUMBER_OF_MULTICAST_QUEUES:
        attr[i].value.u32 = kMaxPortMulticastQueues;
        break;
      case SAI_SWITCH_ATTR_NUMBER_OF_QUEUES:
        attr[i].value.u32 = kMaxPortQueues;
        break;
      case SAI_SWITCH_ATTR_NUMBER_OF_CPU_QUEUES:
        attr[i].value.u32 = kMaxCpuQueues;
        break;
      case SAI_SWITCH_ATTR_ECMP_HASH:
        attr[i].value.oid = kEcmpHashId;
        break;
      case SAI_SWITCH_ATTR_LAG_HASH:
        attr[i].value.oid = kLagHashId;
        break;
      case SAI_SWITCH_ATTR_ECMP_DEFAULT_HASH_SEED:
        attr[i].value.u32 = sw.ecmpSeed();
        break;
      case SAI_SWITCH_ATTR_LAG_DEFAULT_HASH_SEED:
        attr[i].value.u32 = sw.lagSeed();
        break;
      case SAI_SWITCH_ATTR_ECMP_DEFAULT_HASH_ALGORITHM:
        attr[i].value.s32 = sw.ecmpAlgorithm();
        break;
      case SAI_SWITCH_ATTR_LAG_DEFAULT_HASH_ALGORITHM:
        attr[i].value.s32 = sw.lagAlgorithm();
        break;
      default:
        return SAI_STATUS_INVALID_PARAMETER;
    }
  }
  return SAI_STATUS_SUCCESS;
}

namespace facebook::fboss {

static sai_switch_api_t _switch_api;

void populate_switch_api(sai_switch_api_t** switch_api) {
  _switch_api.create_switch = &create_switch_fn;
  _switch_api.remove_switch = &remove_switch_fn;
  _switch_api.set_switch_attribute = &set_switch_attribute_fn;
  _switch_api.get_switch_attribute = &get_switch_attribute_fn;
  *switch_api = &_switch_api;
}

} // namespace facebook::fboss
