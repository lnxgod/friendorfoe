# Image and icon audit

The visual direction stays consistent with the interface refresh: calm charcoal
surfaces, clear aircraft photography, restrained category artwork, and a quieter
map. Photos should show the whole airframe. A missing or unverified photo should
leave useful, explicitly labeled artwork instead of an empty rectangle.

## Review scope

- Checked all 371 bundled JPEGs for decoding and dimensions, and visually reviewed
  contact sheets covering every file. All files decoded; none were below 120 px
  on either side. Also checked every literal aircraft/drone photo path in code.
- Reviewed all three image-loading surfaces: aircraft detail, aircraft guide,
  and drone guide; their loading, failed-load, and absent-photo behavior.
- Reviewed catalog-to-photo associations, duplicate list keys, vector silhouettes,
  map marker geometry, and the map's dark tile treatment.
- Kept camera capture/share handling unchanged; these use validated captured
  bitmaps rather than the reference-photo catalog.

## Corrections

- Replaced 16 poor or incorrect photos with specifically selected Wikimedia
  source files. Each replacement has an author, license, source URL, and SHA-256
  in [photo-replacements.json](photo-replacements.json). Bundled `CREDITS.md` files
  carry the updated attributions.
- Corrected 12 unrelated aircraft mappings, including Su-57 → F-22, Su-25 → A-10,
  PC-24 → PC-12, and F/A-18 → a passenger aircraft. Restored the existing CH-53
  photo to its previously empty reference entry.
- Removed duplicate Kfir and DJI Neo entries whose repeated keys could break
  Compose reference lists.
- Withheld 81 unsuitable assets from rendering. Examples included scenery,
  weather maps, documents, drone-camera shots rather than the drone, unrelated
  models, and images whose subject could not be confidently identified.
  The original files remain for provenance. Reasons live in
  `ReferencePhotoCatalog.kt`; they cannot be returned as usable catalog photos.
- Sixteen aircraft entries without suitable photos and the HOVERAir X1 Pro
  entry now use category artwork rather than photos of other airframes. This
  is a visual audit, not a guarantee that every historical photo's exact subtype
  or provenance has been independently verified.
- Removed speculative prefix-based photo matching. An unknown type no longer
  borrows an image merely because its first characters resemble a known type.

## Presentation

- All photo surfaces share loading/error handling. A remote image failure tries
  the bundled type photo, then a labeled category illustration. Bundled images
  can appear immediately while online photos load.
- Full-airframe fitting replaces center cropping. Bundled detail photos are
  labeled **Type reference** so they are not mistaken for the observed aircraft.
- Ground vehicles, unknown objects, and fixed-wing UAVs have appropriate vector
  symbols. Military transport codes no longer inherit fighter silhouettes.
- Map tiles use a neutral dark luminance palette. Marker outlines are lighter
  and inset so rotated tips are not clipped by the bitmap edge.

## Regression coverage

`ReferencePhotoCatalogTest` checks physical catalog paths, rejected assets,
corrected model mappings, unique list IDs, and replacement hashes/credits.
`ReferenceImageAuditTest` decodes every JPEG on Android, renders every silhouette
and map marker, exercises corrupt and missing image fallbacks, and captures the
actual reference-guide composables in light and dark modes.

Native screenshots and final verification results are recorded in the main
[interface review](README.md).
