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

#include "fboss/agent/Platform.h"

namespace facebook {
namespace fboss {

class SimSwitch;

class SimPlatform : public Platform {
 public:
  SimPlatform(folly::MacAddress mac, uint32_t numPorts);
  ~SimPlatform() override;

  HwSwitch* getHwSwitch() const override;
  void onHwInitialized(SwSwitch* sw) override;
  void onInitialConfigApplied(SwSwitch* sw) override;
  void stop() override;

  std::unique_ptr<ThriftHandler> createHandler(SwSwitch* sw) override;

  folly::MacAddress getLocalMac() const override {
    return mac_;
  }
  std::string getVolatileStateDir() const override;
  std::string getPersistentStateDir() const override;
  TransceiverIdxThrift getPortMapping(PortID /* unused */) const override {
    return TransceiverIdxThrift();
  }
  PlatformPort* getPlatformPort(PortID /* unused */) const override {
    return nullptr;
  }

  HwAsic* getAsic() const override {
    throw std::runtime_error("getAsic not implemented for SimPlatform");
  }

  void initPorts() override;

 private:
  // Forbidden copy constructor and assignment operator
  SimPlatform(SimPlatform const&) = delete;
  SimPlatform& operator=(SimPlatform const&) = delete;

  void initImpl() override {}

  folly::MacAddress mac_;
  std::unique_ptr<SimSwitch> hw_;
};

} // namespace fboss
} // namespace facebook
