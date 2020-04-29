/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "FxRWindowManager.h"

#include <service/openvr/src/pathtools_public.h>

#include "mozilla/Assertions.h"
#include "mozilla/ClearOnShutdown.h"
#include "base/platform_thread.h"

#include "nsIWindowWatcher.h"
#include "nsPIDOMWindow.h"
#include "nsWindow.h"
#include "nsIDOMWindowUtils.h"
#include "WinMouseScrollHandler.h"

#include "mozilla/dom/MediaControlService.h"

static mozilla::StaticAutoPtr<FxRWindowManager> sFxrWinMgrInstance;
// To use this for console output, add --MOZ_LOG=FxRWindowManager:5 to cmd line
static mozilla::LazyLogModule gFxrWinLog("FxRWindowManager");

FxRWindowManager* FxRWindowManager::GetInstance() {
  if (sFxrWinMgrInstance == nullptr) {
    sFxrWinMgrInstance = new FxRWindowManager();
    ClearOnShutdown(&sFxrWinMgrInstance);
  }

  return sFxrWinMgrInstance;
}

FxRWindowManager::FxRWindowManager() :
  mVrApp(nullptr),
  mDxgiAdapterIndex(-1),
  mIsOverlayPumpActive(false),
  mOverlayPumpThread(nullptr),
  mFxRWindow({ 0 }),
  mTransportWindow({ 0 })
{ }

FxRWindowManager::~FxRWindowManager() {
  MOZ_ASSERT(mFxRWindow.mOverlayHandle == 0);
  MOZ_ASSERT(mOverlayPumpThread == nullptr);
  MOZ_ASSERT(mFxRWindow.mTransportControlsOverlayHandle == 0);
}

// Initialize an instance of OpenVR for the window manager
bool FxRWindowManager::VRinit() {
  vr::EVRInitError eError = vr::VRInitError_None;
  if (mVrApp == nullptr) {
    mVrApp = vr::VR_Init(&eError, vr::VRApplication_Overlay);
    if (eError == vr::VRInitError_None) {
      mVrApp->GetDXGIOutputInfo(&mDxgiAdapterIndex);
      MOZ_ASSERT(mDxgiAdapterIndex != -1);
    }
  }

  return (eError == vr::VRInitError_None);
}

// Track this new Firefox Reality window instance
bool FxRWindowManager::AddWindow(nsPIDOMWindowOuter* aWindow) {
  if (mFxRWindow.mWindow != nullptr) {
    MOZ_CRASH("Only one window is supported");
  }

  InitWindow(mFxRWindow, aWindow);

  bool created = CreateOverlayForWindow(mFxRWindow, "Firefox Reality", 4.0f);
  if (created) {
    // Associate this new window with this new OpenVR overlay for output
    // rendering
    nsCOMPtr<nsIWidget> newWidget =
      mozilla::widget::WidgetUtils::DOMWindowToWidget(mFxRWindow.mWindow);
    newWidget->RequestFxrOutput(mFxRWindow.mOverlayHandle);
  }

  return created;
}

