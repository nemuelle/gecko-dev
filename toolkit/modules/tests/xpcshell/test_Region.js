"use strict";

const { AppConstants } = ChromeUtils.import(
  "resource://gre/modules/AppConstants.jsm"
);
const { HttpServer } = ChromeUtils.import("resource://testing-common/httpd.js");
const { Region } = ChromeUtils.import("resource://gre/modules/Region.jsm");
const { setTimeout } = ChromeUtils.import("resource://gre/modules/Timer.jsm");
const { TestUtils } = ChromeUtils.import(
  "resource://testing-common/TestUtils.jsm"
);

const REGION_PREF = "browser.region.network.url";
const INTERVAL_PREF = "browser.region.update.interval";

const RESPONSE_DELAY = 500;
const RESPONSE_TIMEOUT = 100;

const histogram = Services.telemetry.getHistogramById(
  "SEARCH_SERVICE_COUNTRY_FETCH_RESULT"
);

// Add notification observer, it will return a promise that will resolve once a notification is fired.
function waitForNotificationSubject(topic) {
  return new Promise((resolve, reject) => {
    Services.obs.addObserver(function observe(aSubject, aTopic, aData) {
      // wrap the subject as a nsISupports
      let subject = aSubject.QueryInterface(Ci.nsISupportsString);
      Services.obs.removeObserver(observe, topic);
      resolve(subject);
    }, topic);
  });
}

// Region.jsm will call init() on startup and sent a background
// task to fetch the region, ensure we have completed this before
// running the rest of the tests.
add_task(async function test_startup() {
  setNetworkRegion("UK");
  await checkTelemetry(Region.TELEMETRY.SUCCESS);
});

add_task(async function test_basic() {
  Region._home = null;
  let srv = useHttpServer(REGION_PREF);
  srv.registerPathHandler("/", (req, res) => {
    res.setStatusLine("1.1", 200, "OK");
    send(res, { country_code: "UK" });
  });
  // start to listen the notification
  let notificationSubjectPromise = waitForNotificationSubject("browser-region");
  await Region._fetchRegion();
  let notificationSub = await notificationSubjectPromise;

  Assert.ok(true, "Region fetch should succeed");
  Assert.equal(Region.home, "UK", "Region fetch should return correct result");
  Assert.equal(
    notificationSub,
    Region.home,
    "Notification should be sent with the correct region"
  );

  await new Promise(r => srv.stop(r));
});

add_task(async function test_invalid_url() {
  histogram.clear();
  Services.prefs.setIntPref("browser.region.retry-timeout", 0);
  Services.prefs.setCharPref(REGION_PREF, "http://localhost:0");
  let result = await Region._fetchRegion();
  Assert.ok(!result, "Should return no result");
  await checkTelemetry(Region.TELEMETRY.NO_RESULT);
});

add_task(async function test_invalid_json() {
  histogram.clear();
  Services.prefs.setCharPref(
    REGION_PREF,
    'data:application/json,{"country_code"'
  );
  let result = await Region._fetchRegion();
  Assert.ok(!result, "Should return no result");
  await checkTelemetry(Region.TELEMETRY.NO_RESULT);
});

add_task(async function test_timeout() {
  histogram.clear();
  Services.prefs.setIntPref("browser.region.retry-timeout", 0);
  Services.prefs.setIntPref("browser.region.timeout", RESPONSE_TIMEOUT);
  let srv = useHttpServer(REGION_PREF);
  srv.registerPathHandler("/", (req, res) => {
    res.processAsync();
    do_timeout(RESPONSE_DELAY, () => {
      send(res, { country_code: "UK" });
      res.finish();
    });
  });

  let result = await Region._fetchRegion();
  Assert.equal(result, null, "Region fetch should return null");

  await checkTelemetry(Region.TELEMETRY.TIMEOUT);
  await new Promise(r => srv.stop(r));
});

add_task(async function test_mismatched_probe() {
  let probeDetails = await getExpectedHistogramDetails();
  let probeHistogram;
  if (probeDetails) {
    probeHistogram = Services.telemetry.getHistogramById(probeDetails.probeId);
    probeHistogram.clear();
  }
  histogram.clear();
  Region._home = null;

  setNetworkRegion("AU");
  await Region._fetchRegion();
  Assert.equal(Region.home, "AU", "Should have correct region");
  await checkTelemetry(Region.TELEMETRY.SUCCESS);

  // We dont store probes for linux and on treeherder +
  // Mac there is no plaform countryCode so in these cases
  // skip the rest of the checks.
  if (!probeDetails) {
    return;
  }
  let snapshot = probeHistogram.snapshot();
  deepEqual(snapshot.values, probeDetails.expectedResult);
});

add_task(async function test_location() {
  let location = { location: { lat: -1, lng: 1 }, accuracy: 100 };
  let srv = useHttpServer("geo.provider.network.url");
  srv.registerPathHandler("/", (req, res) => {
    res.setStatusLine("1.1", 200, "OK");
    send(res, location);
  });

  let result = await Region._getLocation();
  Assert.ok(true, "Region fetch should succeed");
  Assert.deepEqual(result, location, "Location is returned");

  await new Promise(r => srv.stop(r));
});

