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

#include <map>
#include <string>
#include "fboss/agent/Platform.h"

extern "C" {
#include <opennsl/types.h>
}

namespace facebook {
namespace fboss {

class BcmPlatformPort;
class BcmWarmBootHelper;
class PortQueue;

/*
 * BcmPlatform specifies additional APIs that must be provided by platforms
 * based on Broadcom chips.
 */
class BcmPlatform : public Platform {
 public:
  typedef std::map<opennsl_port_t, BcmPlatformPort*> BcmPlatformPortMap;

  using Platform::Platform;

  /*
   * onUnitCreate() will be called by the BcmSwitch code immediately after
   * creating up switch unit. This is distinct from actually attaching
   * to the unit/ASIC, which happens in a separate step.
   */
  virtual void onUnitCreate(int unit) = 0;

  /*
   * onUnitAttach() will be called by the BcmSwitch code immediately after
   * attaching to the switch unit.
   */
  virtual void onUnitAttach(int unit) = 0;

  /*
   * The BcmPlatform should return a map of BCM port ID to BcmPlatformPort
   * objects.  The BcmPlatform object will retain ownership of all the
   * BcmPlatformPort objects, and must ensure that they remain valid for as
   * long as the BcmSwitch exists.
   */
  virtual BcmPlatformPortMap getPlatformPortMap() = 0;

  /*
   * Get filename for where we dump the HW config that
   * the switch was initialized with.
   */
  virtual std::string getHwConfigDumpFile() const;

  /*
   * Based on the chip we may or may not be able to
   * use the host table for host routes (/128 or /32).
   */
  virtual bool canUseHostTableForHostRoutes() const = 0;
  TransceiverIdxThrift getPortMapping(PortID portId) const override = 0;

  /*
   * Get total device buffer in bytes
   */
  virtual uint32_t getMMUBufferBytes() const = 0;
  /*
   * MMU buffer is split into cells, each of which is
   * X bytes. Cells then serve as units of for allocation
   * and accounting of MMU resources.
   */
  virtual uint32_t getMMUCellBytes() const = 0;

  virtual const PortQueue& getDefaultPortQueueSettings(
      cfg::StreamType streamType) const = 0;

  virtual const PortQueue& getDefaultControlPlaneQueueSettings(
      cfg::StreamType streamType) const = 0;

  virtual BcmWarmBootHelper* getWarmBootHelper() = 0;

  virtual bool isBcmShellSupported() const;

  virtual bool isCosSupported() const = 0;

  virtual bool v6MirrorTunnelSupported() const = 0;

  virtual bool sflowSamplingSupported() const = 0;

  virtual bool mirrorPktTruncationSupported() const = 0;

  virtual bool useQueueGportForCos() const = 0;
  virtual uint32_t maxLabelStackDepth() const = 0;

  virtual bool isMultiPathLabelSwitchActionSupported() const = 0;

 protected:
  /*
   * Dump map containing switch h/w config as a key, value pair
   * to a file. We dump in format that name, value pair format that
   * the SDK can read. Later this map is used to initialize the chip
   */
  void dumpHwConfig() const;

 private:
  // Forbidden copy constructor and assignment operator
  BcmPlatform(BcmPlatform const&) = delete;
  BcmPlatform& operator=(BcmPlatform const&) = delete;
};

} // namespace fboss
} // namespace facebook
