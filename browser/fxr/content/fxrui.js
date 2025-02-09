/* -*- indent-tabs-mode: nil; js-indent-level: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* import-globals-from common.js */
/* import-globals-from permissions.js */

// Configuration vars
let homeURL = "https://webxr.today/?skipinteractive=true";
// Bug 1586294 - Localize the privacy policy URL (Services.urlFormatter?)
let privacyPolicyURL = "https://www.mozilla.org/en-US/privacy/firefox/";
let reportIssueURL = "https://mzl.la/fxrpcbugs";
let licenseURL =
  "https://mixedreality.mozilla.org/FirefoxRealityPC/license.html";

// https://developer.mozilla.org/en-US/docs/Mozilla/Tech/XUL/browser
let browser = null;
// Keep track of the current Permissions request to only allow one outstanding
// request/prompt at a time.
let currentPermissionRequest = null;
// And, keep a queue of pending Permissions requests to resolve when the
// current request finishes
let pendingPermissionRequests = [];
// The following variable map to UI elements whose behavior changes depending
// on some state from the browser control
let urlInput = null;
let secureIcon = null;
let backButton = null;
let forwardButton = null;
let refreshButton = null;
let stopButton = null;

let { Services } = ChromeUtils.import("resource://gre/modules/Services.jsm");
const { PrivateBrowsingUtils } = ChromeUtils.import(
  "resource://gre/modules/PrivateBrowsingUtils.jsm"
);
const { AppConstants } = ChromeUtils.import(
  "resource://gre/modules/AppConstants.jsm"
);
const { XPCOMUtils } = ChromeUtils.import(
  "resource://gre/modules/XPCOMUtils.jsm"
);

// Note: FxR UI uses a fork of browser-fullScreenAndPointerLock.js which removes
// the dependencies on browser.js.
// Bug 1587946 - Rationalize the fork of browser-fullScreenAndPointerLock.js
XPCOMUtils.defineLazyScriptGetter(
  this,
  "FullScreen",
  "chrome://fxr/content/fxr-fullScreen.js"
);
XPCOMUtils.defineLazyGetter(this, "gSystemPrincipal", () =>
  Services.scriptSecurityManager.getSystemPrincipal()
);

window.addEventListener(
  "DOMContentLoaded",
  () => {
    urlInput = document.getElementById("eUrlInput");
    secureIcon = document.getElementById("eUrlSecure");
    backButton = document.getElementById("eBack");
    forwardButton = document.getElementById("eForward");
    refreshButton = document.getElementById("eRefresh");
    stopButton = document.getElementById("eStop");

    setupBrowser();
    setupWinManagementButtons();
    setupNavButtons();
    setupUrlBar();

    // Minimize the window after everything is setup
    window.setTimeout(
      () => { window.requestIdleCallback(() => { window.minimize(); }) },
      3000
    );
  },
  { once: true }
);

// Create XUL browser object
function setupBrowser() {
  // Note: createXULElement is undefined when this page is not loaded
  // via chrome protocol
  if (document.createXULElement) {
    browser = document.createXULElement("browser");
    browser.setAttribute("type", "content");
    browser.setAttribute("remote", "true");
    browser.classList.add("browser_instance");
    document.getElementById("eBrowserContainer").appendChild(browser);

    browser.loadUrlWithSystemPrincipal = function(url) {
      this.loadURI(url, { triggeringPrincipal: gSystemPrincipal });
    };

    // Expose this function for Permissions to be used on this browser element
    // in other parts of the frontend
    browser.fxrPermissionPrompt = permissionPrompt;
    browser.fxrPermissionUpdate = permissionUpdate;

    goHome();

    // With the introduction of BrowsingContextWebProgress, must ensure that
    // there is a reference to this nsIWebProgressListener to keep it alive, as
    // the native code only keeps a weak reference. Pin the reference to an
    // expando on the browser objection to keep their lifetimes together.
    // (see BrowsingContextWebProgress::UpdateAndNotifyListeners)
    browser.pinnedProgressListener = {
      QueryInterface: ChromeUtils.generateQI([
        Ci.nsIWebProgressListener,
        Ci.nsISupportsWeakReference,
      ]),
      onLocationChange(aWebProgress, aRequest, aLocation, aFlags) {
        // When URL changes, update the URL in the URL bar and update
        // whether the back/forward buttons are enabled.
        let newUrl = browser.currentURI.spec;
        if (newUrl !== homeURL) {
          urlInput.value = newUrl;
        }

        backButton.disabled = !browser.canGoBack;
        forwardButton.disabled = !browser.canGoForward;

        if (!(aFlags & Ci.nsIWebProgressListener.LOCATION_CHANGE_SAME_DOCUMENT)) {
          permissionUpdate(null);
        }
      },
      onStateChange(aWebProgress, aRequest, aStateFlags, aStatus) {
        if (aStateFlags & Ci.nsIWebProgressListener.STATE_STOP) {
          // Network requests are complete. Disable (hide) the stop button
          // and enable (show) the refresh button
          refreshButton.disabled = false;
          stopButton.disabled = true;
        } else {
          // Network requests are outstanding. Disable (hide) the refresh
          // button and enable (show) the stop button
          refreshButton.disabled = true;
          stopButton.disabled = false;
        }
      },
      onSecurityChange(aWebProgress, aRequest, aState) {
        // Update the Secure Icon when the security status of the
        // content changes
        if (aState & Ci.nsIWebProgressListener.STATE_IS_SECURE) {
          secureIcon.style.visibility = "visible";
        } else {
          secureIcon.style.visibility = "hidden";
        }
      },
    };

    browser.addProgressListener(
      browser.pinnedProgressListener,
      Ci.nsIWebProgress.NOTIFY_LOCATION |
        Ci.nsIWebProgress.NOTIFY_SECURITY |
        Ci.nsIWebProgress.NOTIFY_ALL
    );

    FullScreen.init();

    // Send this notification to start and allow background scripts for
    // WebExtensions, since this FxR UI doesn't participate in typical
    // startup activities
    Services.obs.notifyObservers(window, "extensions-late-startup");

    // Load this script in the content process to start and allow scripts for
    // WebExtensions that run in the content process
    browser.messageManager.loadFrameScript(
      "chrome://fxr/content/fxr-content.js",
      true // allowDelayedLoad
    );
  }
}

