/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GFX_VR_SERVICE_OPENVRCOSMOSMAPPER_H
#define GFX_VR_SERVICE_OPENVRCOSMOSMAPPER_H

#include "OpenVRControllerMapper.h"

namespace mozilla {
namespace gfx {

class OpenVRCosmosMapper : public OpenVRControllerMapper {
 public:
  OpenVRCosmosMapper() = default;
  virtual ~OpenVRCosmosMapper() = default;
  virtual void UpdateButtons(VRControllerState& aControllerState,
                             ControllerInfo& aControllerInfo);
  // Exit Present via bumper button (i.e., R1/L1)
  virtual uint32_t GetExitPresentButtonMask() override { return 1ULL << 6; }
};

}  // namespace gfx
}  // namespace mozilla

#endif  // GFX_VR_SERVICE_OPENVRCOSMOSMAPPER_H