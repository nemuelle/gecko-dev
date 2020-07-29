/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "FxRWindowManager.h"

#include "mozilla/Assertions.h"
#include "mozilla/ClearOnShutdown.h"
#include "base/platform_thread.h"

#include "nsIWindowWatcher.h"
#include "nsPIDOMWindow.h"
#include "nsWindow.h"
#include "nsIDOMWindowUtils.h"
#include "WinMouseScrollHandler.h"

#include "mozilla/dom/MediaControlService.h"

#include "service/OpenVRSession.h"

// To view console logging output for FxRWindowManager, add
//     --MOZ_LOG=FxRWindowManager:5
// to cmd line
static mozilla::LazyLogModule gFxrWinLog("FxRWindowManager");

// FxRWindowManager is a singleton in the Main/UI process
static mozilla::StaticAutoPtr<FxRWindowManager> sFxrWinMgrInstance;

// Default window width, in meters
float s_DefaultOverlayWidth = 2.0f;
float s_DefaultOverlayDistance = 2.0f;
float s_MinOverlayPositionHeight = -1.0f;
float s_MaxOverlayPositionHeight = 5.0f;
// Default window transform, in front of the user and facing the origin 
vr::HmdMatrix34_t s_DefaultOverlayTransform = {{
    // no move in x direction
    {1.0f, 0.0f, 0.0f, 0.0f},
    // +y to move it up
    {0.0f, 1.0f, 0.0f, 1.0f},
    // -z to move it forward from the origin
    {0.0f, 0.0f, 1.0f, -s_DefaultOverlayDistance}
}};


FxRWindowManager* FxRWindowManager::GetInstance() {
  if (sFxrWinMgrInstance == nullptr) {
    sFxrWinMgrInstance = new FxRWindowManager();
    ClearOnShutdown(&sFxrWinMgrInstance);
  }

  return sFxrWinMgrInstance;
}

bool FxRWindowManager::HasInstance() {
  return sFxrWinMgrInstance != nullptr;
}

FxRWindowManager::FxRWindowManager()
    : mVrApp(nullptr),
      mDxgiAdapterIndex(-1),
      mIsOverlayPumpActive(false),
      mOverlayPumpThread(nullptr),
      mIsInFullscreen(false),
      mIsVirtualKeyboardVisible(false) {}

FxRWindowManager::~FxRWindowManager() {
  MOZ_ASSERT(mFxRWindow.mOverlayHandle == 0);
  MOZ_ASSERT(mTransportWindow.mOverlayHandle == 0);
  MOZ_ASSERT(mOverlayPumpThread == nullptr);
}

// Initialize an instance of OpenVR for the window manager
bool FxRWindowManager::VRinit() {
  vr::EVRInitError eError = vr::VRInitError_None;
  if (mVrApp == nullptr) {
    mVrApp = vr::VR_Init(&eError, vr::VRApplication_Overlay);
    if (eError == vr::VRInitError_None) {
      mVrApp->GetDXGIOutputInfo(&mDxgiAdapterIndex);
      MOZ_ASSERT(mDxgiAdapterIndex != -1);

      // **TEMPORARY WORKAROUND**
      // Both the main process (where this class runs) and the VR process (via
      // WebVR/XR) interact with SteamVR/OpenVR. As such, both processes end
      // up with the same appkey (system.generated.firefox.exe) even though
      // each process initializes with a different type of application type.
      // This leads to some shared config/data, including the action manifest
      // between the two processes. Because the UI process launches first, it
      // must be responsible for setting this manifest. According to the header
      // file, the action manifest must be set before the first call to
      // IVRInput::UpdateActionState or IVRSystem::PollNextEvent.
      // 
      // To keep behavior consistent, OpenVRSession's function is refactored
      // into a public static function so that this class can call it and set
      // the action manifest. It does not involve synchronizing the manifest
      // and binding paths main proc and GPU proc (as it is between VR proc and
      // GPU proc, see OpenVRControllerManifestManager). But, this can also be
      // fixed if there are synchronization problems.
      // 
      // Note: Setting the action manifest from the main/UI process is a
      // temporary fix. The long-lasting fix is to move all OpenVR calls into
      // the VR process (even for FxR) should to avoid this problem. Also, the
      // main process becomes responsible for deleting the temp files (via
      // passing nullptr for VRParent).
      //
      // Note: SetupContollerActions must be done before any overlays are
      // created because it uses the presence of an overlay to determine whether
      // or not FxR is running.
      mozilla::gfx::ControllerInfo controllerHand[mozilla::gfx::OpenVRHand::Total];
      mozilla::gfx::OpenVRSession::SetupContollerActions(nullptr, controllerHand);
    }
  }

  return (eError == vr::VRInitError_None);
}

// OpenVR allows for an OpenVR scene to have rendering in a separate process,
// but that process must first be identified. When the GPU process PID is
// available, notify OpenVR of this PID.
void FxRWindowManager::SetRenderPid(uint64_t aOverlayId, uint32_t aPid) {
  if (aOverlayId != mFxRWindow.mOverlayHandle &&
      aOverlayId != mTransportWindow.mOverlayHandle) {
    MOZ_CRASH("Unexpected Overlay ID");
  }

  vr::VROverlayError overlayError =
      vr::VROverlay()->SetOverlayRenderingPid(aOverlayId, aPid);
  MOZ_ASSERT(overlayError == vr::VROverlayError_None);
  mozilla::Unused << overlayError;
}

/* static */
bool FxRWindowManager::TryFocusExistingInstance() {
  if (HasInstance() && GetInstance()->mFxRWindow.mWindow != nullptr) {
    GetInstance()->MakeOverlayInteractive(
      GetInstance()->mFxRWindow,
      true);
    return true;
  }
  else {
    return false;
  }
}

/* FxRWindow Helper Methods */

