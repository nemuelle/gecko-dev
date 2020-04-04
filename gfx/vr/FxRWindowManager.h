/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#pragma once
#include <cstdint>
#include <vector>
#include "mozilla/Atomics.h"
#include "windows.h"
#include "openvr.h"

class nsPIDOMWindowOuter;
class nsWindow;
class nsIWidget;
namespace vr {
  class IVRSystem;
};

typedef std::vector<vr::VREvent_t> VREventVector;

// FxRWindowManager is a singleton that is responsible for tracking all of
// the top-level windows created for Firefox Reality on Desktop. Only a
// single window is initially supported.
class FxRWindowManager final {
 public:
  static FxRWindowManager* GetInstance();
  ~FxRWindowManager();

  bool VRinit();
  bool CreateOverlayForWindow();
  void SetRenderPid(uint64_t aOverlayId, uint32_t aPid);
  uint64_t GetOverlayId() const;  

  bool AddWindow(nsPIDOMWindowOuter* aWindow);
  void RemoveWindow(uint64_t aOverlayId);

  bool IsFxRWindow(uint64_t aOuterWindowID);
  bool IsFxRWindow(const nsWindow* aWindow) const;
  uint64_t GetWindowID() const;

  void ShowVirtualKeyboard(uint64_t aOverlayId);
  void HideVirtualKeyboard();

  void OnWebXRPresentationChange(uint64_t aOuterWindowID, bool isPresenting);

  void ProcessOverlayEvents();
 private:
  vr::VROverlayError SetupOverlayInput(vr::VROverlayHandle_t overlayId);
  static DWORD OverlayInputPump(_In_ LPVOID lpParameter);
  void CollectOverlayEvents();

  FxRWindowManager();

  vr::IVRSystem * mVrApp;
  int32_t mDxgiAdapterIndex;

  mozilla::Atomic<bool> mIsOverlayPumpActive;
  HANDLE mOverlayPumpThread;

  // Only a single window is supported for tracking. Support for multiple
  // windows will require a data structure to collect windows as they are
  // created.
  struct FxRWindow{
    // Note: mWidget takes a full reference
    nsIWidget* mWidget;
    nsPIDOMWindowOuter* mWindow;
    vr::VROverlayHandle_t mOverlayHandle;
    vr::VROverlayHandle_t mOverlayThumbnailHandle;
    HWND mHwndWidget;

    // Works with Collect/ProcessOverlayEvents to transfer OpenVR input events
    // from the background thread to the main thread. Consider using a queue?
    VREventVector mEventsVector;
    CRITICAL_SECTION mEventsCritSec;

    // OpenVR scroll event doesn't provide the position of the controller on the
    // overlay, so keep track of the last-known position to use with the scroll
    // event
    POINT mLastMousePt;
    RECT mOverlaySizeRec;
  } mFxRWindow;
  //nsPIDOMWindowOuter* mWindow;
};