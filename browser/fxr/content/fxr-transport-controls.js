/* -*- indent-tabs-mode: nil; js-indent-level: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

window.addEventListener(
  "DOMContentLoaded",
  () => {
    setupTransportButtons();
  },
  { once: true }
);


function setupTransportButtons() {
  let aryButtons = [
    "eExitFullScreen",
    "ePlayPause",
    "eProjection360",
    "eProjection360Stereo",
    "eProjection3D",
    "eProjection2D",
  ];

  function transportButtonHandler(e) {
    switch (this.id) {
      case "eExitFullScreen":
        ChromeUtils.setFxrProjectionMode("exit");
        break;
        
      case "ePlayPause":
        ChromeUtils.setFxrPlayMediaState("toggle");        
        break;

      case "eProjection360":
        ChromeUtils.setFxrProjectionMode("360");
        break;
        
      case "eProjection360Stereo":
        ChromeUtils.setFxrProjectionMode("360-stereo");
        break;
          
      case "eProjection3D":
        ChromeUtils.setFxrProjectionMode("3d");
        break;

      case "eProjection2D":
        ChromeUtils.setFxrProjectionMode("2d");
        break;
    }
  }

  for (let btnName of aryButtons) {
    let elem = document.getElementById(btnName);
    elem.addEventListener("click", transportButtonHandler);
  }
}