function setupNavButtons() {
  let aryNavButtons = [
    "eBack",
    "eForward",
    "eRefresh",
    "eStop",
    "eHome",
    "ePrefs",
  ];

  function navButtonHandler(e) {
    if (!this.disabled) {
      switch (this.id) {
        case "eBack":
          browser.goBack();
          break;

        case "eForward":
          browser.goForward();
          break;

        case "eRefresh":
          browser.reload();
          break;

        case "eStop":
          browser.stop();
          break;

        case "eHome":
          goHome();
          break;

        case "ePrefs":
          openSettings();
          break;
      }
    }
  }

  for (let btnName of aryNavButtons) {
    let elem = document.getElementById(btnName);
    elem.addEventListener("click", navButtonHandler);
  }
}

function goHome() {
  // Set to empty so that placeholder text will show
  urlInput.value = "";
  browser.loadUrlWithSystemPrincipal(homeURL);
}

function setupUrlBar() {
  // Navigate to new value when the user presses "Enter"
  urlInput.addEventListener("keypress", async function(e) {
    if (e.key == "Enter") {
      // Use the URL Fixup Service in case the user wants to search instead
      // of directly navigating to a location.
      await Services.search.init();

      let valueToFixUp = urlInput.value;
      let flags =
        Services.uriFixup.FIXUP_FLAG_FIX_SCHEME_TYPOS |
        Services.uriFixup.FIXUP_FLAG_ALLOW_KEYWORD_LOOKUP;
      if (PrivateBrowsingUtils.isWindowPrivate(window)) {
        flags |= Services.uriFixup.FIXUP_FLAG_PRIVATE_CONTEXT;
      }
      let { preferredURI } = Services.uriFixup.getFixupURIInfo(
        valueToFixUp,
        flags
      );

      browser.loadUrlWithSystemPrincipal(preferredURI.spec);
      browser.focus();
      ChromeUtils.setFxrKeyboardVisibility(window.docShell.outerWindowID, false);
    }
  });

  // Upon focus, highlight the whole URL if not already highlighted. This
  // allows for all text to be replaced when typing. Otherwise, if it is
  // already highlighted, then allow the cursor to be positioned where
  // the user clicked.
  urlInput.addEventListener("focus", function() {
    if ((urlInput.selectionEnd - urlInput.selectionStart)
          !== urlInput.value.length) {
      urlInput.select();
    }
  });
}

//
// Code for Overlay Window Management
//

