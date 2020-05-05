/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "FxROutputHandler.h"
#include "mozilla/Assertions.h"
#include "moz_external_vr.h"
#include "VRShMem.h"
#include "openvr.h"


FxROutputHandler::FxROutputHandler(uint64_t aOverlayId)
  : mOverlayId(aOverlayId) {
  if (mOverlayId == 0) {
    MOZ_CRASH("Invalid overlay ID");
  }
}

bool FxROutputHandler::GetSize(uint32_t& aWidth, uint32_t& aHeight) const {
  aWidth = mLastWidth;
  aHeight = mLastHeight;

  return mLastWidth != 0 && mLastHeight != 0;
}

// TryInitialize is responsible for associating this output handler with the
// calling window's swapchain for subsequent updates.
// See nsFxrCommandLineHandler::Handle for more information about the
// bootstrap process.
bool FxROutputHandler::TryInitialize(IDXGISwapChain* aSwapChain,
                                     ID3D11Device* aDevice) {
  if (mSwapChain == nullptr) {
    vr::EVRInitError eError = vr::VRInitError_None;
    if (m_pHMD == nullptr) {
      m_pHMD = vr::VR_Init(&eError, vr::VRApplication_Overlay);
      if (eError == vr::VRInitError_None) {
        // The texture is successfully created and shared, so cache a
        // pointer to the swapchain to indicate this success.
        mSwapChain = aSwapChain;

        ID3D11Texture2D* texOrig = nullptr;
        HRESULT hr = mSwapChain->GetBuffer(0, IID_PPV_ARGS(&texOrig));
        if (hr == S_OK) {
          D3D11_TEXTURE2D_DESC desc;
          texOrig->GetDesc(&desc);

          mLastWidth = desc.Width;
          mLastHeight = desc.Height;

          // In order to properly process mouse events from the controller, set
          // the mouse scale based on the size of the window texture
          vr::HmdVector2_t vecWindowSize = { (float)mLastWidth,
                                             (float)mLastHeight };

          vr::EVROverlayError error = vr::VROverlay()->SetOverlayMouseScale(
            mOverlayId, &vecWindowSize);
          MOZ_ASSERT(error == vr::VROverlayError_None);
          mozilla::Unused << error;

          texOrig->Release();
        }
      }
    }
  } else {
    MOZ_ASSERT(aSwapChain == mSwapChain);
  }

  return mSwapChain != nullptr && aSwapChain == mSwapChain;
}

// Update the OpenVR Overlay's rendering from the swapchain
void FxROutputHandler::UpdateOutput(ID3D11DeviceContext* aCtx) {
  MOZ_ASSERT(mSwapChain != nullptr);

  ID3D11Texture2D* texOrig = nullptr;
  HRESULT hr = mSwapChain->GetBuffer(0, IID_PPV_ARGS(&texOrig));
  if (hr == S_OK) {
    
    vr::Texture_t overlayTextureDX11 = {
      texOrig,
      vr::TextureType_DirectX,
      vr::ColorSpace_Gamma
    };
    
    vr::VROverlayError error = vr::VROverlay()->SetOverlayTexture(
      mOverlayId,
      &overlayTextureDX11
    );
    MOZ_ASSERT(error == vr::VROverlayError_None);
    mozilla::Unused << error;

    texOrig->Release();
  }
}