// Returns true if the window at the provided ID was created for Firefox Reality
bool FxRWindowManager::IsFxRWindow(uint64_t aOuterWindowID) {
  return (mFxRWindow.mWindow != nullptr) &&
         (mFxRWindow.mWindow->WindowID() == aOuterWindowID);
}

// Returns true if the window was created for Firefox Reality
bool FxRWindowManager::IsFxRWindow(const nsWindow* aWindow) const {
  return (mFxRWindow.mWindow != nullptr) &&
         (aWindow ==
          mozilla::widget::WidgetUtils::DOMWindowToWidget(mFxRWindow.mWindow)
              .take());
}

FxRWindowManager::FxRWindow& FxRWindowManager::GetFxrWindowFromWidget(
    nsIWidget* widget) {
  if (mFxRWindow.mWidget == widget) {
    return mFxRWindow;
  } else if (mTransportWindow.mWidget == widget) {
    return mTransportWindow;
  } else {
    MOZ_CRASH("Unknown widget");
  }
}

/* FxRWindow Management Methods */

// Track this new Firefox Reality window instance
bool FxRWindowManager::AddWindow(nsPIDOMWindowOuter* aWindow) {
  if (mFxRWindow.mWindow != nullptr) {
    MOZ_CRASH("Only one window is supported");
  }

  InitWindow(mFxRWindow, aWindow);

  const char* newWindowName = "Firefox Reality";
  bool created =
      CreateOverlayForWindow(mFxRWindow, newWindowName, s_DefaultOverlayWidth);
  if (created) {
    // Associate this new window with this new OpenVR overlay for output
    // rendering
    nsCOMPtr<nsIWidget> newWidget =
      mozilla::widget::WidgetUtils::DOMWindowToWidget(mFxRWindow.mWindow);
    newWidget->RequestFxrOutput(mFxRWindow.mOverlayHandle);
    SetOverlayScale(mFxRWindow.mWindow->WindowID(), 1.0f);
  }

  return created;
}

void FxRWindowManager::InitWindow(FxRWindowManager::FxRWindow& newWindow, nsPIDOMWindowOuter* aWindow) {
  MOZ_ASSERT(newWindow.mWindow == nullptr);
  newWindow.mWindow = aWindow;

  // This full reference is released when the window is cleaned up
  newWindow.mWidget =
    mozilla::widget::WidgetUtils::DOMWindowToWidget(newWindow.mWindow).take();

  newWindow.mHwndWidget = (HWND)newWindow.mWidget->GetNativeData(NS_NATIVE_WINDOW);

  ::InitializeCriticalSection(&newWindow.mEventsCritSec);
}

void FxRWindowManager::RemoveWindow(uint64_t aOverlayId) {
  if (aOverlayId != 0 && aOverlayId != mFxRWindow.mOverlayHandle) {
    return;
  }
  MOZ_ASSERT(mFxRWindow.mOverlayHandle != 0);

  if (mIsOverlayPumpActive) {
    // Wait for input thread to return
    mIsOverlayPumpActive = false;
    ::WaitForSingleObject(mOverlayPumpThread, 20 * 1000);

    ::CloseHandle(mOverlayPumpThread);
    mOverlayPumpThread = nullptr;
  }

  CleanupWindow(mFxRWindow);

  // Since only one browser window is supported, close and cleanup the
  // transport window as well because there is no reason for it to be
  // available after the browser window is cleaned up.
  if (mTransportWindow.mWindow != nullptr) {
    MOZ_ASSERT(!mIsInFullscreen);
    mTransportWindow.mWindow->Close();
    CleanupWindow(mTransportWindow);
  }
}

void FxRWindowManager::CleanupWindow(FxRWindowManager::FxRWindow& fxrWindow) {
  vr::VROverlayError overlayError = vr::VROverlay()->DestroyOverlay(
    fxrWindow.mOverlayHandle
  );
  MOZ_ASSERT(overlayError == vr::VROverlayError_None);

  ::DeleteCriticalSection(&fxrWindow.mEventsCritSec);

  fxrWindow.mWidget->Release();

  // Now, clear the state so that another window can be created later
  fxrWindow.Reset();

  mozilla::Unused << overlayError;
}

bool FxRWindowManager::CreateOverlayForWindow(FxRWindow& newWindow,
                                              const char* name, float width) {
  std::string sKey = std::string(name);
  vr::VROverlayError overlayError = vr::VROverlay()->CreateOverlay(
    sKey.c_str(),
    sKey.c_str(),
    &newWindow.mOverlayHandle
  );

  if (overlayError == vr::VROverlayError_None) {
    newWindow.mOverlayWidth = width;
    overlayError = vr::VROverlay()->SetOverlayWidthInMeters(
      newWindow.mOverlayHandle,
      newWindow.mOverlayWidth
    );

    if (overlayError == vr::VROverlayError_None) {
      // Set the transform for the overlay position
      newWindow.mOverlayPosition = s_DefaultOverlayTransform;
      overlayError = vr::VROverlay()->SetOverlayTransformAbsolute(
          newWindow.mOverlayHandle, vr::TrackingUniverseStanding,
          &newWindow.mOverlayPosition);

      if (overlayError == vr::VROverlayError_None) {
        overlayError = vr::VROverlay()->SetOverlayFlag(
          newWindow.mOverlayHandle,
          vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible,
          true);

        if (overlayError == vr::VROverlayError_None) {
          overlayError = vr::VROverlay()->SetOverlayInputMethod(
            newWindow.mOverlayHandle,
            vr::VROverlayInputMethod_Mouse
          );

          if (overlayError == vr::VROverlayError_None) {
            // Finally, show the prepared overlay
            overlayError = vr::VROverlay()->ShowOverlay(
              newWindow.mOverlayHandle
            );
            MOZ_ASSERT(overlayError == vr::VROverlayError_None);

            if (overlayError == vr::VROverlayError_None) {
              // Now, start listening for input...
              overlayError = SetupOverlayInput(newWindow.mOverlayHandle);
            }
          }
        }
      }
    }
  }

  if (overlayError != vr::VROverlayError_None) {
    RemoveWindow(newWindow.mOverlayHandle);
    return false;
  } else {
    return true;
  }
}

