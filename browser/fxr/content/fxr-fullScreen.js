/* -*- indent-tabs-mode: nil; js-indent-level: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// This file is loaded into the browser window scope.
/* eslint-env mozilla/browser-window */

// fxr-fullScreen.js is a provisional, stripped-down clone of
//   browser\base\content\browser-fullScreenAndPointerLock.js
// that is adapted for Firefox Reality on Desktop.
// The bug to track its removal is
//   Bug 1587946 - Rationalize the fork of browser-fullScreenAndPointerLock.js

var FullScreen = {
  init() {
    // Called when the Firefox window go into fullscreen.
    addEventListener("fullscreen", this, true);

    if (window.fullScreen) {
      this.toggle();
    }
  },

  toggle() {
    var enterFS = window.fullScreen;
    if (enterFS) {
      document.documentElement.setAttribute("inFullscreen", true);
    } else {
      document.documentElement.removeAttribute("inFullscreen");
    }
  },

  handleEvent(event) {
    if (event.type === "fullscreen") {
      this.toggle();
    }
  },

  enterDomFullscreen(aBrowser, aActor) {
    if (!document.fullscreenElement) {
      return;
    }

    // Get the mozVideoProjection query string if there is one, and use it to set the projection mode for FxR
    // See: https://github.com/MozillaReality/FirefoxReality/wiki/Media-Playback
    // NOTE: Currently "Stereo 180 Left to Right" and "Stereo 180 Top to Bottom" are unsupported
    let currentFullscreenURI = document.fullscreenElement.currentURI;
    if (currentFullscreenURI &&
        currentFullscreenURI.query
    )
    {
      let searchParams = new URLSearchParams(currentFullscreenURI.query);

      const mozVideoProjectionKey = "mozVideoProjection";
      if (searchParams.has(mozVideoProjectionKey))
      {
        let projectionMode = searchParams.get(mozVideoProjectionKey);
        switch (projectionMode) {
          case "360_auto":
            ChromeUtils.setFxrProjectionMode("360");
            break;

          case "360s_auto":
            ChromeUtils.setFxrProjectionMode("360-stereo");
            break;

          case "3d_auto":
            ChromeUtils.setFxrProjectionMode("3d");
            break;

          default:
            ChromeUtils.setFxrProjectionMode("2d");
            break;
        }
      }
    }

    // If it is a remote browser, send a message to ask the content
    // to enter fullscreen state. We don't need to do so if it is an
    // in-process browser, since all related document should have
    // entered fullscreen state at this point.
    // This should be done before the active tab check below to ensure
    // that the content document handles the pending request. Doing so
    // before the check is fine since we also check the activeness of
    // the requesting document in content-side handling code.
    if (aBrowser.isRemoteBrowser) {
      aActor.sendAsyncMessage("DOMFullscreen:Entered", {});
    }

    document.documentElement.setAttribute("inDOMFullscreen", true);
  },

  cleanupDomFullscreen(aActor) {
    aActor.sendAsyncMessage("DOMFullscreen:CleanUp", {});
    document.documentElement.removeAttribute("inDOMFullscreen");
  },
};
