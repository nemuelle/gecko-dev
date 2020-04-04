/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "FxRWindowManager.h"

#include "mozilla/Assertions.h"
#include "mozilla/ClearOnShutdown.h"
#include "base/platform_thread.h"

#include "nsPIDOMWindow.h"
#include "nsWindow.h"
#include "nsIDOMWindowUtils.h"
#include "WinMouseScrollHandler.h"

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
  mFxRWindow({ 0 })
{ }

FxRWindowManager::~FxRWindowManager() {
  MOZ_ASSERT(mFxRWindow.mOverlayHandle == 0);
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
    }
  }

  return (eError == vr::VRInitError_None);
}

// Track this new Firefox Reality window instance
bool FxRWindowManager::AddWindow(nsPIDOMWindowOuter* aWindow) {
  if (mFxRWindow.mWindow != nullptr) {
    MOZ_CRASH("Only one window is supported");
  }

  mFxRWindow.mWindow = aWindow;

  // This full reference is released when the window is removed
  mFxRWindow.mWidget =
    mozilla::widget::WidgetUtils::DOMWindowToWidget(mFxRWindow.mWindow).take();

  mFxRWindow.mHwndWidget = (HWND)mFxRWindow.mWidget->GetNativeData(NS_NATIVE_WINDOW);

  ::InitializeCriticalSection(&mFxRWindow.mEventsCritSec);

  return CreateOverlayForWindow();
}

void FxRWindowManager::RemoveWindow(uint64_t aOverlayId) {
  if (aOverlayId != 0 && aOverlayId != mFxRWindow.mOverlayHandle) {
    return;
  }
  MOZ_ASSERT(mFxRWindow.mOverlayHandle != 0);

  if (mIsOverlayPumpActive) {
    // Wait for input thread to return
    mIsOverlayPumpActive = false;
    ::WaitForSingleObject(mOverlayPumpThread, 20*1000);
    
    ::CloseHandle(mOverlayPumpThread);
    mOverlayPumpThread = nullptr;
  }

  vr::VROverlayError overlayError = vr::VROverlay()->DestroyOverlay(
    mFxRWindow.mOverlayHandle
  );
  MOZ_ASSERT(overlayError == vr::VROverlayError_None);

  ::DeleteCriticalSection(&mFxRWindow.mEventsCritSec);

  mFxRWindow.mWidget->Release();

  // Now, clear the state so that another window can be created later
  mFxRWindow = { 0 };
}

void FxRWindowManager::SetRenderPid(uint64_t aOverlayId, uint32_t aPid) {
  if (aOverlayId != mFxRWindow.mOverlayHandle) {
    MOZ_CRASH("Unexpected Overlay ID");
  }

  vr::VROverlayError error = vr::VROverlay()->SetOverlayRenderingPid(
    mFxRWindow.mOverlayHandle,
    aPid
  );
  MOZ_ASSERT(error == vr::VROverlayError_None);
}

bool FxRWindowManager::CreateOverlayForWindow() {
  std::string sKey = std::string("Firefox Reality");
  vr::VROverlayError overlayError = vr::VROverlay()->CreateOverlay(
    sKey.c_str(),
    sKey.c_str(),
    &mFxRWindow.mOverlayHandle
  );

  if (overlayError == vr::VROverlayError_None) {
    // Start with default width of 1.5m
    overlayError = vr::VROverlay()->SetOverlayWidthInMeters(
      mFxRWindow.mOverlayHandle,
      4.0f
    );

    if (overlayError == vr::VROverlayError_None) {
      // Set the transform for the overlay position
      vr::HmdMatrix34_t transform = {
        1.0f, 0.0f, 0.0f,  0.0f, // no move in x direction
        0.0f, 1.0f, 0.0f,  2.0f, // +y to move it up
        0.0f, 0.0f, 1.0f, -2.0f  // -z to move it forward from the origin
      };
      overlayError = vr::VROverlay()->SetOverlayTransformAbsolute(
        mFxRWindow.mOverlayHandle,
        vr::TrackingUniverseStanding,
        &transform
      );

      if (overlayError == vr::VROverlayError_None) {
        overlayError = vr::VROverlay()->SetOverlayFlag(
          mFxRWindow.mOverlayHandle,
          vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible,
          true);

        if (overlayError == vr::VROverlayError_None) {
          overlayError = vr::VROverlay()->SetOverlayInputMethod(
            mFxRWindow.mOverlayHandle,
            vr::VROverlayInputMethod_Mouse
          );

          if (overlayError == vr::VROverlayError_None) {
            // Finally, show the prepared overlay
            overlayError = vr::VROverlay()->ShowOverlay(
              mFxRWindow.mOverlayHandle
            );
            MOZ_ASSERT(overlayError == vr::VROverlayError_None);

            if (overlayError == vr::VROverlayError_None) {
              // Now, start listening for input...
              overlayError = SetupOverlayInput(mFxRWindow.mOverlayHandle);
            }
          }
        }
      }
    }
  }

  if (overlayError != vr::VROverlayError_None) {
    RemoveWindow(mFxRWindow.mOverlayHandle);
    return false;
  }
  else {
    return true;
  }
}