void FxRWindowManager::SetOverlayScale(uint64_t aOuterWindowID, float aScale) {
  MOZ_LOG(gFxrWinLog, mozilla::LogLevel::Info,
    ("FxRWindowManager::SetOverlayScale -- (%f)", aScale));

  if (IsFxRWindow(aOuterWindowID)) {
    mFxRWindow.mOverlayWidth = s_DefaultOverlayWidth * aScale;

    vr::VROverlayError overlayError = vr::VROverlay()->SetOverlayWidthInMeters(
      mFxRWindow.mOverlayHandle,
      mFxRWindow.mOverlayWidth
    );

    MOZ_ASSERT(overlayError == vr::VROverlayError_None);
    mozilla::Unused << overlayError;
  }
}

void FxRWindowManager::SetOverlayMoveMode(uint64_t aOuterWindowID, bool aEnable) {
  MOZ_LOG(gFxrWinLog, mozilla::LogLevel::Info,
    ("FxRWindowManager::SetOverlayMoveMode -- (%s)", aEnable ? "true" : "false"));

  if (IsFxRWindow(aOuterWindowID)) {
    // Changing this variable will be reflected while processing VR events via
    // HandleOverlayMove
    mFxRWindow.mIsMoving = aEnable;
  }
}

// This function updates an overlay window's position in space based on the
// cursor's position in the window. The overlay stays "under" the cursor in 3D
// space, seemingly moving on a cylinder around where the user is standing.
//
// There are two inputs from OpenVR to calculate how to move:
// - The current pose of the HMD, in global coordinates
// - The 2D position of the cursor on the overlay, transformed into global
//   coordinates
// The cursor's input (i.e., mouse input via the controller) moves the overlay
// left/right (along x-axis) or up/down (along y-axis). To move the overlay
// forward/backward (along z-axis), the user must physically move (i.e., change
// the position of the HMD). 
//
// Thus, the final matrix set for the overlay results in
// - Yaw rotates to ensure that the overlay always faces the headset
// - Pitch/Roll is constant (i.e., straight upright perpendicular to the floor)
// - 3D point is positioned at the middle of the overlay where the cursor/
//   reticle intersects the overlay at a constant distance from the headset
//
// When the supplied VREvent is consumed for moving the window, this function
// returns true; otherwise, it returns false to indicate that the caller should
// still process the event.
// 
// Note: for some reason, trace logging can disrupt functionality, probably
// because it introduces notable lag that impacts position and transform data.
bool FxRWindowManager::HandleOverlayMove(FxRWindow& fxrWindow, vr::VREvent_t& aEvent) {
  if (!fxrWindow.mIsMoving) {
    return false;
  }

  if (aEvent.eventType == vr::VREvent_MouseMove) {
    MOZ_LOG(gFxrWinLog, mozilla::LogLevel::Info,
      ("FxRWindowManager::HandleOverlayMove -- moving overlay", nullptr));    
    
    const vr::VREvent_Mouse_t data = aEvent.data.mouse;

    // First, get the world position of where the pointer intersects the window
    // and create a point for it.
    const vr::HmdVector2_t coordinatesInOverlay = { data.x, data.y };    
    vr::HmdMatrix34_t mouseCoordTransform;
    vr::VROverlayError overlayError =
      vr::VROverlay()->GetTransformForOverlayCoordinates(
        mFxRWindow.mOverlayHandle, vr::TrackingUniverseStanding,
        coordinatesInOverlay, &mouseCoordTransform);
    MOZ_ASSERT(overlayError == vr::VROverlayError_None);

    mozilla::gfx::Point3D mouseCoord(
      mouseCoordTransform.m[0][3], // x
      mouseCoordTransform.m[1][3], // y
      mouseCoordTransform.m[2][3]  // z
    );

    // Constrain the height of the position of the overlay
    mouseCoord.y = std::max(s_MinOverlayPositionHeight,
      std::min(s_MaxOverlayPositionHeight, mouseCoord.y));

    // Next, capture the current headpose to get the HMD's position in space.
    vr::TrackedDevicePose_t currentHeadPoseData;
    vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(
      vr::TrackingUniverseStanding,
      0, // fPredictedSecondsToPhotonsFromNow
      &currentHeadPoseData,
      1 // unTrackedDevicePoseArrayCount
    );
    const vr::HmdMatrix34_t currentHeadPose = 
      currentHeadPoseData.mDeviceToAbsoluteTracking;    
    
    // Now, calculate the rotation vectors of the overlay such that the overlay
    // always faces the HMD.    

    // Since the overlay will be upright, start with a normalized vertical axis.
    const mozilla::gfx::Point3D lookAtY = mozilla::gfx::Point3D(0.0, 1.0, 0.0);

    // Calculate the vector between the overlay and the HMD on the same plane
    // (i.e., the point of the HMD at the same height as the mouse).
    const mozilla::gfx::Point3D hmdCoordAtMouseHeight(
        currentHeadPose.m[0][3],  // x
        mouseCoord[1],            // y 
        currentHeadPose.m[2][3]   // z
      );
    mozilla::gfx::Point3D lookAtZ = hmdCoordAtMouseHeight - mouseCoord;
    lookAtZ.Normalize();

    // The final vector is simply the cross product of the two known vectors
    // (i.e, the tangent of the cylinder to which the overlay is bound).
    mozilla::gfx::Point3D lookAtX = lookAtY.CrossProduct(lookAtZ);
    lookAtX.Normalize();

    // Update the position of the overlay so that it is at a constant distance
    // from the HMD along the vector between the HMD and the mouse. This point
    // will be the new center of the overlay.
    float moveBy =
      mouseCoord.Distance(hmdCoordAtMouseHeight) - s_DefaultOverlayDistance;
    mouseCoord.MoveBy(
      lookAtZ[0] * moveBy, // x
      lookAtZ[1] * moveBy, // y
      lookAtZ[2] * moveBy  // z
    );

    // Finally, create the transform matrix using the rotation and position
    // vectors/ calculated above and set the matrix on the overlay.
    mFxRWindow.mOverlayPosition = { {
      {lookAtX[0], lookAtY[0], lookAtZ[0], mouseCoord[0]},
      {lookAtX[1], lookAtY[1], lookAtZ[1], mouseCoord[1]},
      {lookAtX[2], lookAtY[2], lookAtZ[2], mouseCoord[2]}
    } };

    overlayError = vr::VROverlay()->SetOverlayTransformAbsolute(
      mFxRWindow.mOverlayHandle, vr::TrackingUniverseStanding,
      &mFxRWindow.mOverlayPosition);
    MOZ_ASSERT(overlayError == vr::VROverlayError_None);
    mozilla::Unused << overlayError;

    return true;
  }
  else if (aEvent.eventType == vr::VREvent_MouseButtonUp
    || aEvent.eventType == vr::VREvent_OverlayFocusChanged) {
    MOZ_LOG(gFxrWinLog, mozilla::LogLevel::Info,
      ("FxRWindowManager::HandleOverlayMove -- move complete", nullptr));

    fxrWindow.mIsMoving = false;
  }

  return false;
}

