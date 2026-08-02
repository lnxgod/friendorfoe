# Friend or Foe Badge Hardware

These are the fabrication and mechanical files used for the Friend or Foe
badge shown at DEF CON 34 Packet Hacking Village. They are published so people
can build, inspect, modify, and reflash the same three-radio ESP32-S3 platform.

## Files

| Path | Contents |
|------|----------|
| [`bom/friend-or-foe-badge-bom.csv`](bom/friend-or-foe-badge-bom.csv) | Original component list from the hardware build |
| [`fabrication/friend-or-foe-badge-single-board.zip`](fabrication/friend-or-foe-badge-single-board.zip) | Gerbers, drill files, and KiCad job file for one full badge PCB |
| [`fabrication/friend-or-foe-badge-oshpark-panel-5-badges-2-cores.zip`](fabrication/friend-or-foe-badge-oshpark-panel-5-badges-2-cores.zip) | Cost-optimized panel containing five full badges and two center cores |
| [`mechanical/battery-cage-dc34phv.stl`](mechanical/battery-cage-dc34phv.stl) | Binary STL for the rear battery cage |

The detailed per-badge parts list is in the root
[README](../../README.md#build-one). The raw CSV keeps the supplied rows and
ordering so the published source stays traceable to the build package.

## Fabrication Notes

The single-board KiCad job describes a two-layer, 1.6 mm board measuring
160.05 x 138.6141 mm. The panel job measures 485.19 x 142.4241 mm. Both ZIPs
contain copper, mask, paste, silkscreen, edge-cut, plated-drill, non-plated-drill,
and Gerber job files. They are fabrication outputs, not editable KiCad project
sources.

The panel was laid out around OSH Park's print-size constraints at the time of
the DEF CON run. Board-house capabilities and panel rules change, so inspect
the Gerber preview and confirm the dimensions, edge cuts, drill files, and
panel acceptance with the fabricator before ordering.

## Build Notes

- One badge uses three XIAO ESP32-S3 boards: one uplink/display controller and
  two scanners running the same scanner image with different runtime roles.
- Each radio connects through its Seeed coax lead to an Abracon 2.4 GHz
  Wi-Fi/Bluetooth patch antenna. Each patch sits over its own copper triangle,
  making the badge art part of the RF ground-plane design.
- Verify PH2.0 battery polarity with a meter before connecting a cell. The
  DEF CON battery batch arrived with the opposite pin order and every connector
  had to be repinned.
- Dry-fit the screen, battery cage, USB-C cables, and lanyard hardware before
  soldering. Display carrier dimensions and preinstalled headers vary by lot.
- The button footprint in this PCB revision did not perfectly match the buttons
  used for the DEF CON run. The badges were completed with hand fitting and
  soldering; verify pad contact and continuity on each switch.
- The battery cage STL does not include printer settings. Choose material and
  settings appropriate for a wearable that holds a lithium-ion cell.

The 45-badge DEF CON run came to roughly **$80 per badge** in direct board and
component cost. That is a historical build estimate, not a supplier quote. It
does not include tools, labor, 3D-printer time or material, or general shop
supplies.

## Firmware

Build and flash the badge from the repository root by following
[Build And Test](../../README.md#build-and-test). The supported field-update
path connects to the uplink over USB-C; the uplink then updates both scanner
boards over their internal UART links.