vr::VROverlayError FxRWindowManager::SetupOverlayInput(vr::VROverlayHandle_t overlayId) {
  // Enable scrolling for this overlay
  vr::VROverlayError overlayError = vr::VROverlay()->SetOverlayFlag(
    overlayId,
    vr::VROverlayFlags_SendVRDiscreteScrollEvents,
    true
  );

  if (overlayError == vr::VROverlayError_None) {
    mIsOverlayPumpActive = true;
    DWORD dwTid = 0;
    mOverlayPumpThread = ::CreateThread(
      nullptr, 0,
      FxRWindowManager::OverlayInputPump,
      this, 0, &dwTid
    );
  }
  return overlayError;
}


// --MOZ_LOG=FxRWindowManager:5
DWORD FxRWindowManager::OverlayInputPump(_In_ LPVOID lpParameter) {
  PlatformThread::SetName("OpenVR Overlay Input");

  FxRWindowManager* manager = static_cast<FxRWindowManager*>(lpParameter);
  
  MOZ_LOG(gFxrWinLog, mozilla::LogLevel::Info, (
    "FxRWindowManager:OverlayInputPump started (%llX)",
    manager
  ));

  while (manager->mIsOverlayPumpActive) {
    manager->CollectOverlayEvents();
    // Yield the thread
    ::Sleep(0);
  }

  MOZ_LOG(gFxrWinLog, mozilla::LogLevel::Info, (
    "FxRWindowManager:OverlayInputPump exited (%llX)",
    manager
  ));

  return 0;
}

// Runs on background thread because input events from OpenVR are retrieved
// through polling, which makes blocking the thread inevitable. This would be
// bad on UI thread. Since UI widget messages cannot be dispatched to nsWindow
// from another thread (need to confirm), collect OpenVR input events in
// m_rgEvents on this background thread and post a message (MOZ_WM_OPENVR_EVENT)
// that events are ready for UI thread/widget.
void FxRWindowManager::CollectOverlayEvents() {
  // Need to find a better place to put this. The problem that needs to be
  // solved is knowing the texture size so that mouse coords can be translated
  // late. This is put in this function because it won't block the UI thread.
  // .right is compared to <= 1 because
  // - if == 0, then uninitialized
  // - if == 1, then mousescale hasn't been set by GPU process yet (default
  // normalizes to 1.0f)
  if (mFxRWindow.mOverlaySizeRec.right <= 1) {
    vr::HmdVector2_t vecWindowSize = { 0 };
    vr::EVROverlayError error = vr::VROverlay()->GetOverlayMouseScale(
      mFxRWindow.mOverlayHandle, &vecWindowSize);

    MOZ_ASSERT(error == vr::VROverlayError_None);
    mFxRWindow.mOverlaySizeRec.right = vecWindowSize.v[0];
    mFxRWindow.mOverlaySizeRec.bottom = vecWindowSize.v[1];
  }

  // Acquire CS
  ::EnterCriticalSection(&mFxRWindow.mEventsCritSec);

  bool initiallyEmpty = mFxRWindow.mEventsVector.empty();

  // Add events to vector
  vr::VREvent_t vrEvent;
  while (vr::VROverlay()->PollNextOverlayEvent(mFxRWindow.mOverlayHandle, &vrEvent,
    sizeof(vrEvent))) {

    if (vrEvent.eventType != vr::VREvent_MouseMove) {
      MOZ_LOG(gFxrWinLog, mozilla::LogLevel::Info, (
        "VREvent_t.eventType: %s",
        vr::VRSystem()->GetEventTypeNameFromEnum((vr::EVREventType)(vrEvent.eventType))
        ));
    }

    switch (vrEvent.eventType) {
      case vr::VREvent_ScrollDiscrete:
      case vr::VREvent_MouseMove:
      case vr::VREvent_MouseButtonUp:
      case vr::VREvent_MouseButtonDown:
      case vr::VREvent_KeyboardCharInput:
      case vr::VREvent_OverlayFocusChanged: {
        mFxRWindow.mEventsVector.emplace_back(vrEvent);
        break;
      }
      default:
        break;
    }
  }

  // Post message to UI thread that new events are waiting
  // TODO: cross-plat notification
  if (initiallyEmpty && !mFxRWindow.mEventsVector.empty()) {
    PostMessage(mFxRWindow.mHwndWidget, MOZ_WM_OPENVR_EVENT, 0, 0);
  }

  ::LeaveCriticalSection(&mFxRWindow.mEventsCritSec);
}