/* FxRWindow Input management */

// Creates a new thread dedicated to polling input from OpenVR. See
// CollectOverlayEvents for more details.
vr::VROverlayError FxRWindowManager::SetupOverlayInput(
    vr::VROverlayHandle_t overlayId) {
  // Enable scrolling for this overlay
  vr::VROverlayError overlayError = vr::VROverlay()->SetOverlayFlag(
      overlayId, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);

  if (overlayError == vr::VROverlayError_None && !mIsOverlayPumpActive) {
    mIsOverlayPumpActive = true;
    DWORD dwTid = 0;
    // TODO: A more Gecko-y way of spawning a new thread?
    mOverlayPumpThread = ::CreateThread(
        nullptr, 0, FxRWindowManager::OverlayInputPump, this, 0, &dwTid);
  }
  return overlayError;
}

// Definition of ThreadProc for Input thread
DWORD FxRWindowManager::OverlayInputPump(LPVOID lpParameter) {
  PlatformThread::SetName("OpenVR Overlay Input");

  FxRWindowManager* manager = static_cast<FxRWindowManager*>(lpParameter);

  MOZ_LOG(gFxrWinLog, mozilla::LogLevel::Info,
          ("FxRWindowManager:OverlayInputPump started (%llX)", manager));

  while (manager->mIsOverlayPumpActive) {
    manager->CollectOverlayEvents(manager->mFxRWindow);
    if (manager->mTransportWindow.mOverlayHandle != 0) {
      manager->CollectOverlayEvents(manager->mTransportWindow);
    }
    // Yield the thread
    ::Sleep(0);
  }

  MOZ_LOG(gFxrWinLog, mozilla::LogLevel::Info,
          ("FxRWindowManager:OverlayInputPump exited (%llX)", manager));

  return 0;
}

// Runs on background thread because input events from OpenVR are retrieved
// through polling, which makes blocking the thread inevitable. This would be
// bad on UI thread. Since UI widget messages cannot be dispatched to nsWindow
// from another thread (need to confirm), collect OpenVR input events in
// m_rgEvents on this background thread and post a message (MOZ_WM_OPENVR_EVENT)
// that events are ready for UI thread/widget.
void FxRWindowManager::CollectOverlayEvents(FxRWindow& fxrWindow) {
  // Need to find a better place to put this. The problem that needs to be
  // solved is knowing the texture size so that mouse coords can be translated
  // late. This is put in this function because it won't block the UI thread.
  // .right is compared to <= 1 because
  // - if == 0, then uninitialized
  // - if == 1, then mousescale hasn't been set by GPU process yet (default
  // normalizes to 1.0f)
  if (fxrWindow.mOverlaySizeRec.right <= 1) {
    vr::HmdVector2_t vecWindowSize = {0};
    vr::EVROverlayError overlayError = vr::VROverlay()->GetOverlayMouseScale(
        fxrWindow.mOverlayHandle, &vecWindowSize);

    MOZ_ASSERT(overlayError == vr::VROverlayError_None);
    mozilla::Unused << overlayError;

    fxrWindow.mOverlaySizeRec.right = vecWindowSize.v[0];
    fxrWindow.mOverlaySizeRec.bottom = vecWindowSize.v[1];
  }

  // Acquire CS
  // TODO: Scope this critsec down further, creating another vector on the
  // stack for copying like ProcessOverlayEvents
  ::EnterCriticalSection(&fxrWindow.mEventsCritSec);

  bool initiallyEmpty = fxrWindow.mEventsVector.empty();

  // Add events to vector
  vr::VREvent_t vrEvent;
  while (vr::VROverlay()->PollNextOverlayEvent(fxrWindow.mOverlayHandle, &vrEvent,
    sizeof(vrEvent))) {

    if (vrEvent.eventType != vr::VREvent_MouseMove) {
      MOZ_LOG(gFxrWinLog, mozilla::LogLevel::Info,
              ("VREvent_t.eventType: %s",
               vr::VRSystem()->GetEventTypeNameFromEnum(
                   (vr::EVREventType)(vrEvent.eventType))));
    }
    
    switch (vrEvent.eventType) {
      case vr::VREvent_ScrollDiscrete:
      case vr::VREvent_MouseMove:
      case vr::VREvent_MouseButtonDown:
      case vr::VREvent_MouseButtonUp:
      case vr::VREvent_ButtonPress:
      case vr::VREvent_ButtonUnpress:
      case vr::VREvent_KeyboardCharInput:
      case vr::VREvent_KeyboardClosed:
      case vr::VREvent_OverlayFocusChanged: {
        if (!HandleOverlayMove(fxrWindow, vrEvent)) {
          fxrWindow.mEventsVector.emplace_back(vrEvent);
        }
        break;
      }
      default:
        break;
    }    
  }

  // Post message to UI thread that new events are waiting
  // TODO: cross-plat notification
  if (initiallyEmpty && !fxrWindow.mEventsVector.empty()) {
    PostMessage(fxrWindow.mHwndWidget, MOZ_WM_OPENVR_EVENT, 0, 0);
  }

  ::LeaveCriticalSection(&fxrWindow.mEventsCritSec);
}