void FxRWindowManager::InitWindow(FxRWindowManager::FxRWindow& newWindow, nsPIDOMWindowOuter* aWindow) {
  MOZ_ASSERT(newWindow.mWindow == nullptr);
  newWindow.mWindow = aWindow;

  // This full reference is released when the window is removed
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
  if (mTransportWindow.mOverlayHandle != 0) {
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
  fxrWindow = { 0 };
}

FxRWindowManager::FxRWindow& FxRWindowManager::GetFxrWindowFromWidget(nsIWidget* widget) {
  if (mFxRWindow.mWidget == widget) {
    return mFxRWindow;
  }
  else if (mTransportWindow.mWidget == widget) {
    return mTransportWindow;
  }
  else {
    MOZ_CRASH("Unknown widget");
  }
}

void FxRWindowManager::SetRenderPid(uint64_t aOverlayId, uint32_t aPid) {
  if (aOverlayId != mFxRWindow.mOverlayHandle && aOverlayId != mTransportWindow.mOverlayHandle) {
    MOZ_CRASH("Unexpected Overlay ID");
  }

  vr::VROverlayError error = vr::VROverlay()->SetOverlayRenderingPid(
    aOverlayId,
    aPid
  );
  MOZ_ASSERT(error == vr::VROverlayError_None);
}

bool FxRWindowManager::CreateOverlayForWindow(
  FxRWindow& newWindow, char* name, float width) {
  std::string sKey = std::string(name);
  vr::VROverlayError overlayError = vr::VROverlay()->CreateOverlay(
    sKey.c_str(),
    sKey.c_str(),
    &newWindow.mOverlayHandle
  );

  if (overlayError == vr::VROverlayError_None) {
    // Start with default width of 4m
    overlayError = vr::VROverlay()->SetOverlayWidthInMeters(
      newWindow.mOverlayHandle,
      width
    );

    if (overlayError == vr::VROverlayError_None) {
      // Set the transform for the overlay position
      vr::HmdMatrix34_t transform = {
        1.0f, 0.0f, 0.0f,  0.0f, // no move in x direction
        0.0f, 1.0f, 0.0f,  2.0f, // +y to move it up
        0.0f, 0.0f, 1.0f, -2.0f  // -z to move it forward from the origin
      };
      overlayError = vr::VROverlay()->SetOverlayTransformAbsolute(
        newWindow.mOverlayHandle,
        vr::TrackingUniverseStanding,
        &transform
      );

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

vr::VROverlayError FxRWindowManager::CreateTransportControlsOverlay() {
  std::string sKey = std::string("Firefox Reality Transport Controls");
  vr::VROverlayError overlayError = vr::VROverlay()->CreateOverlay(
      sKey.c_str(), sKey.c_str(), &mFxRWindow.mTransportControlsOverlayHandle);

  // Place the transport controls just in front of the window
  if (overlayError == vr::VROverlayError_None) {
    vr::HmdMatrix34_t transform = {
        1.0f, 0.0f, 0.0f, 0.0f,  // no move in x direction
        0.0f, 1.0f, 0.0f, 1.0f,  // +y to move it up
        0.0f, 0.0f, 1.0f, -1.0f  // -z to move it forward from the origin
    };

    overlayError = vr::VROverlay()->SetOverlayTransformAbsolute(
        mFxRWindow.mTransportControlsOverlayHandle,
        vr::TrackingUniverseStanding, &transform);
    if (overlayError == vr::VROverlayError_None) {
      // Use placeholder image for the transport controls overlay
      std::string overlayImagePath =
          Path_StripFilename(Path_GetExecutablePath())
              .append(
                  "\\browser\\chrome\\browser\\content\\branding\\about.png");

      overlayError = vr::VROverlay()->SetOverlayFromFile(
          mFxRWindow.mTransportControlsOverlayHandle, overlayImagePath.c_str());
      if (overlayError == vr::VROverlayError_None) {
        overlayError = vr::VROverlay()->SetOverlayWidthInMeters(
            mFxRWindow.mTransportControlsOverlayHandle, 0.5f);
        if (overlayError == vr::VROverlayError_None) {
          overlayError = vr::VROverlay()->SetOverlayFlag(
              mFxRWindow.mTransportControlsOverlayHandle,
              vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, true);

          if (overlayError == vr::VROverlayError_None) {
            overlayError = vr::VROverlay()->SetOverlayInputMethod(
                mFxRWindow.mTransportControlsOverlayHandle,
                vr::VROverlayInputMethod_Mouse);

            if (overlayError == vr::VROverlayError_None) {
              // Finally, show the prepared overlay
              overlayError = vr::VROverlay()->ShowOverlay(
                  mFxRWindow.mTransportControlsOverlayHandle);
              MOZ_ASSERT(overlayError == vr::VROverlayError_None);

              if (overlayError == vr::VROverlayError_None) {
                // Now, start listening for input...
                overlayError = SetupOverlayInput(
                    mFxRWindow.mTransportControlsOverlayHandle);
              }
            }
          }
        }
      }
    }
  }

  if (overlayError != vr::VROverlayError_None) {
    vr::VROverlayError destroyError = vr::VROverlay()->DestroyOverlay(
        mFxRWindow.mTransportControlsOverlayHandle);
    MOZ_ASSERT(destroyError == vr::VROverlayError_None);
    mFxRWindow.mTransportControlsOverlayHandle = 0;
  }
  return overlayError;
}

vr::VROverlayError FxRWindowManager::SetupOverlayInput(
    vr::VROverlayHandle_t overlayId) {
  // Enable scrolling for this overlay
  vr::VROverlayError overlayError = vr::VROverlay()->SetOverlayFlag(
      overlayId, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);

  if (overlayError == vr::VROverlayError_None) {
    mIsOverlayPumpActive = true;
    DWORD dwTid = 0;
    mOverlayPumpThread = ::CreateThread(
        nullptr, 0, FxRWindowManager::OverlayInputPump, this, 0, &dwTid);
  }
  return overlayError;
}

// --MOZ_LOG=FxRWindowManager:5
DWORD FxRWindowManager::OverlayInputPump(_In_ LPVOID lpParameter) {
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
    vr::HmdVector2_t vecWindowSize = { 0 };
    vr::EVROverlayError error = vr::VROverlay()->GetOverlayMouseScale(
      fxrWindow.mOverlayHandle, &vecWindowSize);

    MOZ_ASSERT(error == vr::VROverlayError_None);
    fxrWindow.mOverlaySizeRec.right = vecWindowSize.v[0];
    fxrWindow.mOverlaySizeRec.bottom = vecWindowSize.v[1];
  }

  // Acquire CS
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
      case vr::VREvent_ButtonPress:
      case vr::VREvent_ButtonUnpress:
      case vr::VREvent_KeyboardCharInput:
      case vr::VREvent_OverlayFocusChanged: {
        fxrWindow.mEventsVector.emplace_back(vrEvent);
        break;
      }
      default:
        break;
    }
  }

  if (mFxRWindow.mTransportControlsOverlayHandle != 0) {
    while (vr::VROverlay()->PollNextOverlayEvent(
        mFxRWindow.mTransportControlsOverlayHandle, &vrEvent,
        sizeof(vrEvent))) {
      if (vrEvent.eventType == vr::VREvent_MouseButtonDown) {
        ToggleProjectionMode();
        break;
      }
    }
  }
  // Post message to UI thread that new events are waiting
  // TODO: cross-plat notification
  if (initiallyEmpty && !fxrWindow.mEventsVector.empty()) {
    PostMessage(fxrWindow.mHwndWidget, MOZ_WM_OPENVR_EVENT, 0, 0);
  }

  ::LeaveCriticalSection(&fxrWindow.mEventsCritSec);
}

void FxRWindowManager::ToggleProjectionMode() {
  mCurrentProjectionIndex =
      (mCurrentProjectionIndex < FxRSupportedProjectionModes.size() - 1)
          ? mCurrentProjectionIndex + 1
          : 0;

  FxRProjectionMode nextMode =
      FxRSupportedProjectionModes[mCurrentProjectionIndex];
  ChangeProjectionMode(nextMode);
}

// Changes the projection mode to one of the supported projection modes defined
// in FxRProjectionMode. Returns true if changing the projection was successful
vr::VROverlayError FxRWindowManager::ChangeProjectionMode(
    FxRProjectionMode projectionMode) {
  bool isPanorama = false;
  bool isStereoPanorama = false;
  bool isStereo2D = false;
  vr::VROverlayError overlayError = vr::VROverlayError_None;

  switch (projectionMode) {
    case (FxRProjectionMode::VIDEO_PROJECTION_360): {
      isPanorama = true;
      break;
    }
    case (FxRProjectionMode::VIDEO_PROJECTION_360S): {
      isStereoPanorama = true;
      break;
    }
    case (FxRProjectionMode::VIDEO_PROJECTION_3D): {
      isStereo2D = true;
      break;
    }
  }
  if (isPanorama || isStereoPanorama) {
    // For panoramic viewing, we want the overlay closer to the user's eyes to
    // fill the entire FOV
    vr::HmdMatrix34_t transform = {
        1.0f, 0.0f, 0.0f, 0.0f,  // no move in x direction
        0.0f, 1.0f, 0.0f, 0.0f,  // +y to move it up
        0.0f, 0.0f, 1.0f, -1.5f  // -z to move it forward from the origin
    };
    // Keep the content centered at user's head
    overlayError = vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(
        mFxRWindow.mOverlayHandle, vr::k_unTrackedDeviceIndex_Hmd, &transform);
  } else {
    vr::HmdMatrix34_t transform = {
        1.0f, 0.0f, 0.0f, 0.0f,  // no move in x direction
        0.0f, 1.0f, 0.0f, 2.0f,  // +y to move it up
        0.0f, 0.0f, 1.0f, -3.0f  // -z to move it forward from the origin
    };
    if (isStereo2D) {
      // For stereo viewing, we want the overlay further to the user's eyes, as
      // the apparent distance of the resultant 3D image is closer than a 2D
      // image
      vr::HmdMatrix34_t transform = {
          1.0f, 0.0f, 0.0f, 0.0f,  // no move in x direction
          0.0f, 1.0f, 0.0f, 2.0f,  // +y to move it up
          0.0f, 0.0f, 1.0f, -6.0f  // -z to move it forward from the origin
      };
    }
    overlayError = vr::VROverlay()->SetOverlayTransformAbsolute(
        mFxRWindow.mOverlayHandle, vr::TrackingUniverseStanding, &transform);
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

  return overlayError;
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

        if (eventType == vr::VREvent_MouseButtonDown ||
            eventType == vr::VREvent_MouseButtonUp) {
          MOZ_LOG(gFxrWinLog, mozilla::LogLevel::Info,
                  ("VREvent_Mouse_t.button: %u", data.button));
        }

        // Windows' origin is top-left, whereas OpenVR's origin is
        // bottom-left, so transform the y-coordinate.
        fxrWindow.mLastMousePt.x = (LONG)(data.x);
        fxrWindow.mLastMousePt.y =
          fxrWindow.mOverlaySizeRec.bottom - (LONG)(data.y);

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

          window->DispatchMouseEvent(
            eMsg,
            0,                                      // wParam
            POINTTOPOINTS(fxrWindow.mLastMousePt)  // lParam
          );
        }
        else if (eventType == vr::VREvent_MouseButtonUp) {
          // When the 2nd button is released, show the transport controls.
          // TODO: Add a check to see if FullScreen + 360 video is active
          EnsureTransportControls();
        }

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
        MOZ_LOG(gFxrWinLog, mozilla::LogLevel::Info, ("scroll", nullptr));
        vr::VREvent_Scroll_t data = iter->data.scroll;

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

        break;
      }

      case vr::VREvent_KeyboardCharInput: {
        vr::VREvent_Keyboard_t data = iter->data.keyboard;

        size_t inputLength =
            strnlen_s(data.cNewInput, ARRAYSIZE(data.cNewInput));
        wchar_t msgChar = data.cNewInput[0];

        if (inputLength > 1) {
          // The event can contain multi-byte UTF8 characters. Convert them to
          // a single Wide character to send to Gecko
          wchar_t convertedChar[ARRAYSIZE(data.cNewInput)] = {0};
          int convertedReturn = ::MultiByteToWideChar(
              CP_UTF8, MB_ERR_INVALID_CHARS, data.cNewInput, inputLength,
              convertedChar, ARRAYSIZE(convertedChar));

          MOZ_ASSERT(convertedReturn == 1);
          msgChar = convertedChar[0];
        } else {
          MOZ_ASSERT(inputLength == 1);
          if (msgChar == L'\n') {
            // Make new line
            msgChar = VK_RETURN;
          }
        }

        switch (msgChar) {
            // These characters need to be mapped to key presses rather than
            // char so that they map to actions instead.
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
            MSG nativeMsg = mozilla::widget::WinUtils::InitMSG(
              WM_CHAR, msgChar, 0, fxrWindow.mHwndWidget);

            window->ProcessCharMessage(nativeMsg, nullptr);
            break;
          }
        }
      }

      case vr::VREvent_OverlayFocusChanged: {
        // As the Overlay's focus changes, update how Firefox sees the focus
        // state of this window. This is especially important so that text
        // input can get the caret and invoke the virtual keyboard.
        // Note that this also means that the Fx Window for the OpenVR Overlay
        // participates in the same focus management as windows on the
        // desktop, so the overlay can steal focus from a desktop Firefox
        // window and vice-versa.

        vr::VREvent_Overlay_t data = iter->data.overlay;

        bool isFocused = data.overlayHandle == fxrWindow.mOverlayHandle;

        MOZ_LOG(gFxrWinLog, mozilla::LogLevel::Info,
                ("Overlay focus: %s", isFocused ? "true" : "false"));

        window->DispatchFocusToTopLevelWindow(isFocused);
      }
    }
  }

  window->DispatchPendingEvents();
}

