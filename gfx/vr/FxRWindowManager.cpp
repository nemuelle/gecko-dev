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
  mFxRWindow({ 0 })
{}

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

  return CreateOverlayForWindow();
}

void FxRWindowManager::RemoveWindow(uint64_t aOverlayId) {
  if (aOverlayId != mFxRWindow.m_ulOverlayHandle) {
    return;
  }

  if (mIsOverlayPumpActive) {
    // Wait for input thread to return
    mIsOverlayPumpActive = false;
    ::WaitForSingleObject(mOverlayPumpThread, INFINITE);

    vr::VROverlayError overlayError = vr::VROverlay()->DestroyOverlay(
      mFxRWindow.m_ulOverlayHandle
    );
    MOZ_ASSERT(overlayError == vr::VROverlayError_None);

    // Now, clear the state so that another window can be created later
    mFxRWindow = { 0 };
  }
}

void FxRWindowManager::SetRenderPid(uint64_t aOverlayId, uint32_t aPid) {
  if (aOverlayId != mFxRWindow.m_ulOverlayHandle) {
    MOZ_CRASH("Unexpected Overlay ID");
  }

  vr::VROverlayError error = vr::VROverlay()->SetOverlayRenderingPid(
    mFxRWindow.m_ulOverlayHandle,
    aPid
  );
  MOZ_ASSERT(error == vr::VROverlayError_None);
}

bool FxRWindowManager::CreateOverlayForWindow() {
  std::string sKey = std::string("Firefox Reality");
  vr::VROverlayError overlayError = vr::VROverlay()->CreateOverlay(
    sKey.c_str(),
    sKey.c_str(),
    &mFxRWindow.m_ulOverlayHandle
  );

  if (overlayError == vr::VROverlayError_None) {
    // Start with default width of 1.5m
    overlayError = vr::VROverlay()->SetOverlayWidthInMeters(
      mFxRWindow.m_ulOverlayHandle,
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
        mFxRWindow.m_ulOverlayHandle,
        vr::TrackingUniverseStanding,
        &transform
      );

      if (overlayError == vr::VROverlayError_None) {
        overlayError = vr::VROverlay()->SetOverlayFlag(
          mFxRWindow.m_ulOverlayHandle,
          vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible,
          true);

        if (overlayError == vr::VROverlayError_None) {
          overlayError = vr::VROverlay()->SetOverlayInputMethod(
            mFxRWindow.m_ulOverlayHandle,
            vr::VROverlayInputMethod_Mouse
          );

          if (overlayError == vr::VROverlayError_None) {
            // Finally, show the prepared overlay
            overlayError = vr::VROverlay()->ShowOverlay(
              mFxRWindow.m_ulOverlayHandle
            );
            MOZ_ASSERT(overlayError == vr::VROverlayError_None);

            // Now, start listening for input...
            mIsOverlayPumpActive = true;
            DWORD dwTid = 0;
            mOverlayPumpThread = ::CreateThread(
              nullptr, 0,
              FxRWindowManager::OverlayInputPump,
              this, 0, &dwTid
            );
          }
        }
      }
    }
  }

  if (overlayError != vr::VROverlayError_None) {
    RemoveWindow(mFxRWindow.m_ulOverlayHandle);
    return false;
  }
  else {
    return true;
  }
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
    if (vr::VROverlay() != nullptr) {
      vr::VREvent_t vrEvent;
      while (vr::VROverlay()->PollNextOverlayEvent(manager->mFxRWindow.m_ulOverlayHandle, &vrEvent, sizeof(vrEvent))) {
        MOZ_LOG(gFxrWinLog, mozilla::LogLevel::Info, (
          "VREvent_t.eventType: %s\n",
          vr::VRSystem()->GetEventTypeNameFromEnum((vr::EVREventType)(vrEvent.eventType))
        ));

        switch (vrEvent.eventType) {
          case vr::VREvent_MouseButtonDown:
          case vr::VREvent_MouseButtonUp:
          case vr::VREvent_MouseMove: {
            vr::VREvent_Mouse_t data = vrEvent.data.mouse;
            MOZ_LOG(gFxrWinLog, mozilla::LogLevel::Info, (
              "VREvent_t.data.mouse (%f, %f)",
              data.x, data.y, vrEvent.eventType
              ));
            // Mouse code...

            break;
          }

          case vr::VREvent_KeyboardCharInput: {
            vr::VREvent_Keyboard_t data = vrEvent.data.keyboard;
            MOZ_LOG(gFxrWinLog, mozilla::LogLevel::Info, (
              "  VREvent_t.data.keyboard.cNewInput --%s--",
              data.cNewInput
            ));

            // Keyboard code...

            break;
          }
        }
      }
    }
    // Is there a better way to know when to poll? I really hope I don't have to do this
    ::Sleep(100);
  }

  MOZ_LOG(gFxrWinLog, mozilla::LogLevel::Info, (
    "FxRWindowManager:OverlayInputPump exited (%llX)",
    manager
  ));

  return 0;
}

uint64_t FxRWindowManager::GetOverlayId() const {
  return mFxRWindow.m_ulOverlayHandle;
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