// Runs on UI thread (for reasons explained with CollectOverlayEvents).
// Copies OpenVR events that were collected on background thread and converts
// them to UI events to be dispatched by the widget.
void FxRWindowManager::ProcessOverlayEvents(nsWindow* window) {
  VREventVector rgEvents;
  // See note above SynthesizeNativeMouseScrollEvent for reasoning
  bool hasScrolled = false;

  FxRWindow& fxrWindow = GetFxrWindowFromWidget(window);
  
  // Acquire CS
  ::EnterCriticalSection(&fxrWindow.mEventsCritSec);

  // Copy elements to stack vector to minimize CritSec acquisition
  fxrWindow.mEventsVector.swap(rgEvents);

  // Clear vector
  fxrWindow.mEventsVector.clear();

  ::LeaveCriticalSection(&fxrWindow.mEventsCritSec);
  
  // Assert size > 0
  MOZ_ASSERT(!rgEvents.empty());

  // Process events
  for (auto iter = rgEvents.begin(); iter != rgEvents.end(); iter++) {
    uint32_t eventType = iter->eventType;

    switch (eventType) {
      case vr::VREvent_MouseMove:
      case vr::VREvent_MouseButtonUp:
      case vr::VREvent_MouseButtonDown: {
        vr::VREvent_Mouse_t data = iter->data.mouse;

        HandleMouseEvent(fxrWindow, window, data, eventType);
        break;
      }

      case vr::VREvent_ButtonPress:
      case vr::VREvent_ButtonUnpress: {
        vr::VREvent_Controller_t data = iter->data.controller;

        MOZ_LOG(gFxrWinLog, mozilla::LogLevel::Info,
                ("VREvent_Controller_t.button: %u", data.button));

        break;
      }

      case vr::VREvent_ScrollDiscrete: {
        vr::VREvent_Scroll_t data = iter->data.scroll;

        HandleScrollEvent(fxrWindow, window, data, hasScrolled);
        break;
      }

      case vr::VREvent_KeyboardCharInput: {
        vr::VREvent_Keyboard_t data = iter->data.keyboard;

        HandleKeyboardEvent(fxrWindow, window, data);
        break;
      }

      case vr::VREvent_KeyboardClosed: {
        mIsVirtualKeyboardVisible = false;
        break;
      }

      case vr::VREvent_OverlayFocusChanged: {
        // As the Overlay's focus changes, update how Firefox sees the focus
        // state of this window. This is especially important so that text
        // input can get the caret and invoke the virtual keyboard.
        // Note that this also means that the Fx Window for the OpenVR Overlay
        // participates in the same focus management as windows on the
        // desktop, so the overlay can steal focus from a desktop Firefox
        // window and vice-versa.
        // Note: when the focus changes while the virtual keyboard is visible,
        // keep the focus state the same for the firefox window. The keyboard
        // represents another overlay, so no need for Firefox to change focus
        // state in this case.

        if (!mIsVirtualKeyboardVisible) {
          vr::VREvent_Overlay_t data = iter->data.overlay;

          bool isFocused = data.overlayHandle == fxrWindow.mOverlayHandle;

          MOZ_LOG(gFxrWinLog, mozilla::LogLevel::Info,
                  ("Overlay focus: %s", isFocused ? "true" : "false"));

          window->DispatchFocusToTopLevelWindow(isFocused);
        }
        break;
      }
    }
  }

  window->DispatchPendingEvents();
}

void FxRWindowManager::ToggleOverlayInteractivity(uint64_t aOuterWindowID) {

  bool oldValue;
  vr::VROverlayError overlayError = vr::VROverlay()->GetOverlayFlag(
    mFxRWindow.mOverlayHandle,
    vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible,
    &oldValue);

  if (overlayError == vr::VROverlayError_None) {
    overlayError = vr::VROverlay()->SetOverlayFlag(
      mFxRWindow.mOverlayHandle,
      vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible,
      !oldValue);

    MOZ_ASSERT(overlayError == vr::VROverlayError_None);
  }
}

void FxRWindowManager::MakeOverlayInteractive(
  FxRWindowManager::FxRWindow& fxrWindow,
  bool aInteractive) {

  vr::VROverlayError overlayError = vr::VROverlay()->SetOverlayFlag(
    fxrWindow.mOverlayHandle,
    vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible,
    aInteractive);

  MOZ_ASSERT(overlayError == vr::VROverlayError_None);
  mozilla::Unused << overlayError;
}