void FxRWindowManager::ShowVirtualKeyboard(uint64_t aOverlayId) {
  // Note: bUseMinimalMode set to true so that each char arrives as an event.
  vr::VROverlayError overlayError = vr::VROverlay()->ShowKeyboardForOverlay(
      aOverlayId, vr::k_EGamepadTextInputModeNormal,
      vr::k_EGamepadTextInputLineModeSingleLine,
      "FxR",  // pchDescription,
      100,    // unCharMax,
      "",     // pchExistingText,
      true,   // bUseMinimalMode
      0       // uint64_t uUserValue
  );

  MOZ_ASSERT(overlayError == vr::VROverlayError_None ||
             overlayError == vr::VROverlayError_KeyboardAlreadyInUse);
}

void FxRWindowManager::HideVirtualKeyboard() {
  vr::VROverlay()->HideKeyboard();
}

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

vr::VROverlayError FxRWindowManager::OnFullScreenChange(bool aIsFullScreen) {
  if (aIsFullScreen) {
    // Create the transport controls overlay
    vr::VROverlayError overlayError = CreateTransportControlsOverlay();
    MOZ_ASSERT(overlayError == vr::VROverlayError_None);
    return ChangeProjectionMode(VIDEO_PROJECTION_360);
  } else {
    // Close the transport controls overlay
    vr::VROverlayError overlayError = vr::VROverlay()->DestroyOverlay(
        mFxRWindow.mTransportControlsOverlayHandle);
    MOZ_ASSERT(overlayError == vr::VROverlayError_None);
    mFxRWindow.mTransportControlsOverlayHandle = 0;
    return ChangeProjectionMode(VIDEO_PROJECTION_2D);
  }
}