add_task(async function test_update() {
  Region._home = null;
  setNetworkRegion("FR");
  await Region._fetchRegion();
  Assert.equal(Region.home, "FR", "Should have correct region");
  setNetworkRegion("DE");
  await Region._fetchRegion();
  Assert.equal(Region.home, "FR", "Shouldnt have changed yet");
  // Thie first fetchRegion will set the prefs to determine when
  // to update the home region, we need to do 2 fetchRegions to test
  // it isnt updating when it shouldnt.
  await Region._fetchRegion();
  Assert.equal(Region.home, "FR", "Shouldnt have changed yet again");
  Services.prefs.setIntPref(INTERVAL_PREF, 1);
  /* eslint-disable mozilla/no-arbitrary-setTimeout */
  await new Promise(resolve => setTimeout(resolve, 1100));
  await Region._fetchRegion();
  Assert.equal(Region.home, "DE", "Should have updated now");
});

add_task(async function test_max_retry() {
  Region._home = null;
  let requestsSeen = 0;
  Services.prefs.setIntPref("browser.region.retry-timeout", RESPONSE_TIMEOUT);
  Services.prefs.setIntPref("browser.region.timeout", RESPONSE_TIMEOUT);
  let srv = useHttpServer(REGION_PREF);
  srv.registerPathHandler("/", (req, res) => {
    requestsSeen++;
    res.setStatusLine("1.1", 200, "OK");
    res.processAsync();
    do_timeout(RESPONSE_DELAY, res.finish.bind(res));
  });

  Region._fetchRegion();
  await TestUtils.waitForCondition(() => requestsSeen === 3);
  /* eslint-disable mozilla/no-arbitrary-setTimeout */
  await new Promise(resolve => setTimeout(resolve, RESPONSE_DELAY));

  Assert.equal(Region.home, null, "failed to fetch region");
  Assert.equal(requestsSeen, 3, "Retried 4 times");

  Region._retryCount = 0;
  await new Promise(r => srv.stop(r));
});

add_task(async function test_retry() {
  Region._home = null;
  let requestsSeen = 0;
  Services.prefs.setIntPref("browser.region.retry-timeout", RESPONSE_TIMEOUT);
  Services.prefs.setIntPref("browser.region.timeout", RESPONSE_TIMEOUT);
  let srv = useHttpServer(REGION_PREF);
  srv.registerPathHandler("/", (req, res) => {
    res.setStatusLine("1.1", 200, "OK");
    if (++requestsSeen == 2) {
      res.setStatusLine("1.1", 200, "OK");
      send(res, { country_code: "UK" });
    } else {
      res.processAsync();
      do_timeout(RESPONSE_DELAY, res.finish.bind(res));
    }
  });

  Region._fetchRegion();
  await TestUtils.waitForCondition(() => requestsSeen === 2);
  /* eslint-disable mozilla/no-arbitrary-setTimeout */
  await new Promise(resolve => setTimeout(resolve, RESPONSE_DELAY));

  Assert.equal(Region.home, "UK", "failed to fetch region");
  Assert.equal(requestsSeen, 2, "Retried 2 times");

  Region._retryCount = 0;
  await new Promise(r => srv.stop(r));
});

function setNetworkRegion(region) {
  Services.prefs.setCharPref(
    REGION_PREF,
    `data:application/json,{"country_code": "${region}"}`
  );
}

function useHttpServer(pref) {
  let server = new HttpServer();
  server.start(-1);
  Services.prefs.setCharPref(
    pref,
    `http://localhost:${server.identity.primaryPort}/`
  );
  return server;
}

function send(res, json) {
  res.setStatusLine("1.1", 200, "OK");
  res.setHeader("content-type", "application/json", true);
  res.write(JSON.stringify(json));
}

async function checkTelemetry(aExpectedValue) {
  // Wait until there is 1 result.
  await TestUtils.waitForCondition(() => {
    let snapshot = histogram.snapshot();
    return Object.values(snapshot.values).reduce((a, b) => a + b) == 1;
  });
  let snapshot = histogram.snapshot();
  Assert.equal(snapshot.values[aExpectedValue], 1);
}

// Define some checks for our platform-specific telemetry.
// We can't influence what they return (as we can't
// influence the countryCode the platform thinks we
// are in), but we can check the values are
// correct given reality.
async function getExpectedHistogramDetails() {
  let probeUSMismatched, probeNonUSMismatched;
  switch (AppConstants.platform) {
    case "macosx":
      probeUSMismatched = "SEARCH_SERVICE_US_COUNTRY_MISMATCHED_PLATFORM_OSX";
      probeNonUSMismatched =
        "SEARCH_SERVICE_NONUS_COUNTRY_MISMATCHED_PLATFORM_OSX";
      break;
    case "win":
      probeUSMismatched = "SEARCH_SERVICE_US_COUNTRY_MISMATCHED_PLATFORM_WIN";
      probeNonUSMismatched =
        "SEARCH_SERVICE_NONUS_COUNTRY_MISMATCHED_PLATFORM_WIN";
      break;
    default:
      break;
  }

  if (probeUSMismatched && probeNonUSMismatched) {
    let countryCode = await Services.sysinfo.countryCode;
    print("Platform says the country-code is", countryCode);
    if (!countryCode) {
      // On treeherder for Mac the countryCode is null, so the probes won't be
      // recorded.
      // We still let the test run for Mac, as a developer would likely
      // eventually pick up on the issue.
      info("No country code set on this machine, skipping rest of test");
      return false;
    }

    if (countryCode == "US") {
      // boolean probe so 3 buckets, expect 1 result for |1|.
      return {
        probeId: probeUSMismatched,
        expectedResult: { 0: 0, 1: 1, 2: 0 },
      };
    }
    // We are expecting probeNonUSMismatched with false if the platform
    // says AU (not a mismatch) and true otherwise.
    return {
      probeId: probeNonUSMismatched,
      expectedResult:
        countryCode == "AU" ? { 0: 1, 1: 0 } : { 0: 0, 1: 1, 2: 0 },
    };
  }
  return false;
}