function setupWinManagementButtons() {
  let aryWinMgmtButtons = [
    "eCloseWindow",
    "eFocusWindow",
    "eResizeWindow",
    "eResize3x",
    "eResize2x",
    "eResize1x",
    "eResizeHalf",
    "eResizeDone",
  ];

  function winMgmtHandler(e) {
    if (!this.disabled) {
      switch (this.id) {
        case "eCloseWindow":
          window.close();
          break;

        case "eFocusWindow":
          ChromeUtils.toggleFxrWindowFocus(window.docShell.outerWindowID);
          break;

        case "eResizeWindow":
          displayResizeContainer();
          break;

        case "eResizeDone":
          displayResizeContainer(false);
          break;

        case "eResize3x":
          doOverlayResize(3.0);
          break;

        case "eResize2x":
          doOverlayResize(2.0);
          break;

        case "eResize1x":
          doOverlayResize(1.0);
          break;

        case "eResizeHalf":
          doOverlayResize(0.5);
          break;
      }
    }
  }

  for (let btnName of aryWinMgmtButtons) {
    let elem = document.getElementById(btnName);
    elem.addEventListener("click", winMgmtHandler);
  }

  // Window move is handled separately, as it us a "drag" operation that is
  // initiated when pressing down on the button.
  // The 'mouseup' is handled elsewhere,
  // - natively, by listening for the controller button release to stop moving
  //   the window (xul!FxRWindowManager::HandleOverlayMove)
  // - in JS, when the button goes up on the modal icon (doOverlayMove)
  document.getElementById("eMoveWindow").addEventListener(
    "mousedown",
    () => { doOverlayMove(true); }
  );
}

function displayResizeContainer(show) {
  var list = document.getElementById("eResizeContainer").classList;
  if (show === undefined) {
    list.toggle("container_hidden");
  } else {
    list.toggle("container_hidden", !show);
  }
}

function doOverlayResize(scale) {
  ChromeUtils.setFxrSizeScale(window.docShell.outerWindowID, scale);
}

function doOverlayMove(enable) {
  if (enable) {
    var d = document.createElement("img");
    d.src = "assets/icon-move.svg";
    d.classList.add("icon_modal");
    d.addEventListener(
      "mouseup",
      () => { doOverlayMove(false) },
      { "once" : true }
    );

    d.addEventListener(
      "mousedown",
      () => { ChromeUtils.setFxrMoveOverlay(
        window.docShell.outerWindowID, enable); },
      { "once" : true }
    );

    showModalContainer(d);
  } else {
    clearModalContainer();
  }

  ChromeUtils.setFxrMoveOverlay(window.docShell.outerWindowID, enable);
}

//
// Code to manage Settings UI
//

function openSettings() {
  let browserSettingsUI = document.createXULElement("browser");
  browserSettingsUI.setAttribute("type", "chrome");
  browserSettingsUI.classList.add("browser_settings");

  showModalContainer(browserSettingsUI);

  browserSettingsUI.loadURI("chrome://fxr/content/prefs.html", {
    triggeringPrincipal: gSystemPrincipal,
  });
}

function closeSettings() {
  clearModalContainer();
}

function showPrivacyPolicy() {
  closeSettings();
  browser.loadUrlWithSystemPrincipal(privacyPolicyURL);
}

function showLicenseInfo() {
  closeSettings();
  browser.loadUrlWithSystemPrincipal(licenseURL);
}

function showReportIssue() {
  closeSettings();
  browser.loadUrlWithSystemPrincipal(reportIssueURL);
}

//
// Code to manage Permissions UI
//

// Called as a response to a permission request from the web platform
function permissionPrompt(aRequest) {
  let newPrompt;
  if (aRequest instanceof Ci.nsIContentPermissionRequest) {
    newPrompt = new FxrContentPrompt(aRequest, this, finishPermissionPrompt);
  } else {
    newPrompt = new FxrWebRTCPrompt(aRequest, this, finishPermissionPrompt);
  }

  if (currentPermissionRequest) {
    // There is already an outstanding request running. Cache this new request
    // to be prompted later
    pendingPermissionRequests.push(newPrompt);
  } else {
    currentPermissionRequest = newPrompt;
    currentPermissionRequest.showPrompt();
  }
}

// Callback when a prompt completes.
function finishPermissionPrompt(status) {
  if (pendingPermissionRequests.length) {
    // Prompt the next request
    currentPermissionRequest = pendingPermissionRequests.shift();
    currentPermissionRequest.showPrompt();
  } else {
    currentPermissionRequest = null;
  }
}

// Called when navigating to another page (aState == null) or when an event
// fires that the UI should be updated
function permissionUpdate(aState) {  
  // For now, only supporting microphone
  let icon = document.getElementById("ePermsMicrophone");

  // First, remove any classes that make the icon visible
  icon.classList.remove("urlbar_device_microphone_allowed");
  icon.classList.remove("urlbar_device_microphone_blocked");

  // Next, determine if the icon should be visible and how
  if (aState?.sharing === "microphone") {
    // The microphone is in use, so show the icon in the navbar
    icon.classList.toggle("urlbar_device_microphone_allowed", true)
  } else {
    let perm = SitePermissions.getForPrincipal(browser.contentPrincipal, "microphone", browser);    
    
    if (perm && perm.state === SitePermissions.BLOCK) {
      // The microphone has been denied, so indicate that in the navbar
      icon.classList.toggle("urlbar_device_microphone_blocked", true);
    }
  }

  // If no state has been found above, then no icon is shown to indicate that
  // no permission has been prompted or that permission prompt is possible
}
