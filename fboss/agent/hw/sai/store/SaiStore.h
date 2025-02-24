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

#include "fboss/agent/hw/sai/api/SaiApiTable.h"
#include "fboss/agent/hw/sai/api/SaiObjectApi.h"
#include "fboss/agent/hw/sai/api/Traits.h"
#include "fboss/agent/hw/sai/store/SaiObject.h"
#include "fboss/agent/hw/sai/store/SaiObjectWithCounters.h"
#include "fboss/lib/RefMap.h"

#include <memory>
#include <optional>

extern "C" {
#include <sai.h>
}

namespace facebook::fboss {

namespace detail {

/*
 * SaiObjectStore is the critical component of SaiStore,
 * it provides the needed operations on a single type of SaiObject
 * e.g. Port, Vlan, Route, etc... SaiStore is largely just a collection
 * of the SaiObjectStores.
 */
template <typename SaiObjectTraits>
class SaiObjectStore {
 public:
  using ObjectType = typename std::conditional<
      SaiObjectHasStats<SaiObjectTraits>::value,
      SaiObjectWithCounters<SaiObjectTraits>,
      SaiObject<SaiObjectTraits>>::type;

  explicit SaiObjectStore(sai_object_id_t switchId) : switchId_(switchId) {}
  SaiObjectStore() {}
  ~SaiObjectStore() {
    for (auto& obj : warmBootHandles_) {
      obj->release();
    }
  }

  void setSwitchId(sai_object_id_t switchId) {
    switchId_ = switchId;
  }

  /*
   * This routine will help load sai objects owned by the SAI Adapter.
   * For instance, sai queue objects are owned by the adapter and will not be
   * loaded during the initial reload. When a port is created, the queues will
   * be created by the SDK and the adapter keys for the queue can be retrieved.
   * Using the adapter key, the sai store can be populated with its attributes.
   */
  std::shared_ptr<ObjectType> loadObjectOwnedByAdapter(
      const typename SaiObjectTraits::AdapterKey& adapterKey) {
    static_assert(
        IsSaiObjectOwnedByAdapter<SaiObjectTraits>::value,
        "Only adapter owned SAI objects can be loaded");
    ObjectType obj(adapterKey);
    auto adapterHostKey = obj.adapterHostKey();
    auto ins = objects_.refOrEmplace(adapterHostKey, std::move(obj));
    return ins.first;
  }

  void reload() {
    if (!switchId_) {
      XLOG(FATAL)
          << "Attempted to reload() on a SaiObjectStore without a switchId";
    }
    auto keys = getObjectKeys<SaiObjectTraits>(switchId_.value());
    for (const auto k : keys) {
      ObjectType obj(k);
      auto adapterHostKey = obj.adapterHostKey();
      auto ins = objects_.refOrEmplace(adapterHostKey, std::move(obj));
      if (!ins.second) {
        XLOG(FATAL) << "[" << saiObjectTypeToString(SaiObjectTraits::ObjectType)
                    << "]"
                    << " Unexpected duplicate adapterHostKey";
      }
      warmBootHandles_.push_back(ins.first);
    }
  }

  std::shared_ptr<ObjectType> setObject(
      const typename SaiObjectTraits::AdapterHostKey& adapterHostKey,
      const typename SaiObjectTraits::CreateAttributes& attributes) {
    auto ins = objects_.refOrEmplace(
        adapterHostKey, adapterHostKey, attributes, switchId_.value());
    if (!ins.second) {
      ins.first->setAttributes(attributes);
    }
    XLOG(DBG5) << "[" << saiObjectTypeToString(SaiObjectTraits::ObjectType)
               << "["
               << "set object";
    return ins.first;
  }

  std::shared_ptr<ObjectType> get(
      const typename SaiObjectTraits::AdapterHostKey& adapterHostKey) {
    XLOG(DBG5) << "[" << saiObjectTypeToString(SaiObjectTraits::ObjectType)
               << "["
               << "get object";
    return objects_.ref(adapterHostKey);
  }

  void release() {
    objects_.clear();
  }

 private:
  std::optional<sai_object_id_t> switchId_;
  UnorderedRefMap<typename SaiObjectTraits::AdapterHostKey, ObjectType>
      objects_;
  std::vector<std::shared_ptr<ObjectType>> warmBootHandles_;
};

} // namespace detail

/*
 * SaiStore represents FBOSS's knowledge of objects and their attributes
 * that have been programmed via SAI.
 *
 */
class SaiStore {
 public:
  // Static function for getting the SaiStore folly::Singleton
  static std::shared_ptr<SaiStore> getInstance();

  SaiStore();
  explicit SaiStore(sai_object_id_t switchId);

  /*
   * Set the switch id on all the SaiObjectStores
   * Useful for the singleton mode of operation, which is constructed
   * with the default constructor, then after the switch_id is ready, that
   * is set on the SaiStore
   */
  void setSwitchId(sai_object_id_t switchId);

  /*
   * Reload the SaiStore from the current SAI state via SAI api calls.
   */
  void reload();

  /*
   *
   */
  void release();

  template <typename SaiObjectTraits>
  detail::SaiObjectStore<SaiObjectTraits>& get() {
    return std::get<detail::SaiObjectStore<SaiObjectTraits>>(stores_);
  }

 private:
  sai_object_id_t switchId_{};
  std::tuple<
      detail::SaiObjectStore<SaiBridgeTraits>,
      detail::SaiObjectStore<SaiBridgePortTraits>,
      detail::SaiObjectStore<SaiPortTraits>,
      detail::SaiObjectStore<SaiVlanTraits>,
      detail::SaiObjectStore<SaiVlanMemberTraits>,
      detail::SaiObjectStore<SaiRouteTraits>,
      detail::SaiObjectStore<SaiRouterInterfaceTraits>,
      detail::SaiObjectStore<SaiNeighborTraits>,
      detail::SaiObjectStore<SaiFdbTraits>,
      detail::SaiObjectStore<SaiVirtualRouterTraits>,
      detail::SaiObjectStore<SaiNextHopTraits>,
      detail::SaiObjectStore<SaiNextHopGroupTraits>,
      detail::SaiObjectStore<SaiNextHopGroupMemberTraits>,
      detail::SaiObjectStore<SaiHostifTrapGroupTraits>,
      detail::SaiObjectStore<SaiHostifTrapTraits>,
      detail::SaiObjectStore<SaiQueueTraits>,
      detail::SaiObjectStore<SaiSchedulerTraits>>
      stores_;
};

} // namespace facebook::fboss