void FxRWindowManager::HandleMouseEvent(FxRWindowManager::FxRWindow& fxrWindow,
                                        nsWindow* window,
                                        vr::VREvent_Mouse_t& data,
                                        uint32_t eventType) {
  if (eventType == vr::VREvent_MouseButtonDown ||
      eventType == vr::VREvent_MouseButtonUp) {
    MOZ_LOG(gFxrWinLog, mozilla::LogLevel::Info,
            ("VREvent_Mouse_t.button: %u", data.button));
  }

  // Windows' origin is top-left, whereas OpenVR's origin is
  // bottom-left, so transform the y-coordinate.
  fxrWindow.mLastMousePt.x = (LONG)(data.x);
  fxrWindow.mLastMousePt.y = fxrWindow.mOverlaySizeRec.bottom - (LONG)(data.y);

  if (data.button != vr::EVRMouseButton::VRMouseButton_Right) {
    mozilla::EventMessage eMsg;
    if (eventType == vr::VREvent_MouseMove) {
      eMsg = mozilla::EventMessage::eMouseMove;
    } else if (eventType == vr::VREvent_MouseButtonDown) {
      eMsg = mozilla::EventMessage::eMouseDown;
    } else {
      MOZ_ASSERT(eventType == vr::VREvent_MouseButtonUp);
      eMsg = mozilla::EventMessage::eMouseUp;
    }

    window->DispatchMouseEvent(eMsg,
                               0,                                     // wParam
                               POINTTOPOINTS(fxrWindow.mLastMousePt)  // lParam
    );
  } else if (eventType == vr::VREvent_MouseButtonUp) {
    // When the 2nd button is released, toggle the transport controls.
    ToggleTransportControlsVisibility();
  }
}

void FxRWindowManager::HandleScrollEvent(FxRWindowManager::FxRWindow& fxrWindow,
                                         nsWindow* window,
                                         vr::VREvent_Scroll_t& data,
                                         bool& hasScrolled) {
  MOZ_LOG(gFxrWinLog, mozilla::LogLevel::Info, ("scroll", nullptr));

  if (!hasScrolled) {
    SHORT scrollDelta = WHEEL_DELTA * (short)data.ydelta;

    // Note: two important things about Synthesize below
    // - It uses SendMessage, not PostMessage, so it's a synchronous call
    // to scroll
    // - Because it's synchronous and because the synthesizer doesn't
    // support multiple synthesized events (i.e., needs a call to Finish),
    // only one can be processed at a time in this message loop
    mozilla::LayoutDeviceIntPoint pt;
    pt.x = fxrWindow.mLastMousePt.x;
    pt.y = fxrWindow.mLastMousePt.y;

    mozilla::widget::MouseScrollHandler::SynthesizeNativeMouseScrollEvent(
        window, pt, WM_MOUSEWHEEL, scrollDelta,
        0,  // aModifierFlags
        nsIDOMWindowUtils::MOUSESCROLL_SEND_TO_WIDGET |
            nsIDOMWindowUtils::MOUSESCROLL_POINT_IN_WINDOW_COORD);

    hasScrolled = true;
  }
}

void FxRWindowManager::HandleKeyboardEvent(
    FxRWindowManager::FxRWindow& fxrWindow, nsWindow* window,
    vr::VREvent_Keyboard_t& data) {
  size_t inputLength = strnlen_s(data.cNewInput, ARRAYSIZE(data.cNewInput));
  wchar_t msgChar = data.cNewInput[0];

  if (inputLength > 1) {
    // The event can contain multi-byte UTF8 characters. Convert them to
    // a single Wide character to send to Gecko
    wchar_t convertedChar[ARRAYSIZE(data.cNewInput)] = {0};
    int convertedReturn = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, data.cNewInput, inputLength,
        convertedChar, ARRAYSIZE(convertedChar));

    MOZ_ASSERT(convertedReturn == 1);
    mozilla::Unused << convertedReturn;

    msgChar = convertedChar[0];
  } else {
    MOZ_ASSERT(inputLength == 1);
    if (msgChar == L'\n') {
      // Make new line
      msgChar = VK_RETURN;
    }
  }

  switch (msgChar) {
    // These characters need to be mapped to key presses rather than char
    // so that they map to actions instead.
    case VK_BACK:
    case VK_TAB:
    case VK_RETURN:
    case VK_ESCAPE: {
      MSG nativeMsgDown = mozilla::widget::WinUtils::InitMSG(
          WM_KEYDOWN, msgChar, 0, fxrWindow.mHwndWidget);
      window->ProcessKeyDownMessage(nativeMsgDown, nullptr);

      MSG nativeMsgUp = mozilla::widget::WinUtils::InitMSG(
          WM_KEYUP, msgChar, 0, fxrWindow.mHwndWidget);
      window->ProcessKeyUpMessage(nativeMsgUp, nullptr);

      break;
    }

    default: {
      MSG nativeMsg = mozilla::widget::WinUtils::InitMSG(WM_CHAR, msgChar, 0,
                                                         fxrWindow.mHwndWidget);

      window->ProcessCharMessage(nativeMsg, nullptr);
      break;
    }
  }
}

void FxRWindowManager::ShowVirtualKeyboardForWindow(uint64_t aOuterWindowID) {
  if (IsFxRWindow(aOuterWindowID)) {
    ShowVirtualKeyboard(mFxRWindow.mOverlayHandle);
  }
}

void FxRWindowManager::ShowVirtualKeyboard(uint64_t aOverlayId) {
  // Note: bUseMinimalMode set to true so that each char arrives as an event.
  vr::VROverlayError overlayError = vr::VROverlay()->ShowKeyboardForOverlay(
      aOverlayId,
      vr::k_EGamepadTextInputModeNormal,
      vr::k_EGamepadTextInputLineModeSingleLine,
      vr::KeyboardFlag_Minimal,
      "FxR",  // pchDescription,
      100,    // unCharMax,
      "",     // pchExistingText,
      0       // uint64_t uUserValue
  );

  if (overlayError == vr::VROverlayError_None ||
    overlayError == vr::VROverlayError_KeyboardAlreadyInUse) {
    mIsVirtualKeyboardVisible = true;

    // Now, ensure that the keyboard doesn't overlap the overlay by providing a
    // rect for OpenVR to avoid (i.e., the whole overlay texture).
    uint32_t width = 0;
    uint32_t height = 0;
    overlayError =
        vr::VROverlay()->GetOverlayTextureSize(aOverlayId, &width, &height);
    vr::HmdRect2_t rect = {{0.0f, (float)height}, {(float)width, 0.0f}};
    vr::VROverlay()->SetKeyboardPositionForOverlay(aOverlayId, rect);

    MOZ_ASSERT(overlayError == vr::VROverlayError_None);
    MOZ_ASSERT(width != 0 && height != 0);
  }
  else {
    MOZ_ASSERT(false, "Failed to show virtual keyboard");
  }
}