// Runs on UI thread (for reasons explained with CollectOverlayEvents).
// Copies OpenVR events that were collected on background thread and converts
// them to UI events to be dispatched by the widget.
void FxRWindowManager::ProcessOverlayEvents() {
  MOZ_LOG(gFxrWinLog, mozilla::LogLevel::Info, (
    "Processing Overlay Input Events"
    ));

  VREventVector rgEvents;
  // See note above SynthesizeNativeMouseScrollEvent for reasoning
  bool hasScrolled = false;
  
  // Acquire CS
  ::EnterCriticalSection(&mFxRWindow.mEventsCritSec);

  // Copy elements to stack vector to minimize CritSec acquisition
  mFxRWindow.mEventsVector.swap(rgEvents);

  // Clear vector
  mFxRWindow.mEventsVector.clear();

  ::LeaveCriticalSection(&mFxRWindow.mEventsCritSec);
  
  // Assert size > 0
  MOZ_ASSERT(!rgEvents.empty());

  // TODO: Confirm this is a supported way to get the nsWindow from the widget
  nsWindow* window = static_cast<nsWindow*>(mFxRWindow.mWidget);

  // Process events
  for (auto iter = rgEvents.begin(); iter != rgEvents.end(); iter++) {
    uint32_t eventType = iter->eventType;

    switch (eventType) {
      case vr::VREvent_MouseMove:
      case vr::VREvent_MouseButtonUp:
      case vr::VREvent_MouseButtonDown: {
        vr::VREvent_Mouse_t data = iter->data.mouse;

        // Windows' origin is top-left, whereas OpenVR's origin is
        // bottom-left, so transform the y-coordinate.
        mFxRWindow.mLastMousePt.x = (LONG)(data.x);
        mFxRWindow.mLastMousePt.y =
          mFxRWindow.mOverlaySizeRec.bottom - (LONG)(data.y);

        mozilla::EventMessage eMsg;
        if (eventType == vr::VREvent_MouseMove) {
          eMsg = mozilla::EventMessage::eMouseMove;
        }
        else if (eventType == vr::VREvent_MouseButtonDown) {
          eMsg = mozilla::EventMessage::eMouseDown;
        }
        else {
          MOZ_ASSERT(eventType == vr::VREvent_MouseButtonUp);
          eMsg = mozilla::EventMessage::eMouseUp;
        }

        window->DispatchMouseEvent(
          eMsg,
          0,                                      // wParam
          POINTTOPOINTS(mFxRWindow.mLastMousePt)  // lParam
        );

        break;
      }

      case vr::VREvent_ScrollDiscrete: {
        MOZ_LOG(gFxrWinLog, mozilla::LogLevel::Info, (
          "scroll", nullptr
          ));
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
          pt.x = mFxRWindow.mLastMousePt.x;
          pt.y = mFxRWindow.mLastMousePt.y;

          mozilla::widget::MouseScrollHandler::SynthesizeNativeMouseScrollEvent(
            window, pt, WM_MOUSEWHEEL, scrollDelta,
            0,  // aModifierFlags
            nsIDOMWindowUtils::MOUSESCROLL_SEND_TO_WIDGET | 
            nsIDOMWindowUtils::MOUSESCROLL_POINT_IN_WINDOW_COORD
          );

          hasScrolled = true;
        }

        break;
      }

      case vr::VREvent_KeyboardCharInput: {
        vr::VREvent_Keyboard_t data = iter->data.keyboard;

        size_t inputLength = strnlen_s(data.cNewInput, ARRAYSIZE(data.cNewInput));
        wchar_t msgChar = data.cNewInput[0];

        if (inputLength > 1) {
          // The event can contain multi-byte UTF8 characters. Convert them to
          // a single Wide character to send to Gecko
          wchar_t convertedChar[ARRAYSIZE(data.cNewInput)] = { 0 };
          int convertedReturn = ::MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            data.cNewInput,
            inputLength,
            convertedChar,
            ARRAYSIZE(convertedChar)
          );

          MOZ_ASSERT(convertedReturn == 1);
          msgChar = convertedChar[0];
        }
        else {
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
              WM_KEYDOWN, msgChar, 0, mFxRWindow.mHwndWidget);
            window->ProcessKeyDownMessage(nativeMsgDown, nullptr);

            MSG nativeMsgUp = mozilla::widget::WinUtils::InitMSG(
              WM_KEYUP, msgChar, 0, mFxRWindow.mHwndWidget);
            window->ProcessKeyUpMessage(nativeMsgUp, nullptr);

            break;
          }

          default: {
            MSG nativeMsg = mozilla::widget::WinUtils::InitMSG(
              WM_CHAR, msgChar, 0, mFxRWindow.mHwndWidget);

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

        bool isFocused = data.overlayHandle == mFxRWindow.mOverlayHandle;

        MOZ_LOG(gFxrWinLog, mozilla::LogLevel::Info, (
          "Overlay focus: %s", isFocused ? "true" : "false"
          ));

        window->DispatchFocusToTopLevelWindow(isFocused);
      }
    }
  }

  window->DispatchPendingEvents();
}

