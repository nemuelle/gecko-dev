/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "FxRWindowManager.h"
#include "mozilla/Assertions.h"
#include "nsPIDOMWindow.h"
#include "mozilla/ClearOnShutdown.h"

#include "nsWindow.h"

static mozilla::StaticAutoPtr<FxRWindowManager> sFxrWinMgrInstance;

FxRWindowManager* FxRWindowManager::GetInstance() {
  if (sFxrWinMgrInstance == nullptr) {
    sFxrWinMgrInstance = new FxRWindowManager();
    ClearOnShutdown(&sFxrWinMgrInstance);
  }

  return sFxrWinMgrInstance;
}

FxRWindowManager::FxRWindowManager() :
  m_pHMD(nullptr),
  m_dxgiAdapterIndex(-1),
  mFxRWindow({ 0 })
{}

// Initialize an instance of OpenVR for the window manager
bool FxRWindowManager::VRinit() {
  vr::EVRInitError eError = vr::VRInitError_None;
  if (m_pHMD == nullptr) {
    m_pHMD = vr::VR_Init(&eError, vr::VRApplication_Overlay);
    if (eError == vr::VRInitError_None)
    {
      m_pHMD->GetDXGIOutputInfo(&m_dxgiAdapterIndex);
      MOZ_ASSERT(m_dxgiAdapterIndex != -1);
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
      1.5f
    );

    if (overlayError == vr::VROverlayError_None) {
      // Set the transform for the overlay position
      vr::HmdMatrix34_t transform = {
        1.0f, 0.0f, 0.0f,  0.0f, // no move in x direction
        0.0f, 1.0f, 0.0f,  3.0f, // +y to move it up
        0.0f, 0.0f, 1.0f, -3.0f  // -z to move it forward from the origin
      };
      overlayError = vr::VROverlay()->SetOverlayTransformAbsolute(
        mFxRWindow.m_ulOverlayHandle,
        vr::TrackingUniverseStanding,
        &transform
      );

      if (overlayError == vr::VROverlayError_None) {
        // For now, set the overlay to a system image. This will be replaced by
        // the Window later.
        overlayError = vr::VROverlay()->SetOverlayFromFile(
          mFxRWindow.m_ulOverlayHandle,
          "C:\\Windows\\System32\\SecurityAndMaintenance_Alert.png"
        );

        if (overlayError == vr::VROverlayError_None) {
          // Finally, show the prepared overlay
          overlayError = vr::VROverlay()->ShowOverlay(mFxRWindow.m_ulOverlayHandle);
        }
      }
    }
  }

  if (overlayError != vr::VROverlayError_None) {
    overlayError = vr::VROverlay()->DestroyOverlay(mFxRWindow.m_ulOverlayHandle);
    MOZ_ASSERT(overlayError == vr::VROverlayError_None);

    mFxRWindow = { 0 };

    return false;
  }
  else {
    return true;
  }
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