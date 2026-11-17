# Store reference photo rights

`verified-photos.json` is the shared image allowlist for the Google Play and iPhone builds. The model database and the image list are separate: a missing photo does not remove a model, change Bluetooth detection, or prevent an offline text lookup. The larger source gallery is retained for development and rights research. A bundled image's presence in Git, matching filename, or GPS cleanup is not evidence of a license.

## Verified subset

On 2026-11-17 we checked 25 images (20 aircraft and five drones). Each record names the author, gives the source and licensing page, and specifies the SHA-256 of the actual app asset and of the source preview used for comparison. Two records are CC0 dedications. For public-domain records, the licensing link goes to that work's own declaration: the basis for release is not identical across countries or creators. File titles are for attribution; the model guide supplies the familiar name.

We traced candidates from the two Android credits files and the historical `images/aircraft/CREDITS.md`. We queried the Commons file's current `imageinfo.extmetadata`, checked the actual subject and credited author, then compared the source preview with the local image. To compare differently sized images, we decoded both to 64 × 64 RGB. Accepted pairs differed by less than 1.2% RMS of full scale and 0.5% in aspect ratio. We also visually reviewed the packed-image candidates and checked their catalog names. Wrong model assignments, unclear subjects, uncertain rights, and entries attributed simply to “Wikimedia Commons” were excluded. For example, the historical C-130 crash-drill credit does not match the packaged in-flight image; the latter matches the NASA record.

## Adding or replacing a photo

1. Confirm the work and individual rights declaration at the source. Record the actual photographer or agency and applicable terms. A search result, vendor name, or gallery-wide license is insufficient. The list currently contains only CC0 and source-declared public-domain works. Review additional license obligations before expanding that policy.
2. Confirm that the photo shows the stated model, not just a similar aircraft or a related product. Compare the source preview to the actual asset and retain the link and SHA-256 for each. Record resizing, cropping, recompression or other edits. Preserve any attribution the source requests.
3. Update the allowlist. Rebuild and inspect the Play bundle and iPhone archive: unlisted paths must be absent and every packaged photo must match its recorded hash. Open the app's credits and test both a model with a photo and one with only its text entry. Review changed photos for location metadata.

[Apple content rights guidance](https://developer.apple.com/app-store/review/guidelines/#intellectual-property) · [Google Play intellectual property policy](https://support.google.com/googleplay/android-developer/answer/9888072) · [Creative Commons CC0](https://creativecommons.org/publicdomain/zero/1.0/)