void FxRWindowManager::ToggleMedia() {
  RefPtr<mozilla::dom::MediaControlService> service =
      mozilla::dom::MediaControlService::GetService();
  mozilla::dom::MediaControlKeysEventSource* source =
      service->GetMediaControlKeysEventSource();
  mozilla::dom::PlaybackState state = source->GetPlaybackState();

  if (state == mozilla::dom::PlaybackState::ePlaying) {
    service->GetMediaControlKeysEventSource()->OnKeyPressed(
        mozilla::dom::MediaControlKeysEvent::ePause);
  } else {
    service->GetMediaControlKeysEventSource()->OnKeyPressed(
        mozilla::dom::MediaControlKeysEvent::ePlay);
  }
}

// Supports modifying via the following arguments:
// - "toggle" - Toggles between playing and pausing current media
void FxRWindowManager::SetPlayMediaState(const nsAString& aState) {
  if (aState == u"toggle") {
    ToggleMedia();
  }
  else {
    MOZ_CRASH("Unsupported Param");
  }
}

// Supports changing projection mode or exiting fullscreen presentation via
// the following arguments:
// - "exit" - Ends the current fullscreen presentation
// - "360" - Maps to xxxx
// - "360-stereo" - Maps to xxxx
// - "3d" - Maps to xxxx
void FxRWindowManager::SetProjectionMode(const nsAString& aMode) {
  MOZ_LOG(gFxrWinLog, mozilla::LogLevel::Info, (
    "FxRWindowManager::SetProjectionMode - %s",
    NS_ConvertUTF16toUTF8(aMode).Data()
    ));
}

