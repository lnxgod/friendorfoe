# Friend or Foe Privacy Policy

GameChangers AI publishes Friend or Foe for Android. You can use the app without creating an account. This policy explains what your phone observes, which services receive data, and how you can remove saved information.

## Nearby signals and location

When phone scanning is enabled, the app examines Bluetooth advertisements and nearby Wi-Fi access points. Observations can include device or network names, radio addresses, beacon identifiers, signal strength and time. The phone uses those details to group observations and identify broadcast patterns. A device match cannot establish whether someone is recording.

Android requires location access for some radio scans. With location access, the app can show your position, save coordinates with observations, and look up nearby aircraft and weather. When you start Nearby Watch, it can continue using your location and checking nearby aircraft while the app is minimized. Stop the watch from the app or its ongoing notification.

## Aircraft, weather and maps

Aircraft lookups send your coordinates or a surrounding area to aircraft-data services. The app can query adsb.fi, airplanes.live, adsb.lol, adsb.one and OpenSky. Weather requests send coordinates to Open-Meteo. Aircraft and route detail queries can send an aircraft identifier or callsign to HexDB. Aircraft photo hosts receive requests for the selected image. OpenStreetMap tile requests reveal the area you are viewing, including the area around you when the map is centered on your location. Online services also receive your IP address when the phone connects.

## Microphone, camera and Google services

The optional acoustic and ultrasonic tools sample the microphone to measure sound frequencies on the phone. They keep no audio recording. Camera tools analyze frames on the phone for the augmented-reality view, image labels and bright spots. When you choose Save or Share, a photo can include the scene, overlays and observation details. Sharing sends it to the recipient or app you select.

Google ML Kit can send installation identifiers, device and app information, feature usage and performance measurements to Google for analytics and diagnostics. Augmented-reality features use Google Play Services for AR. Google collects device identifiers, API usage and diagnostics and, when you are signed in to Google, account identifiers. Google Play also handles requests to check for app updates. Google describes its handling of this information in its published notices.

[Google ML Kit data disclosures](https://developers.google.com/ml-kit/android-data-disclosure)

[Google Play Services for AR data disclosures](https://developers.google.com/ar/develop/play-safety-label)

[Google Privacy Policy](https://policies.google.com/privacy)

## Optional badges and sensor servers

While the app is open and a USB badge connection is inactive, Friend or Foe periodically asks the stock badge’s local address (192.168.4.1) for status. This starts automatically, including on first launch before you open the Badge screen. The read-only request uses unencrypted HTTP. A device reachable at that address can see the request and the phone’s network address. Badge status can include nearby observations from the badge. Badge controls use a USB connection.

You can also connect a sensor server. The app stores the connection settings on your phone and sends requests to the server you choose using HTTPS, including on a local network. Badge and sensor data can include network names, radio identifiers, locations and times from their sensors. The app does not follow redirects for these connections.

If you start a calibration walk, the app sends your operator label, a session identifier, precise coordinates, time, accuracy when available, and calibration measurements to the server you configured. The app uses HTTPS for these requests and sends the calibration token to that server. Calibration can also advertise a Bluetooth identifier for nearby sensors to measure. The operator of your chosen server controls its records, logs and backups. Ask that operator about retention or deletion of server-side data.

## Retention and deletion

Privacy keeps up to 200 recent encounters in memory for up to 30 minutes after their last observation. Use Clear recent encounters in Privacy to clear this list. History can keep observations and phone coordinates on the device until you delete entries or clear History. You can delete saved flight paths individually in Saved flights.

Android settings let you revoke camera, microphone, nearby-device and location permissions, or clear the app’s storage. Clearing storage or uninstalling removes local app settings and databases. Friend or Foe excludes its private data from Android Auto Backup. Delete photos separately in your gallery, and remove exports and shared copies wherever you saved or sent them.

Online data providers apply their own retention and deletion practices to requests they receive. Clearing the app cannot delete those records or records held by a sensor operator. For privacy questions about Friend or Foe, contact GameChangers AI through its published organization contact details.

[Contact GameChangers AI](https://gamechangersai.org/about.html)