void FxRWindowManager::HideVirtualKeyboard() {
  if (vr::VROverlay() != nullptr) {
    vr::VROverlay()->HideKeyboard();
  }
}

/* FxRWindow Media Management */

// Handle when WebVR/XR content is showing or not, so that both the FxR Overlay
// and the Fx immersive scene do not render at the same time
void FxRWindowManager::OnWebXRPresentationChange(uint64_t aOuterWindowID,
                                                 bool isPresenting) {
  if (IsFxRWindow(aOuterWindowID)) {
    vr::VROverlayError overlayError;
    if (isPresenting) {
      overlayError = vr::VROverlay()->HideOverlay(mFxRWindow.mOverlayHandle);
    } else {
      overlayError = vr::VROverlay()->ShowOverlay(mFxRWindow.mOverlayHandle);
    }

    MOZ_ASSERT(overlayError == vr::VROverlayError_None);
  }
}

void FxRWindowManager::OnFullScreenChange(uint64_t aOuterWindowID,
                                          bool aIsFullScreen) {
  if (IsFxRWindow(aOuterWindowID)) {
    mIsInFullscreen = aIsFullScreen;

    if (aIsFullScreen) {
      // Create the transport controls overlay
      EnsureTransportControls();
    } else {
      // Close the transport controls overlay
      HideTransportControls();
      vr::VROverlayError overlayError = ChangeProjectionMode(VIDEO_PROJECTION_2D);
      MOZ_ASSERT(overlayError == vr::VROverlayError_None);
    }
  }
}

// Forwarded from privileged javascript and supports modifying via the
// following arguments:
// - "toggle" - Toggles between playing and pausing current media
void FxRWindowManager::SetPlayMediaState(const nsAString& aState) {
  if (aState == u"toggle") {
    ToggleMedia();
  } else {
    MOZ_CRASH("Unsupported Param");
  }
}

void FxRWindowManager::ToggleMedia() {
  RefPtr<mozilla::dom::MediaControlService> service =
      mozilla::dom::MediaControlService::GetService();
  mozilla::dom::MediaControlKeySource* source =
      service->GetMediaControlKeySource();

  source->OnKeyPressed(mozilla::dom::MediaControlKey::Playpause);
}

// Forwarded from privileged javascript and supports changing projection mode
// or exiting fullscreen presentation via the following arguments:
// - "exit" - Ends the current fullscreen presentation
// - "2d" - for Theater mode display
// - "360" - Maps to VIDEO_PROJECTION_360
// - "360-stereo" - Maps to VIDEO_PROJECTION_360S
// - "3d" - Maps to VIDEO_PROJECTION_3D
void FxRWindowManager::SetProjectionMode(const nsAString& aMode) {
  MOZ_LOG(gFxrWinLog, mozilla::LogLevel::Info,
          ("FxRWindowManager::SetProjectionMode - %s",
           NS_ConvertUTF16toUTF8(aMode).Data()));

  if (mFxRWindow.mOverlayHandle != 0) {
    if (aMode == u"360") {
      ChangeProjectionMode(VIDEO_PROJECTION_360);
    } else if (aMode == u"360-stereo") {
      ChangeProjectionMode(VIDEO_PROJECTION_360S);
    } else if (aMode == u"3d") {
      ChangeProjectionMode(VIDEO_PROJECTION_3D);
    } else if (aMode == u"2d") {
      ChangeProjectionMode(VIDEO_PROJECTION_2D);
    } else if (aMode == u"exit") {
      mFxRWindow.mWindow->SetFullScreen(false);
    }
  }
}

// Changes the projection mode to one of the supported projection modes defined
// in FxRProjectionMode.
vr::VROverlayError FxRWindowManager::ChangeProjectionMode(
    FxRProjectionMode projectionMode) {
  MOZ_ASSERT(mFxRWindow.mOverlayHandle != 0);

  bool isPanorama = (projectionMode == FxRProjectionMode::VIDEO_PROJECTION_360);
  bool isStereoPanorama =
      (projectionMode == FxRProjectionMode::VIDEO_PROJECTION_360S);
  bool isStereo2D = (projectionMode == FxRProjectionMode::VIDEO_PROJECTION_3D);

  vr::VROverlayError overlayError = vr::VROverlayError_None;
  if (isPanorama || isStereoPanorama) {
    overlayError = vr::VROverlay()->SetOverlayWidthInMeters(
        mFxRWindow.mOverlayHandle, 6.0f);

    if (overlayError == vr::VROverlayError_None) {
      // For panoramic viewing, we want the overlay closer to the user's eyes to
      // fill the entire FOV
      vr::HmdMatrix34_t transform = {{
          {1.0f, 0.0f, 0.0f, 0.0f},  // no move in x direction
          {0.0f, 1.0f, 0.0f, 0.0f},  // +y to move it up
          {0.0f, 0.0f, 1.0f, -2.1f}  // -z to move it forward from the origin
      }};
      // Keep the content centered at user's head
      overlayError = vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(
          mFxRWindow.mOverlayHandle, vr::k_unTrackedDeviceIndex_Hmd,
          &transform);
    }
  } else {
    // Set width/size back to most recent size
    overlayError = vr::VROverlay()->SetOverlayWidthInMeters(
        mFxRWindow.mOverlayHandle, mFxRWindow.mOverlayWidth);

    if (overlayError == vr::VROverlayError_None) {
      if (isStereo2D) {
        // For stereo viewing, we want the overlay further from the user's eyes,
        // as the apparent distance of the resultant 3D image is closer than a
        // 2D image

        vr::HmdMatrix34_t transform = {{
            {1.0f, 0.0f, 0.0f, 0.0f},  // no move in x direction
            {0.0f, 1.0f, 0.0f, 2.0f},  // +y to move it up
            {0.0f, 0.0f, 1.0f, -6.0f}  // -z to move it forward from the origin
        }};

        overlayError = vr::VROverlay()->SetOverlayTransformAbsolute(
            mFxRWindow.mOverlayHandle, vr::TrackingUniverseStanding,
            &transform);
      } else {
        overlayError = vr::VROverlay()->SetOverlayTransformAbsolute(
            mFxRWindow.mOverlayHandle, vr::TrackingUniverseStanding,
            &mFxRWindow.mOverlayPosition);
      }
    }
  }

  if (overlayError == vr::VROverlayError_None) {
    overlayError = vr::VROverlay()->SetOverlayFlag(
        mFxRWindow.mOverlayHandle, vr::VROverlayFlags_Panorama, isPanorama);
  }
  if (overlayError == vr::VROverlayError_None) {
    overlayError = vr::VROverlay()->SetOverlayFlag(
        mFxRWindow.mOverlayHandle, vr::VROverlayFlags_StereoPanorama,
        isStereoPanorama);
  }
  if (overlayError == vr::VROverlayError_None) {
    overlayError = vr::VROverlay()->SetOverlayFlag(
        mFxRWindow.mOverlayHandle, vr::VROverlayFlags_SideBySide_Parallel,
        isStereo2D);
  }

  // TODO: If there is an overlay error, reset back to original overlay
  // position and size

  return overlayError;
}