void FxRWindowManager::EnsureTransportControls() {
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

    nsPIDOMWindowOuter* newWindowOuter = nsPIDOMWindowOuter::From(newDOMWindow);
    InitWindow(mTransportWindow, newWindowOuter);

    if (CreateOverlayForWindow(mTransportWindow, "Firefox Reality Transport Controls", 1.0f)) {
      nsCOMPtr<nsIWidget> newWidget =
        mozilla::widget::WidgetUtils::DOMWindowToWidget(newWindowOuter);
      newWidget->RequestFxrOutput(mTransportWindow.mOverlayHandle);

      // Set the transform for the overlay position
      vr::HmdMatrix34_t transform = {
        1.0f, 0.0f, 0.0f,  0.0f, // no move in x direction
        0.0f, 1.0f, 0.0f,  0.9f, // +y to move it up
        0.0f, 0.0f, 1.0f, -1.9f  // -z to move it forward from the origin
      };
      vr::VROverlayError overlayError = vr::VROverlay()->SetOverlayTransformAbsolute(
        mTransportWindow.mOverlayHandle,
        vr::TrackingUniverseStanding,
        &transform
      );
      MOZ_ASSERT(overlayError == vr::VROverlayError_None);
    }
  } else {
    vr::VROverlayError overlayError =
        vr::VROverlay()->ShowOverlay(mTransportWindow.mOverlayHandle);
    MOZ_ASSERT(overlayError == vr::VROverlayError_None);
  }
}

void FxRWindowManager::HideTransportControls() {
  MOZ_ASSERT(mTransportWindow.mOverlayHandle != 0);
  MOZ_LOG(gFxrWinLog, mozilla::LogLevel::Info,
          ("FxRWindowManager::HideTransportControls", nullptr));
  vr::VROverlayError overlayError =
      vr::VROverlay()->HideOverlay(mTransportWindow.mOverlayHandle);
  MOZ_ASSERT(overlayError == vr::VROverlayError_None);
}
