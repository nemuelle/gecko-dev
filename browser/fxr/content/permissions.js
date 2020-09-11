/* -*- indent-tabs-mode: nil; js-indent-level: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* import-globals-from fxrui.js */

/**
 * Code to manage Permissions UI
 *
 * FxR on Desktop only supports granting permission for
 * - Location
 * - Camera
 * - Microphone
 * Any other permissions are automatically denied.
 *
 */

// Base class for managing permissions in FxR on PC
class FxrPermissionPromptPrototype {
  constructor(aRequest, aBrowser, aCallback) {
    this.request = aRequest;
    this.targetBrowser = aBrowser;
    this.responseCallback = aCallback;
    this.promptTypes = null;
    this.ui = null;
  }

  showPrompt() {
    // For now, all permissions default to denied. Testing for allow must be
    // done manually until UI is finished:
    // Bug 1594840 - Add UI for Web Permissions in FxR for Desktop
    this.defaultDeny();
  }

  defaultDeny() {
    this.handleResponse(false);
  }

  handleResponse(allowed) {
    if (allowed) {
      this.allow();
    } else {
      this.deny();
    }

    if (this.ui) {
      this.ui.close();
      this.ui = null;
    }

    let status = {};
    status[allowed ? "allowed" : "denied"] = this.promptTypes;
    this.responseCallback(status);
  }
}

// WebRTC-specific class implementation
class FxrWebRTCPrompt extends FxrPermissionPromptPrototype {
  constructor(aRequest, aBrowser, aCallback) {
    super(aRequest, aBrowser, aCallback);

    let { audioDevices, videoDevices } = this.request;

    let principal = Services.scriptSecurityManager.createContentPrincipalFromOrigin(
      this.request.origin
    );

    // For now, collect the first audio and video device by default. User
    // selection will be enabled later:
    // Bug 1594841 - Add UI to select device for WebRTC in FxR for Desktop
    this.promptedDevices = [];

    if (audioDevices.length) {
      this.promptedDevices.push(audioDevices[0]);
    }

    if (videoDevices.length) {
      Services.perms.addFromPrincipal(
        principal,
        "MediaManagerVideo",
        Services.perms.ALLOW_ACTION,
        Services.perms.EXPIRE_SESSION
      );
      this.promptedDevices.push(videoDevices[0]);
    }
  }

  showPrompt() {
    for (let typeName of this.request.requestTypes) {
      if (typeName === "Microphone") {
        // Only Microphone requests are allowed. Automatically deny any other
        // request.
        this.promptTypes = this.request.requestTypes;
        this.ui = new FxrPermissionUI(
          this,
          this.promptTypes,
          this.promptedDevices,
          this.request.origin
          );
        
        return;
      }
    }

    super.showPrompt();
  }

  allow() {
    // WebRTCChild doesn't currently care which actor
    // this is sent to and just uses the windowID.
    let deviceIndices = [];
    for (let i = 0; i < this.promptedDevices.length; i++) {
      deviceIndices.push(this.promptedDevices[i].deviceIndex);
    }


    this.targetBrowser.sendMessageToActor(
      "webrtc:Allow",
      {
        callID: this.request.callID,
        windowID: this.request.windowID,
        devices: deviceIndices,
      },
      "WebRTC"
    );
  }

  deny() {
    this.targetBrowser.sendMessageToActor(
      "webrtc:Deny",
      {
        callID: this.request.callID,
        windowID: this.request.windowID,
      },
      "WebRTC"
    );
  }
}

// Implementation for other, non-WebRTC web permission prompts
class FxrContentPrompt extends FxrPermissionPromptPrototype {
  showPrompt() {
    // Only allow exactly one permission request here.
    let types = this.request.types.QueryInterface(Ci.nsIArray);
    if (types.length != 1) {
      this.defaultDeny();
      return;
    }

    // Only Location is supported from this type of request
    let type = types.queryElementAt(0, Ci.nsIContentPermissionType).type;
    if (type !== "geolocation") {
      this.defaultDeny();
      return;
    }

    // Override type so that it can be more easily interpreted by the code
    // for the prompt.
    type = "Location";
    super.showPrompt();
  }

  allow() {
    this.request.allow();
  }

  deny() {
    this.request.cancel();
  }
}


// This class displays the UI for the user make a permission choice.
class FxrPermissionUI {
  constructor(prompt, promptTypes, promptDevices, promptDomain) {
    this.types = promptTypes;
    this.domain = promptDomain;
    this.prompt = prompt;


    this.names = promptDevices[0].name;

    this.init();
  }

  init() {
    this.permsUI = document.createXULElement("browser");
    this.permsUI.setAttribute("type", "chrome");
    this.permsUI.classList.add("permission_prompt");

    showModalContainer(this.permsUI);

    this.permsUI.contentWindow.addEventListener(
      "DOMContentLoaded",
      () => {
        this.browserDoc = this.permsUI.contentWindow.document;
        this.setupType();
        this.setupButtons();
      },
      { once: true }
    );

    this.permsUI.loadURI("chrome://fxr/content/permissions.html", {
      triggeringPrincipal: gSystemPrincipal,
    });
  }

  close() {
    clearModalContainer();
  }

  setupType() {
    let typeContainer = this.browserDoc.getElementById("eTypeIconContainer");
    let typeText = "";

    for (let typeName of this.types) {
      let img = this.browserDoc.createElement("img");
      img.classList.add("type_icon");
      img.src = "assets/icon-" + typeName.toLowerCase() + ".svg";
      img.setAttribute("role", "description");
      typeContainer.appendChild(img);

      if (typeText === "") {
        typeText = typeName;
      } else {
        typeText += " and " + typeName;
      }
    }

    this.browserDoc.getElementById("eTypeTitle").textContent = typeText;
    this.browserDoc.getElementById("eDeviceNames").textContent = " (" + this.names + ")";
    this.browserDoc.getElementById(
      "eDevices"
    ).textContent = typeText.toLowerCase();

    this.browserDoc.getElementById("eDomain").textContent = this.domain;
  }

  setupButtons() {
    this.browserDoc
      .getElementById("eDoNotAllow")
      .addEventListener("click", () => this.prompt.handleResponse(false));

    this.browserDoc
      .getElementById("eAllow")
      .addEventListener("click", () => this.prompt.handleResponse(true));
  }
}