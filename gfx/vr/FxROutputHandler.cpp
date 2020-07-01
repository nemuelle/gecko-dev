/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "FxROutputHandler.h"
#include "mozilla/Assertions.h"
#include "VRManager.h"
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
    // Ensure that enumeration starts so that output can be later sent to VR
    // process via VRManager
    mozilla::gfx::VRManager* vr = mozilla::gfx::VRManager::Get();
    if (!vr->IsActive()) {
      vr->EnumerateDevices();
    }

    ID3D11Texture2D* texOrig = nullptr;
    HRESULT hr = aSwapChain->GetBuffer(0, IID_PPV_ARGS(&texOrig));
    if (hr == S_OK) {
      D3D11_TEXTURE2D_DESC desc;
      texOrig->GetDesc(&desc);

      mLastWidth = desc.Width;
      mLastHeight = desc.Height;

      desc.MiscFlags |= D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
      hr = aDevice->CreateTexture2D(&desc, nullptr,
        mTexCopy.StartAssignment());
      if (hr == S_OK) {
        RefPtr<IDXGIResource> texResource;
        hr = mTexCopy->QueryInterface(IID_IDXGIResource,
          getter_AddRefs(texResource));

        if (hr == S_OK) {
          hr = texResource->GetSharedHandle(&mTexCopyShared);
          if (hr == S_OK) {
            // The texture is successfully created and shared, so cache a
            // pointer to the swapchain to indicate this success.
            mSwapChain = aSwapChain;
          }
        }
      }

      if (hr != S_OK) {
        mTexCopy = nullptr;
        mSwapChain = nullptr;
        mTexCopyShared = nullptr;
        return false;
      }

      texOrig->Release();
    }
  } else {
    MOZ_ASSERT(aSwapChain == mSwapChain);
  }

  return mSwapChain != nullptr && aSwapChain == mSwapChain;
}

// Update the OpenVR Overlay's rendering from the swapchain
void FxROutputHandler::UpdateOutput(ID3D11DeviceContext* aCtx) {
  MOZ_ASSERT(mSwapChain != nullptr);

  mozilla::gfx::VRManager* vr = mozilla::gfx::VRManager::Get();
  if (vr->IsActive()) {
    ID3D11Texture2D* texOrig = nullptr;
    HRESULT hr = mSwapChain->GetBuffer(0, IID_PPV_ARGS(&texOrig));
    if (hr == S_OK) {
      mozilla::layers::SurfaceDescriptorD3D10 desc(
        mozilla::WindowsHandle(mTexCopyShared),
        mozilla::gfx::SurfaceFormat::B8G8R8A8,
        mozilla::gfx::IntSize(mLastWidth, mLastHeight),
        mozilla::gfx::YUVColorSpace(),
        mozilla::gfx::ColorRange()
      );

      IDXGIKeyedMutex* mutex = nullptr;
      hr = mTexCopy->QueryInterface(IID_PPV_ARGS(&mutex));
      if (SUCCEEDED(hr)) {
        hr = mutex->AcquireSync(0, 1000);
        if (hr == S_OK) {
          aCtx->CopyResource(mTexCopy, texOrig);
          hr = mutex->ReleaseSync(0);
        }

        mutex->Release();
        mutex = nullptr;
      }

      bool ret = mozilla::gfx::VRManager::Get()->Submit2DFrame(
        desc, ++mFrameId, mOverlayId
      );

      MOZ_ASSERT(ret);
      mozilla::Unused << ret;
    }
  }
}