void FxRWindowManager::ShowVirtualKeyboard(uint64_t aOverlayId) {
  // Note: bUseMinimalMode set to true so that each char arrives as an event.
  vr::VROverlayError overlayError = vr::VROverlay()->ShowKeyboardForOverlay(
    aOverlayId,
    vr::k_EGamepadTextInputModeNormal,
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

uint64_t FxRWindowManager::GetOverlayId() const {
  return mFxRWindow.mOverlayHandle;
}

// Returns true if the window at the provided ID was created for Firefox Reality
bool FxRWindowManager::IsFxRWindow(uint64_t aOuterWindowID) {
  return (mFxRWindow.mWindow != nullptr) && (mFxRWindow.mWindow->WindowID() == aOuterWindowID);
}

// Returns true if the window was created for Firefox Reality
bool FxRWindowManager::IsFxRWindow(const nsWindow* aWindow) const {
  return (mFxRWindow.mWindow != nullptr) &&
         (aWindow ==
          mozilla::widget::WidgetUtils::DOMWindowToWidget(mFxRWindow.mWindow).take());
}

uint64_t FxRWindowManager::GetWindowID() const {
  MOZ_ASSERT(mFxRWindow.mWindow);
  return mFxRWindow.mWindow->WindowID();
}

// Handle when WebVR/XR content is showing or not, so that both the FxR Overlay
// and the Fx immersive scene do not render at the same time
void FxRWindowManager::OnWebXRPresentationChange(
  uint64_t aOuterWindowID, bool isPresenting) {
  if (IsFxRWindow(aOuterWindowID)) {
    vr::VROverlayError overlayError;
    if (isPresenting) {
      overlayError = vr::VROverlay()->HideOverlay(mFxRWindow.mOverlayHandle);
    }
    else {
      overlayError = vr::VROverlay()->ShowOverlay(mFxRWindow.mOverlayHandle);
    }

    MOZ_ASSERT(overlayError == vr::VROverlayError_None);
  }
}