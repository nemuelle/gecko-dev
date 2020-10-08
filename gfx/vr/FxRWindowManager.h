/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#pragma once
#include <cstdint>
#include <vector>
#include "mozilla/Atomics.h"
#include "nsAString.h"
#include "windows.h"
#include "openvr.h"

class nsPIDOMWindowOuter;
class nsWindow;
class nsIWidget;

typedef std::vector<vr::VREvent_t> VREventVector;

enum FxRProjectionMode {
  VIDEO_PROJECTION_2D = 0,  // 2D
  VIDEO_PROJECTION_360 =
      1,  // 360 mono (VROverlayFlags::VROverlayFlags_Panorama)
  VIDEO_PROJECTION_360S =
      2,  // 360 stereo (VROverlayFlags::VROverlayFlags_StereoPanorama)
  VIDEO_PROJECTION_180 = 3,  // 180 mono (No equivalent OpenVR VROverlayFlag)
  VIDEO_PROJECTION_180LR =
      4,  // 180 left to right (No equivalent OpenVR VROverlayFlag)
  VIDEO_PROJECTION_180TB =
      5,  // 180 top to bottom (No equivalent OpenVR VROverlayFlag)
  VIDEO_PROJECTION_3D =
      6  // 3D side by side (VROverlayFlags::VROverlayFlags_SideBySide_Parallel)
};

//
// FxRWindowManager is a singleton that is responsible for tracking all of
// the top-level windows created for Firefox Reality on Desktop. Only a
// single window is initially supported.
class FxRWindowManager final {
 private:
  // TODO: make this a class, and move methods from the manager that only
  // interact with struct members to this class (i.e., some of the static
  // functions that take an FxRWindow
  struct FxRWindow {
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
    
    // When true, indicates that VREvents should be used to move the overlay
    mozilla::Atomic<bool> mIsMoving;

    // OpenVR scroll event doesn't provide the position of the controller on the
    // overlay, so keep track of the last-known position to use with the scroll
    // event
    POINT mLastMousePt;
    RECT mOverlaySizeRec;

    float mOverlayWidth;
    vr::HmdMatrix34_t mOverlayPosition;

    FxRWindow() {
      Reset();
    }

    void Reset() {
      mWidget = nullptr;
      mWindow = nullptr;
      mOverlayHandle = 0;
      mOverlayThumbnailHandle = 0;

      mEventsCritSec = { 0 };
      mIsMoving = false;

      mLastMousePt = { 0 };
      mOverlaySizeRec = { 0 };

      mOverlayWidth = 0;
      mOverlayPosition = {{{0},{0},{0}}};
    }
  };

 public:
  static FxRWindowManager* GetInstance();
  static bool HasInstance();
  static bool TryFocusExistingInstance();
  ~FxRWindowManager();

  bool VRinit();
  bool CreateOverlayForWindow();
  vr::VROverlayError CreateTransportControlsOverlay();
  void SetRenderPid(uint64_t aOverlayId, uint32_t aPid);
  uint64_t GetOverlayId() const;
  int32_t GetDxgiAdapterIndex() const { return mDxgiAdapterIndex; }

  bool AddWindow(nsPIDOMWindowOuter* aWindow);
  void RemoveWindow(uint64_t aOverlayId);
  bool IsFxRWindow(uint64_t aOuterWindowID);
  bool IsFxRWindow(const nsWindow* aWindow) const;

  void SetOverlayScale(uint64_t aOuterWindowID, float aScale);
  void SetOverlayMoveMode(uint64_t aOuterWindowID, bool aEnable);

  void ProcessOverlayEvents(nsWindow* window);
  void ShowVirtualKeyboard(uint64_t aOverlayId);
  void ShowVirtualKeyboardForWindow(uint64_t aOuterWindowID);
  void HideVirtualKeyboard();

  void OnWebXRPresentationChange(uint64_t aOuterWindowID, bool isPresenting);
  void OnFullScreenChange(uint64_t aOuterWindowID, bool aIsFullScreen);
  void ToggleOverlayInteractivity(uint64_t aOuterWindowID);

  void SetPlayMediaState(const nsAString& aState);
  void SetProjectionMode(const nsAString& aMode);

 private:
  FxRWindowManager();

  void MakeOverlayInteractive(FxRWindow& fxrWindow, bool aInteractive);

  static void InitWindow(FxRWindow& newWindow, nsPIDOMWindowOuter* aWindow);
  static void CleanupWindow(FxRWindow& fxrWindow);
  FxRWindow& GetFxrWindowFromWidget(nsIWidget* widget);
  bool CreateOverlayForWindow(FxRWindow& newWindow, const char* name,
                              float width);

  vr::VROverlayError SetupOverlayInput(vr::VROverlayHandle_t overlayId);
  static DWORD WINAPI OverlayInputPump(LPVOID lpParameter);
  void CollectOverlayEvents(FxRWindow& fxrWindow);

  void HandleMouseEvent(FxRWindow& fxrWindow, nsWindow* window,
                        vr::VREvent_Mouse_t& data, uint32_t eventType);
  void HandleScrollEvent(FxRWindow& fxrWindow, nsWindow* window,
                         vr::VREvent_Scroll_t& data, bool& hasScrolled);
  void HandleKeyboardEvent(FxRWindow& fxrWindow, nsWindow* window,
                           vr::VREvent_Keyboard_t& data);
  bool HandleOverlayMove(FxRWindow& fxrWindow, vr::VREvent_t& aEvent);

  void ToggleMedia();
  void EnsureTransportControls();
  void HideTransportControls();
  void ToggleTransportControlsVisibility();

  vr::VROverlayError ChangeProjectionMode(FxRProjectionMode projectionMode);
  void ToggleProjectionMode();

  // Members for OpenVR
  vr::IVRSystem* mVrApp;
  int32_t mDxgiAdapterIndex;

  // Members for Input
  mozilla::Atomic<bool> mIsOverlayPumpActive;
  HANDLE mOverlayPumpThread;

  // Only a single window is supported for tracking. Support for multiple
  // windows will require a data structure to collect windows as they are
  // created.
  FxRWindow mFxRWindow;
  FxRWindow mTransportWindow;

  bool mIsInFullscreen;
  bool mIsVirtualKeyboardVisible;

  // Members for projection mode toggling
  int mCurrentProjectionIndex = 0;
  const std::vector<FxRProjectionMode> FxRSupportedProjectionModes = {
      VIDEO_PROJECTION_2D, VIDEO_PROJECTION_360, VIDEO_PROJECTION_360S,
      VIDEO_PROJECTION_3D};
};