// TODO: Can this be removed?
void FxRWindowManager::ToggleProjectionMode() {
  mCurrentProjectionIndex =
      (mCurrentProjectionIndex < FxRSupportedProjectionModes.size() - 1)
          ? mCurrentProjectionIndex + 1
          : 0;

  FxRProjectionMode nextMode =
      FxRSupportedProjectionModes[mCurrentProjectionIndex];
  ChangeProjectionMode(nextMode);
}

void FxRWindowManager::EnsureTransportControls() {
  MOZ_ASSERT(mIsInFullscreen);

  vr::VROverlayError overlayError;
  // Setup the window if it doesn't already exist
  if (mTransportWindow.mOverlayHandle == 0) {
    nsCOMPtr<nsIWindowWatcher> wwatch =
      do_GetService(NS_WINDOWWATCHER_CONTRACTID);
    MOZ_ASSERT(wwatch != nullptr, "Failed to get WindowWatcher");
    
    nsCOMPtr<mozIDOMWindowProxy> newDOMWindow;
    nsresult result = wwatch->OpenWindow(
      nullptr,                            // aParent
      "chrome://fxr/content/fxr-transport-controls.html",  // aUrl
      "_blank",                           // aName
      "chrome,dialog=no,all",             // aFeatures
      nullptr,  // aArguments
      getter_AddRefs(newDOMWindow));
    MOZ_ASSERT(result == NS_OK);
    mozilla::Unused << result;

    nsPIDOMWindowOuter* newWindowOuter = nsPIDOMWindowOuter::From(newDOMWindow);
    InitWindow(mTransportWindow, newWindowOuter);

    const char* newWindowName = "Firefox Reality Transport Controls";
    if (CreateOverlayForWindow(mTransportWindow, newWindowName, 1.0f)) {
      nsCOMPtr<nsIWidget> newWidget =
        mozilla::widget::WidgetUtils::DOMWindowToWidget(newWindowOuter);
      newWidget->RequestFxrOutput(mTransportWindow.mOverlayHandle);
    }
  } else {
    // The overlay for the controls are already created, so simply show them.
    overlayError =
        vr::VROverlay()->ShowOverlay(mTransportWindow.mOverlayHandle);
    MOZ_ASSERT(overlayError == vr::VROverlayError_None);
  }

  // Set the transform for the overlay position relative to the main
  // overlay window
  vr::HmdMatrix34_t transform = mFxRWindow.mOverlayPosition;
  transform.m[1][3] -= (mFxRWindow.mOverlayWidth / 3.0); // down below the main
  transform.m[2][3] += 0.1f; // back slightly to the user

  overlayError = vr::VROverlay()->SetOverlayTransformAbsolute(
    mTransportWindow.mOverlayHandle, vr::TrackingUniverseStanding,
    &transform);
  MOZ_ASSERT(overlayError == vr::VROverlayError_None);

  mozilla::Unused << overlayError;
}

void FxRWindowManager::HideTransportControls() {
  MOZ_ASSERT(mTransportWindow.mOverlayHandle != 0);
  MOZ_LOG(gFxrWinLog, mozilla::LogLevel::Info,
          ("FxRWindowManager::HideTransportControls", nullptr));

  vr::VROverlayError overlayError =
      vr::VROverlay()->HideOverlay(mTransportWindow.mOverlayHandle);

  MOZ_ASSERT(overlayError == vr::VROverlayError_None);
  mozilla::Unused << overlayError;
}

void FxRWindowManager::ToggleTransportControlsVisibility() {
  if (mIsInFullscreen && mTransportWindow.mOverlayHandle != 0) {
    vr::VROverlayError overlayError;
    if (vr::VROverlay()->IsOverlayVisible(mTransportWindow.mOverlayHandle)) {
      overlayError =
          vr::VROverlay()->HideOverlay(mTransportWindow.mOverlayHandle);
    } else {
      overlayError =
          vr::VROverlay()->ShowOverlay(mTransportWindow.mOverlayHandle);
    }

    MOZ_ASSERT(overlayError == vr::VROverlayError_None);
    mozilla::Unused << overlayError;
  }
}
