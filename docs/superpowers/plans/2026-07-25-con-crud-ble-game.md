# CON CRUD BLE Game Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and physically prove a provisional `0.64.79-badge-defcon34` CON CRUD BLE game in which only the uplink advertises game state, the BLE-primary scanner qualifies nearby peers, factory flashing selects `normal`, `infected`, or `immune`, and every USB/UART firmware operation safely preempts the game.

**Architecture:** Keep game rules, BLE/UART codecs, encounter qualification, and update-maintenance decisions in small allocation-free C policy modules covered by native Unity tests. The uplink owns persisted game state and a controller-only VHCI advertiser; the existing BLE-primary scanner owns a fixed eight-peer observer and sends only qualified evidence over its existing UART. A distinct rebooted update-maintenance mode releases BLE memory before USB OTA or scanner relay, while the host binds every reconnect to the same hidden hardware identity and restarts interrupted transfers from byte zero.

**Tech Stack:** ESP32-S3, ESP-IDF 5.x, PlatformIO, C11, FreeRTOS, NimBLE scanner callbacks, ESP-IDF VHCI controller API, NVS, RTC no-init memory, native Unity/AddressSanitizer tests, Python 3 host flashers and pytest/unittest, Kotlin/Jetpack Compose regression builds.

## Global Constraints

- Preserve every existing dirty-worktree change; never edit or delete `.camera-before-zoom.jpg`.
- Do not commit, push, tag, publish, merge, replace the embedded factory ZIP, or promote a release during this plan. Each task ends with a diff-and-test checkpoint instead of a Git commit.
- Production environments remain `uplink-s3-fof_badge` and `scanner-s3-combo-fof_badge` at `0.64.78-badge-defcon34`.
- Canary environments are `uplink-s3-fof_badge-con-crud-canary` and `scanner-s3-combo-fof_badge-con-crud-canary` at `0.64.79-badge-defcon34`.
- New game runtime, scanner observer, `game_seed` mutation, BLE controller,
  queue reclamation, and update-maintenance boot call sites compile only when
  `FOF_DC34_GAME_CANARY=1`. Shared coordinator correctness fixes remain common
  only when their existing production tests prove behavior.
- The factory and USB flashers continue to use one uplink image and one shared scanner image for both physical scanner slots.
- Only the uplink emits CON CRUD advertisements. Scanner game advertising is forbidden; existing BLE active scan requests and bounded GATT investigation remain enabled.
- Game transport uses BLE only and never uses Wi-Fi, SSIDs, Remote ID, or the four normal privacy lanes.
- Existing Easter triggers remain byte-exact: SSID `GameChangersAI-67`; Remote ID Basic ID `fof-michagain` at the existing exact Hell, Michigan coordinates with geodetic altitude `666` metres; and the existing physical-button trigger.
- Game activation occurs only after a successfully launched Easter presentation is dismissed. The existing 90-second Easter radio cooldown remains unchanged.
- The reset/recovery chord is both existing buttons held continuously for exactly 10 seconds and always wins over Easter or game input.
- Peer quorum is three distinct sequence values for one peer/session within six seconds, each at RSSI `-60 dBm` or stronger. The third packet applies the first effect.
- Immune peers add `5` shield to normal players and `10` cure to infected players. Cure at `100` produces normal role with `50` shield.
- Infected peers subtract `10` shield. The packet that reaches zero is absorbed; the next qualified infected packet infects the unshielded player.
- Shield/cure decays lazily by one point per minute while active; immune remains fixed at `100`.
- The custom Service Data UUID is `f0f34c01-6761-6d65-6368-616e67657273`, protocol version is `1`, round is `0x22`, and the public 16-byte SipHash key is ASCII `FoF-DC34-CONCRUD`.
- The canonical scanner command is `{"type":"crud_self","v":1,"round":34,"peer":"A1B2C3","session":"07"}` and the canonical acknowledgment is `{"type":"crud_self_ack","v":1,"round":34,"peer":"A1B2C3","session":"07"}`.
- The canonical qualified UART line is `FOF_CRUD:1,22,2,A1B2C3,07,2A,-058`.
- Sequence increments once per one-second payload epoch. Controller retries
  may repeat that sequence and scanners reject those repeats as duplicates.
- Game code adds no FreeRTOS task, display framebuffer, heap-backed RF frame, cJSON object, dynamic peer table, or per-frame allocation. Fixed static HCI command/data arrays are permitted.
- Every update begins with `FOF_CTL:{"cmd":"prepare_update","session":"0123456789ABCDEF"}` and the exact response prefix is `FOF_UPDATE_MODE:`.
- The sole bootstrap exception is an exact trusted
  `0.64.78-badge-defcon34` uplink, whose build has Bluetooth disabled, updating
  directly to exact `0.64.79-badge-defcon34`. After `.79` proves fresh health,
  it must enter update maintenance before either scanner is staged.
- Update maintenance reports `recovery_mode:"update_maintenance"`, never initializes BLE, releases BLE controller memory for that boot, and retains USB, display status, scanner UARTs, staging, and the durable relay coordinator.
- Host retries never resume a byte offset. They accept an exact already-committed identity or retransmit from byte zero; ambiguity fails closed.
- Factory CLI uses `--game-role {normal,infected,immune}`, defaults to `normal`, sends `FOF_SET:game_seed=<role>`, requires exact `FOF_OK:game_seed`, reboots, and proves fresh game status before PASS.
- Public factory PASS output contains `PASS // GAME ROLE <role> // RECEIPT rcpt_<8 uppercase Crockford characters>` and contains no MAC-derived label.
- Before hardware mutation, automated gates must prove at least `24 KiB` free internal heap, `16 KiB` largest internal block, and `12 KiB` minimum-ever internal heap in normal mode; update-maintenance heap must meet or exceed the `.78` updater baseline.
- Physical promotion remains blocked until two complete badges pass the full matrix and three consecutive complete uplink-plus-two-scanner update cycles.

---

## File and Interface Map

The implementation uses one name family throughout:

- `esp32/shared/badge_con_protocol.{h,c}` owns SipHash, exact BLE payloads, exact advertisement parsing, exact UART lines, and canonical self-command/ack rendering.
- `esp32/shared/badge_con_game.{h,c}` owns role parsing, game transitions, lazy decay, the atomic 12-byte NVS record, and the RTC record codec.
- `esp32/shared/badge_con_encounter.{h,c}` owns the fixed eight-peer quorum/duplicate/rate-limit table.
- `esp32/shared/badge_update_maintenance_policy.{h,c}` owns exact session validation, maintenance-marker validation, inactivity, and boot decisions.
- `esp32/uplink/main/core/badge_con_runtime.{h,c}` is the sole mutable game-state owner and NVS/RTC adapter.
- `esp32/uplink/main/game/badge_con_vhci.{h,c}` is the sole game advertiser and owns no task.
- `esp32/scanner/main/detection/badge_con_observer.{h,c}` adapts NimBLE advertisements to the shared protocol and encounter policies.
- Existing scanner command registries authorize `crud_self`; existing UART TX drains `crud_self_ack` and qualified `FOF_CRUD` evidence below command, firmware, and detection traffic.
- Existing uplink UART ingress accepts game evidence only from scanner slot `0`, before cJSON/general detection ingress.
- Existing `fw_store` owns campaign state and update-radio inhibition; the game is allowed only when the fail-busy campaign sample says `IDLE` or `ALL_TERMINAL`.
- Existing `BadgeSerial` owns update preparation, same-device reconnect, byte-zero retry, and maintenance completion.

### Canonical shared types

```c
typedef enum {
    BADGE_CON_ROLE_NORMAL = 0,
    BADGE_CON_ROLE_INFECTED = 1,
    BADGE_CON_ROLE_IMMUNE = 2,
} badge_con_role_t;

typedef struct {
    uint8_t version;
    uint8_t round;
    badge_con_role_t role;
    uint32_t peer;       /* low 24 bits; never zero */
    uint8_t session;     /* never zero */
    uint8_t sequence;
    int8_t rssi;
} badge_con_packet_t;

typedef struct {
    badge_con_role_t seed;
    badge_con_role_t role;
    bool active;
    uint8_t shield;
    uint32_t last_decay_ms;
} badge_con_game_state_t;

typedef struct {
    badge_con_role_t seed;
    badge_con_role_t role;
    bool active;
    uint8_t shield;
} badge_con_snapshot_t;
```

### Task 1: Exact BLE and UART Protocol Codec

**Files:**

- Create: `esp32/shared/badge_con_protocol.h`
- Create: `esp32/shared/badge_con_protocol.c`
- Create: `esp32/test/test_badge_con_protocol.c`
- Modify: `esp32/platformio.ini`
- Modify: `esp32/test/test_runner.c`

**Interfaces:**

- Consumes: no new production interface.
- Produces:

```c
#define BADGE_CON_PROTOCOL_VERSION 1U
#define BADGE_CON_ROUND 0x22U
#define BADGE_CON_SERVICE_PAYLOAD_BYTES 10U
#define BADGE_CON_LEGACY_ADV_BYTES 31U
#define BADGE_CON_UART_LINE_CHARS 33U
#define BADGE_CON_UART_WIRE_BYTES 34U
#define BADGE_CON_UART_BUFFER_BYTES 35U

typedef enum {
    BADGE_CON_FRAME_NOT_GAME = 0,
    BADGE_CON_FRAME_INVALID = 1,
    BADGE_CON_FRAME_VALID = 2,
} badge_con_frame_result_t;

uint64_t badge_con_siphash24(const uint8_t key[16],
                             const uint8_t *bytes,
                             size_t byte_count);
bool badge_con_build_service_payload(badge_con_role_t role,
                                     uint32_t peer,
                                     uint8_t session,
                                     uint8_t sequence,
                                     uint8_t out[10]);
bool badge_con_build_legacy_advertisement(badge_con_role_t role,
                                          uint32_t peer,
                                          uint8_t session,
                                          uint8_t sequence,
                                          uint8_t out[31]);
badge_con_frame_result_t badge_con_parse_advertisement(
    const uint8_t *advertisement,
    size_t advertisement_size,
    int8_t rssi,
    badge_con_packet_t *out);
bool badge_con_render_uart_line(const badge_con_packet_t *packet,
                                char *out,
                                size_t out_size,
                                size_t *wire_size_out);
bool badge_con_parse_uart_line(const uint8_t *bytes,
                               size_t byte_count,
                               badge_con_packet_t *out);
bool badge_con_render_self_command(uint32_t peer,
                                   uint8_t session,
                                   char *out,
                                   size_t out_size);
bool badge_con_render_self_ack(uint32_t peer,
                               uint8_t session,
                               char *out,
                               size_t out_size);
```

- [ ] **Step 1: Register one intentionally unresolved protocol test**

Add the source filter and runner declarations, then add this first red test:

```c
void test_badge_con_protocol_builds_exact_reference_frame(void)
{
    uint8_t advertisement[BADGE_CON_LEGACY_ADV_BYTES] = {0};
    TEST_ASSERT_TRUE(badge_con_build_legacy_advertisement(
        BADGE_CON_ROLE_IMMUNE, 0xA1B2C3U, 0x07U, 0x2AU, advertisement));
    TEST_ASSERT_EQUAL_HEX8(0x02, advertisement[0]);
    TEST_ASSERT_EQUAL_HEX8(0x01, advertisement[1]);
    TEST_ASSERT_EQUAL_HEX8(0x06, advertisement[2]);
    TEST_ASSERT_EQUAL_HEX8(0x1B, advertisement[3]);
    TEST_ASSERT_EQUAL_HEX8(0x21, advertisement[4]);
    TEST_ASSERT_EQUAL_HEX8(0x73, advertisement[5]);
    TEST_ASSERT_EQUAL_HEX8(0xF3, advertisement[19]);
    TEST_ASSERT_EQUAL_HEX8(0xF0, advertisement[20]);
    TEST_ASSERT_EQUAL_HEX8(0x12, advertisement[21]);
    TEST_ASSERT_EQUAL_HEX8(0x22, advertisement[22]);
    TEST_ASSERT_EQUAL_HEX8(0xC3, advertisement[23]);
    TEST_ASSERT_EQUAL_HEX8(0xB2, advertisement[24]);
    TEST_ASSERT_EQUAL_HEX8(0xA1, advertisement[25]);
    TEST_ASSERT_EQUAL_HEX8(0x07, advertisement[26]);
    TEST_ASSERT_EQUAL_HEX8(0x2A, advertisement[27]);
    TEST_ASSERT_EQUAL_HEX8(0xCA, advertisement[28]);
    TEST_ASSERT_EQUAL_HEX8(0x00, advertisement[29]);
    TEST_ASSERT_EQUAL_HEX8(0x4D, advertisement[30]);
}
```

- [ ] **Step 2: Run the focused native test and record the red result**

Run:

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final/esp32
/Users/billh/.platformio/penv/bin/pio test -e test -f test_badge_con_protocol
```

Expected: compile/link failure naming `badge_con_build_legacy_advertisement`.

- [ ] **Step 3: Implement SipHash and the exact 31-byte advertisement**

Use a private constant:

```c
static const uint8_t BADGE_CON_SIPHASH_KEY[16] = {
    'F','o','F','-','D','C','3','4','-','C','O','N','C','R','U','D'
};
```

The byte layout is exactly:

```text
02 01 06
1B 21
73 72 65 67 6E 61 68 63 65 6D 61 67 01 4C F3 F0
VR 22 PP PP PP SS QQ TT TT TT
```

where the UUID is the little-endian form of
`f0f34c01-6761-6d65-6368-616e67657273`, `VR` has version `1` in the high
nibble and role in bits `0..1`, and `TT TT TT` is the low 24 bits of
SipHash-2-4 over the preceding seven payload bytes, serialized least
significant byte first. For the reference payload, SipHash is
`0x827642885D4D00CA` and the three tag bytes are `CA 00 4D`.

- [ ] **Step 4: Add the complete parser and UART rejection matrix**

Add named tests covering:

```c
void test_badge_con_protocol_matches_siphash_reference_vectors(void);
void test_badge_con_protocol_round_trips_all_three_roles(void);
void test_badge_con_protocol_rejects_zero_peer_and_session(void);
void test_badge_con_protocol_distinguishes_non_game_from_claimed_invalid(void);
void test_badge_con_protocol_rejects_reserved_role_bits_wrong_round_and_tag(void);
void test_badge_con_protocol_accepts_sequence_zero_and_ff(void);
void test_badge_con_uart_renders_exact_reference_line(void);
void test_badge_con_uart_accepts_rssi_minus_127_and_minus_001(void);
void test_badge_con_uart_rejects_weak_width_case_whitespace_nul_and_suffixes(void);
void test_badge_con_self_messages_are_canonical_and_bounded(void);
```

Use this exact UART assertion:

```c
char line[BADGE_CON_UART_BUFFER_BYTES] = {0};
size_t wire_size = 0;
badge_con_packet_t packet = {
    .version = 1,
    .round = 0x22,
    .role = BADGE_CON_ROLE_IMMUNE,
    .peer = 0xA1B2C3,
    .session = 0x07,
    .sequence = 0x2A,
    .rssi = -58,
};
TEST_ASSERT_TRUE(badge_con_render_uart_line(
    &packet, line, sizeof(line), &wire_size));
TEST_ASSERT_EQUAL_STRING("FOF_CRUD:1,22,2,A1B2C3,07,2A,-058\n", line);
TEST_ASSERT_EQUAL_UINT(BADGE_CON_UART_WIRE_BYTES, wire_size);
```

The parser must consume an explicit byte count, reject embedded NUL bytes,
return `BADGE_CON_FRAME_INVALID` for any advertisement containing the exact
UUID with invalid fields, and never search beyond valid AD structures.

- [ ] **Step 5: Run focused and full native tests**

Run:

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final/esp32
/Users/billh/.platformio/penv/bin/pio test -e test -f test_badge_con_protocol
/Users/billh/.platformio/penv/bin/pio test -e test
```

Expected: both commands pass under AddressSanitizer.

- [ ] **Step 6: Checkpoint the task without committing**

Run:

```bash
git diff --check -- esp32/shared/badge_con_protocol.h esp32/shared/badge_con_protocol.c esp32/test/test_badge_con_protocol.c esp32/platformio.ini esp32/test/test_runner.c
git status --short -- esp32/shared/badge_con_protocol.h esp32/shared/badge_con_protocol.c esp32/test/test_badge_con_protocol.c esp32/platformio.ini esp32/test/test_runner.c
```

Expected: no whitespace errors and only the listed protocol/test integration paths.

### Task 2: Pure Game State, Atomic NVS Record, and RTC Codec

**Files:**

- Create: `esp32/shared/badge_con_game.h`
- Create: `esp32/shared/badge_con_game.c`
- Create: `esp32/test/test_badge_con_game.c`
- Modify: `esp32/platformio.ini`
- Modify: `esp32/test/test_runner.c`

**Interfaces:**

- Consumes: `badge_con_role_t` from `badge_con_protocol.h`.
- Produces:

```c
#define BADGE_CON_NVS_RECORD_BYTES 12U
#define BADGE_CON_RTC_RECORD_BYTES 20U

typedef enum {
    BADGE_CON_EFFECT_NONE = 0,
    BADGE_CON_EFFECT_SHIELD_GAINED,
    BADGE_CON_EFFECT_SHIELD_DRAINED,
    BADGE_CON_EFFECT_ZERO_ABSORBED,
    BADGE_CON_EFFECT_INFECTED,
    BADGE_CON_EFFECT_CURE_GAINED,
    BADGE_CON_EFFECT_CURED,
} badge_con_effect_t;

typedef enum {
    BADGE_CON_NVS_INVALID = 0,
    BADGE_CON_NVS_SEED_ONLY,
    BADGE_CON_NVS_VALID,
} badge_con_nvs_decode_result_t;

bool badge_con_role_parse_exact(const char *value, badge_con_role_t *out);
const char *badge_con_role_name(badge_con_role_t role);
void badge_con_game_defaults(badge_con_game_state_t *out);
void badge_con_game_apply_factory_seed(badge_con_game_state_t *state,
                                       badge_con_role_t seed,
                                       uint32_t now_ms);
bool badge_con_game_activate(badge_con_game_state_t *state,
                             uint32_t now_ms);
badge_con_effect_t badge_con_game_apply_peer(
    badge_con_game_state_t *state,
    badge_con_role_t peer_role,
    uint32_t now_ms);
void badge_con_game_snapshot(badge_con_game_state_t *state,
                             uint32_t now_ms,
                             badge_con_snapshot_t *out);
bool badge_con_game_encode_nvs(
    const badge_con_game_state_t *state,
    uint8_t out[BADGE_CON_NVS_RECORD_BYTES]);
badge_con_nvs_decode_result_t badge_con_game_decode_nvs(
    const uint8_t *record,
    size_t record_size,
    badge_con_game_state_t *out);
bool badge_con_game_encode_rtc(
    const badge_con_game_state_t *state,
    uint32_t expected_reboot_generation,
    uint8_t out[BADGE_CON_RTC_RECORD_BYTES]);
bool badge_con_game_decode_rtc(
    const uint8_t *record,
    size_t record_size,
    uint32_t expected_reboot_generation,
    badge_con_game_state_t *out);
uint8_t badge_con_game_shield_checkpoint(uint8_t shield);
```

- [ ] **Step 1: Write red transition tests**

Start with:

```c
void test_badge_con_game_infection_requires_packet_after_zero(void)
{
    badge_con_game_state_t game = {
        .seed = BADGE_CON_ROLE_NORMAL,
        .role = BADGE_CON_ROLE_NORMAL,
        .active = true,
        .shield = 10,
        .last_decay_ms = 1000,
    };
    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_ZERO_ABSORBED,
        badge_con_game_apply_peer(
            &game, BADGE_CON_ROLE_INFECTED, 1000));
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_NORMAL, game.role);
    TEST_ASSERT_EQUAL_UINT8(0, game.shield);
    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_INFECTED,
        badge_con_game_apply_peer(
            &game, BADGE_CON_ROLE_INFECTED, 2000));
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_INFECTED, game.role);
}

void test_badge_con_game_cure_at_100_becomes_normal_with_50_shield(void)
{
    badge_con_game_state_t game = {
        .seed = BADGE_CON_ROLE_INFECTED,
        .role = BADGE_CON_ROLE_INFECTED,
        .active = true,
        .shield = 90,
        .last_decay_ms = 0,
    };
    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_CURED,
        badge_con_game_apply_peer(
            &game, BADGE_CON_ROLE_IMMUNE, 1000));
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_NORMAL, game.role);
    TEST_ASSERT_EQUAL_UINT8(50, game.shield);
}
```

- [ ] **Step 2: Run the focused test and capture the red result**

Run:

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final/esp32
/Users/billh/.platformio/penv/bin/pio test -e test -f test_badge_con_game
```

Expected: unresolved `badge_con_game_apply_peer`.

- [ ] **Step 3: Implement transitions and lazy decay**

Implement one mutation path that first applies:

```c
uint32_t elapsed_ms = now_ms - state->last_decay_ms;
uint32_t whole_minutes = elapsed_ms / 60000U;
uint8_t decay = whole_minutes > state->shield
    ? state->shield
    : (uint8_t)whole_minutes;
state->shield = (uint8_t)(state->shield - decay);
state->last_decay_ms += whole_minutes * 60000U;
```

Run decay only when active and non-immune. Keep `last_decay_ms` unchanged for
sub-minute snapshots. Clamp every shield/cure mutation to `0..100`. A normal
peer and repeated infected-to-infected encounters return
`BADGE_CON_EFFECT_NONE`. Activation sets an immune seed/current role to
shield `100`; pre-activation factory status remains shield `0`.

- [ ] **Step 4: Add exact persistence and invalid-data tests**

The 12-byte NVS record is:

```text
byte 0    magic 0xC3
byte 1    version 1
byte 2    seed role
byte 3    current role
byte 4    active 0 or 1
byte 5    shield checkpoint in the set 0,10,20,30,40,50,60,70,80,90,100
bytes 6-7 zero
bytes 8-11 little-endian CRC32 over bytes 0-7
```

The 20-byte RTC record is:

```text
byte 0    magic 0xC4
byte 1    version 1
byte 2    seed role
byte 3    current role
byte 4    active 0 or 1
byte 5    exact shield 0..100
bytes 6-7 zero
bytes 8-11 little-endian last_decay_ms
bytes 12-15 little-endian expected reboot generation
bytes 16-19 little-endian CRC32 over bytes 0-15
```

Add named tests for exact lowercase role parsing, case/whitespace/suffix
rejection, default `normal/normal/false/0`, factory reset semantics,
activation idempotence, immune activation to `100`,
normal/immune/infected effects, clamp behavior,
one-point-per-minute decay, multi-minute decay, `uint32_t` wrap, immediate
role changes, decile crossings, record round-trip, and rejection of wrong
length, magic, version, enum, active byte, shield range, reserved byte,
generation, and CRC. A valid magic/version/CRC/seed with an invalid mutable
role, active byte, or shield returns `BADGE_CON_NVS_SEED_ONLY` and outputs the
configured seed as current role with inactive game and zero shield. Invalid
length, magic, version, CRC, or seed returns `BADGE_CON_NVS_INVALID` without
modifying the caller output; the runtime then uses normal/inactive/zero so
corruption can never grant immunity.

- [ ] **Step 5: Run focused and full native tests**

Run:

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final/esp32
/Users/billh/.platformio/penv/bin/pio test -e test -f test_badge_con_game
/Users/billh/.platformio/penv/bin/pio test -e test
```

Expected: all tests pass under AddressSanitizer.

- [ ] **Step 6: Checkpoint the task without committing**

Run:

```bash
git diff --check -- esp32/shared/badge_con_game.h esp32/shared/badge_con_game.c esp32/test/test_badge_con_game.c esp32/platformio.ini esp32/test/test_runner.c
```

Expected: no whitespace errors.

### Task 3: Fixed Eight-Peer Encounter Qualifier

**Files:**

- Create: `esp32/shared/badge_con_encounter.h`
- Create: `esp32/shared/badge_con_encounter.c`
- Create: `esp32/test/test_badge_con_encounter.c`
- Modify: `esp32/platformio.ini`
- Modify: `esp32/test/test_runner.c`

**Interfaces:**

- Consumes: validated `badge_con_packet_t` from Task 1.
- Produces:

```c
#define BADGE_CON_PEER_CAPACITY 8U
#define BADGE_CON_QUORUM_PACKETS 3U
#define BADGE_CON_QUORUM_WINDOW_MS 6000U
#define BADGE_CON_EFFECT_RATE_MS 1000U
#define BADGE_CON_MIN_RSSI (-60)

typedef enum {
    BADGE_CON_OBSERVE_DROPPED_WEAK = 0,
    BADGE_CON_OBSERVE_DROPPED_SELF,
    BADGE_CON_OBSERVE_DROPPED_DUPLICATE,
    BADGE_CON_OBSERVE_DROPPED_TABLE_FULL,
    BADGE_CON_OBSERVE_COUNTED,
    BADGE_CON_OBSERVE_QUALIFIED,
    BADGE_CON_OBSERVE_RATE_LIMITED,
} badge_con_observe_result_t;

typedef struct {
    uint32_t self_peer;
    uint8_t self_session;
    bool self_valid;
    struct {
        bool used;
        uint32_t peer;
        uint8_t session;
        uint8_t recent_sequence[3];
        uint32_t recent_ms[3];
        uint8_t recent_count;
        uint32_t last_seen_ms;
        uint8_t last_emitted_sequence;
        uint32_t last_emitted_ms;
        bool emitted;
    } peer[BADGE_CON_PEER_CAPACITY];
} badge_con_encounter_table_t;

void badge_con_encounter_init(badge_con_encounter_table_t *table);
bool badge_con_encounter_set_self(badge_con_encounter_table_t *table,
                                  uint32_t peer,
                                  uint8_t session);
badge_con_observe_result_t badge_con_encounter_consume(
    badge_con_encounter_table_t *table,
    const badge_con_packet_t *packet,
    uint32_t now_ms);
```

- [ ] **Step 1: Write red quorum, self, and RSSI boundary tests**

Use one packet and mutate only sequence/time:

```c
void test_badge_con_encounter_third_distinct_strong_packet_qualifies(void)
{
    badge_con_encounter_table_t table;
    badge_con_packet_t packet = {
        .version = 1,
        .round = 0x22,
        .role = BADGE_CON_ROLE_INFECTED,
        .peer = 0x102030,
        .session = 0x44,
        .sequence = 1,
        .rssi = -60,
    };
    badge_con_encounter_init(&table);
    TEST_ASSERT_EQUAL(BADGE_CON_OBSERVE_COUNTED,
                      badge_con_encounter_consume(&table, &packet, 1000));
    packet.sequence = 2;
    TEST_ASSERT_EQUAL(BADGE_CON_OBSERVE_COUNTED,
                      badge_con_encounter_consume(&table, &packet, 2000));
    packet.sequence = 3;
    TEST_ASSERT_EQUAL(BADGE_CON_OBSERVE_QUALIFIED,
                      badge_con_encounter_consume(&table, &packet, 3000));
}
```

Add a second red test proving `-61` never counts and a third proving an exact
self peer/session never occupies a table entry.

- [ ] **Step 2: Run the focused test and capture the red result**

Run:

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final/esp32
/Users/billh/.platformio/penv/bin/pio test -e test -f test_badge_con_encounter
```

Expected: unresolved encounter functions.

- [ ] **Step 3: Implement fixed-table admission and recent-sequence windows**

Expire per-peer quorum samples older than `6000` ms before duplicate checks.
An entire peer entry is replaceable only when
`(uint32_t)(now_ms - last_seen_ms) > 6000U`; the exact six-second boundary
remains active.
Treat `(peer, session)` as the identity. Keep all three recent sequence values
so reordered duplicates cannot qualify. Reuse an expired peer slot before an
unused slot; when all eight peers have unexpired evidence, return
`BADGE_CON_OBSERVE_DROPPED_TABLE_FULL` for a ninth peer.

- [ ] **Step 4: Add the full deterministic table test matrix**

Add named tests for:

```c
void test_badge_con_encounter_packet_two_does_not_qualify(void);
void test_badge_con_encounter_six_second_boundary_qualifies(void);
void test_badge_con_encounter_packet_after_six_seconds_restarts_quorum(void);
void test_badge_con_encounter_duplicate_and_reordered_sequence_do_not_count(void);
void test_badge_con_encounter_sequence_wrap_ff_to_zero_counts(void);
void test_badge_con_encounter_new_session_resets_peer_quorum(void);
void test_badge_con_encounter_later_effects_are_limited_to_one_per_second(void);
void test_badge_con_encounter_replaces_expired_entry_before_dropping_new_peer(void);
void test_badge_con_encounter_drops_ninth_active_peer(void);
void test_badge_con_encounter_uint32_time_wrap_is_safe(void);
```

No test or implementation may call `malloc`, create a queue, create a mutex,
write UART, log, or parse JSON.

- [ ] **Step 5: Run focused and full native tests**

Run:

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final/esp32
/Users/billh/.platformio/penv/bin/pio test -e test -f test_badge_con_encounter
/Users/billh/.platformio/penv/bin/pio test -e test
```

Expected: all tests pass.

- [ ] **Step 6: Checkpoint the task without committing**

Run:

```bash
git diff --check -- esp32/shared/badge_con_encounter.h esp32/shared/badge_con_encounter.c esp32/test/test_badge_con_encounter.c esp32/platformio.ini esp32/test/test_runner.c
```

Expected: no whitespace errors.

### Task 4: Scanner Observer, Self-Acknowledgment, and Allocation-Free UART Fast Path

**Files:**

- Create: `esp32/scanner/main/detection/badge_con_observer.h`
- Create: `esp32/scanner/main/detection/badge_con_observer.c`
- Modify: `esp32/scanner/main/detection/ble_remote_id.c`
- Modify: `esp32/scanner/main/comms/uart_tx.h`
- Modify: `esp32/scanner/main/comms/uart_tx.c`
- Modify: `esp32/scanner/main/main.c`
- Modify: `esp32/scanner/main/CMakeLists.txt`
- Modify: `esp32/shared/scanner_command_schema_registry.h`
- Modify: `esp32/shared/scanner_command_schema_registry.c`
- Modify: `esp32/shared/scanner_command_ingress.h`
- Modify: `esp32/shared/scanner_command_ingress.c`
- Modify: `esp32/shared/scanner_uplink_ingress_registry.h`
- Modify: `esp32/shared/scanner_uplink_ingress_registry.c`
- Modify: `esp32/uplink/main/comms/uart_rx.c`
- Create: `esp32/test/test_badge_con_observer.c`
- Modify: `esp32/test/test_scanner_command_schema_registry.c`
- Modify: `esp32/test/test_scanner_command_ingress.c`
- Modify: `esp32/test/test_scanner_uplink_ingress_registry.c`
- Modify: `backend/tests/test_uplink_uart_ingress_contract.py`
- Modify: `esp32/platformio.ini`
- Modify: `esp32/test/test_runner.c`

**Interfaces:**

- Consumes: the scanner observer consumes only the protocol codec from Task 1
  and encounter table from Task 3. The uplink slot-0 ingress adapter consumes
  `badge_con_runtime_apply_qualified_peer()` from Task 5.
- Produces:

```c
void badge_con_observer_init(bool ble_primary);
bool badge_con_observer_set_self(
                                 uint32_t peer,
                                 uint8_t session);
badge_con_frame_result_t badge_con_observer_consume(
    const uint8_t *advertisement,
    size_t advertisement_size,
    int8_t rssi,
    uint32_t now_ms,
    badge_con_observe_result_t *observe_result_out);
bool badge_con_observer_take_pending(badge_con_packet_t *out);
void uart_tx_set_firmware_quiet_window(bool active);
```

- [ ] **Step 1: Add red command-schema tests**

Add exact registry/ingress cases for:

```json
{"type":"crud_self","v":1,"round":34,"peer":"A1B2C3","session":"07"}
```

The schema accepts exactly five members, requires those exact types and
widths, rejects duplicate keys, extra keys, lowercase hex, zero peer/session,
wrong version/round, whitespace inside values, escaped-NUL strings, and
suffixes. Deployment policy sends it to both slots; BLE-primary installs and
acknowledges it, while Wi-Fi-primary validates and ignores it.

- [ ] **Step 2: Run scanner registry tests and capture the red result**

Run:

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final/esp32
/Users/billh/.platformio/penv/bin/pio test -e test -f test_scanner_command_schema_registry
/Users/billh/.platformio/penv/bin/pio test -e test -f test_scanner_command_ingress
```

Expected: `crud_self` is rejected or unrecognized.

- [ ] **Step 3: Implement the observer and hook both NimBLE discovery events**

In both `BLE_GAP_EVENT_DISC` and `BLE_GAP_EVENT_EXT_DISC`, call
`badge_con_observer_consume()` before `badge_ble_note_any_packet`, Remote ID,
fingerprints, investigation, and generic privacy handling. Return immediately
for both `BADGE_CON_FRAME_VALID` and `BADGE_CON_FRAME_INVALID`; only
`BADGE_CON_FRAME_NOT_GAME` may continue into existing detectors.

Preserve `BLE_SCAN_PASSIVE_MODE 0` for badge builds and preserve current GATT
investigation. The observer itself never advertises, connects, allocates,
logs, creates synchronization objects, or writes UART from the callback.

- [ ] **Step 4: Implement one coalesced pending packet and exact self ack**

The BLE-primary scanner module owns one static observer and stores at most one
pending qualified packet. A newer qualified packet replaces the pending game
packet. Protect the fixed packet copy and `pending_valid` with one static
`portMUX_TYPE`; the callback holds the critical section only for the bounded
copy and never waits on a task primitive. The UART task calls
`badge_con_observer_take_pending()` directly on each eligible loop, so there
is no pointer-ownership ambiguity or wakeup flag to lose.

`uart_tx` renders at most one `FOF_CRUD` line per TX-loop iteration only when:

```c
scanner_data_tx_allowed() &&
!uart_ota_is_active_snapshot() &&
!uart_tx_firmware_quiet_window_active() &&
uxQueueMessagesWaiting(detection_queue) == 0
```

Add an atomic `s_firmware_quiet_window_active` in `uart_tx.c`, export
`uart_tx_set_firmware_quiet_window(bool)`, and update it at every existing
`scanner_firmware_quiet_window_active()` transition in `main.c`.
`uart_tx_firmware_quiet_window_active()` remains file-static and nonblocking.
Command responses, OTA frames, scanner identity, and normal detections retain
their current priority. On successful `crud_self` installation, enqueue this
exact allocation-free acknowledgment through the command-response path:

```json
{"type":"crud_self_ack","v":1,"round":34,"peer":"A1B2C3","session":"07"}
```

The Wi-Fi-primary scanner emits no ack and no game evidence.

- [ ] **Step 5: Add the uplink slot-0 fast path**

Register `crud_self_ack` in `scanner_uplink_ingress_registry`. In
`uart_rx.c::process_line`, check a claimed `FOF_CRUD:` prefix before Easter,
cJSON, and general scanner ingress. Parse by explicit byte count, accept only
scanner slot `0`, reject the current local peer/session as defense in depth,
and pass a stack-local `badge_con_packet_t` directly to
`badge_con_runtime_apply_qualified_peer()`. Any malformed claimed game line is
consumed and rejected; it cannot become a normal detection or JSON frame.

- [ ] **Step 6: Add source-contract and behavior tests**

The observer tests prove valid, malformed, non-game, self, weak, duplicate,
quorum, peer-table, pending-coalescing, bounded critical-section handoff, and
concurrent publish/take behavior. The Python contract test
must assert the game hook precedes existing BLE detection calls and that the
callback slice contains none of:

```text
cJSON
malloc
calloc
realloc
xQueueCreate
xSemaphoreCreate
uart_write_bytes
ESP_LOG
```

It must also assert `FOF_CRUD:` is checked before `cJSON_Parse` and
`fof_scanner_uplink_ingress_select_and_validate`.

- [ ] **Step 7: Run focused native and backend tests**

Run:

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final/esp32
/Users/billh/.platformio/penv/bin/pio test -e test -f test_badge_con_observer
/Users/billh/.platformio/penv/bin/pio test -e test -f test_scanner_command_schema_registry
/Users/billh/.platformio/penv/bin/pio test -e test -f test_scanner_command_ingress
/Users/billh/.platformio/penv/bin/pio test -e test -f test_scanner_uplink_ingress_registry
cd ..
python -m pytest backend/tests/test_uplink_uart_ingress_contract.py -q
```

Expected: all commands pass.

- [ ] **Step 8: Checkpoint the task without committing**

Run:

```bash
git diff --check -- esp32/scanner/main/detection/badge_con_observer.h esp32/scanner/main/detection/badge_con_observer.c esp32/scanner/main/detection/ble_remote_id.c esp32/scanner/main/comms/uart_tx.h esp32/scanner/main/comms/uart_tx.c esp32/shared/scanner_command_schema_registry.h esp32/shared/scanner_command_schema_registry.c esp32/shared/scanner_command_ingress.h esp32/shared/scanner_command_ingress.c esp32/shared/scanner_uplink_ingress_registry.h esp32/shared/scanner_uplink_ingress_registry.c esp32/uplink/main/comms/uart_rx.c
```

Expected: no whitespace errors.

### Task 5: Uplink Game Runtime, Atomic Persistence, USB Seed Command, and Status

**Files:**

- Create: `esp32/uplink/main/core/badge_con_runtime.h`
- Create: `esp32/uplink/main/core/badge_con_runtime.c`
- Modify: `esp32/uplink/main/core/nvs_config.h`
- Modify: `esp32/uplink/main/core/nvs_config.c`
- Modify: `esp32/uplink/main/core/serial_config_ingress.c`
- Modify: `esp32/uplink/main/core/serial_config.c`
- Modify: `esp32/uplink/main/core/badge_runtime.h`
- Modify: `esp32/uplink/main/core/badge_runtime.c`
- Modify: `esp32/uplink/main/network/http_status.c`
- Modify: `esp32/uplink/main/main.c`
- Modify: `esp32/uplink/main/CMakeLists.txt`
- Create: `esp32/test/test_badge_con_runtime_policy.c`
- Modify: `esp32/test/test_badge_usb_phase_a_ingress.c`
- Modify: `backend/tests/test_badge_http_status_contract.py`
- Modify: `backend/tests/test_badge_firmware_transport_contract.py`
- Modify: `esp32/platformio.ini`
- Modify: `esp32/test/test_runner.c`

**Interfaces:**

- Consumes: Task 2 game state and record codecs; existing `nvs_config_*`,
  `badge_runtime_last_reset_expected()`, and expected-reboot evidence.
- Produces:

```c
void badge_con_runtime_init(void);
bool badge_con_runtime_set_factory_seed(badge_con_role_t seed);
bool badge_con_runtime_activate_after_easter(void);
badge_con_effect_t badge_con_runtime_apply_qualified_peer(
    const badge_con_packet_t *packet);
bool badge_con_runtime_snapshot(badge_con_snapshot_t *out);
bool badge_con_runtime_identity(uint32_t *peer_out,
                                uint8_t *session_out);
bool badge_con_runtime_self_ack_matches(uint32_t peer,
                                        uint8_t session);
void badge_con_runtime_note_self_ack(uint32_t peer,
                                     uint8_t session);

typedef void (*badge_runtime_expected_reboot_hook_t)(
    uint32_t expected_reboot_generation);
void badge_runtime_set_expected_reboot_hook(
    badge_runtime_expected_reboot_hook_t hook);
uint32_t badge_runtime_last_expected_reboot_generation(void);
```

- [ ] **Step 1: Write red atomic-seed and status-contract tests**

The native seam must prove:

```c
s_state = (badge_con_game_state_t) {
    .seed = BADGE_CON_ROLE_INFECTED,
    .role = BADGE_CON_ROLE_INFECTED,
    .active = true,
    .shield = 70,
    .last_decay_ms = 1000,
};
s_initialized = true;
s_nvs_write_ok = true;
TEST_ASSERT_TRUE(badge_con_runtime_set_factory_seed(
    BADGE_CON_ROLE_IMMUNE));
badge_con_snapshot_t after = {0};
TEST_ASSERT_TRUE(badge_con_runtime_snapshot(&after));
TEST_ASSERT_EQUAL(BADGE_CON_ROLE_IMMUNE, after.seed);
TEST_ASSERT_EQUAL(BADGE_CON_ROLE_IMMUNE, after.role);
TEST_ASSERT_FALSE(after.active);
TEST_ASSERT_EQUAL_UINT8(0, after.shield);
```

`test_badge_con_runtime_policy.c` includes
`../uplink/main/core/badge_con_runtime.c` directly under `UNIT_TESTING` with
`FOF_DC34_GAME_CANARY=1`, stubs
`nvs_config_read_blob`, `nvs_config_set_blob`, expected-reboot evidence,
`esp_fill_random`, `esp_timer_get_time`, and critical-section primitives, and
records every persistence call. This keeps production ESP-IDF dependencies
out of the native link while exercising the real runtime adapter.

The USB ingress tests require only `normal`, `infected`, and `immune` for
`FOF_SET:game_seed=` and reject case changes, whitespace, aliases, embedded
NUL, suffixes, and unknown keys.

- [ ] **Step 2: Run focused tests and capture the red results**

Run:

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final/esp32
/Users/billh/.platformio/penv/bin/pio test -e test -f test_badge_con_runtime_policy
/Users/billh/.platformio/penv/bin/pio test -e test -f test_badge_usb_phase_a_ingress
```

Expected: missing game runtime and rejected `game_seed`.

- [ ] **Step 3: Implement the sole game-state owner**

Compile `badge_con_runtime.c`, its `main.c` initialization, `game_seed`
mutation/status fields, and NVS/RTC hooks only for
`FOF_DC34_GAME_CANARY=1`. The unchanged `.78` production environment must
neither link the module nor expose the new command/status.

Use one static state and one `portMUX_TYPE`. Read/write the single NVS key
`game_state_v1` through `nvs_config_read_blob()` and
`nvs_config_set_blob()`. Initialization rules are exact:

1. Decode NVS. `BADGE_CON_NVS_VALID` restores the record,
   `BADGE_CON_NVS_SEED_ONLY` restores configured seed as current role with an
   inactive game and zero shield, and missing/fully invalid data becomes
   `normal/normal/false/0`.
2. Restore exact RTC data only when its CRC/generation is valid and
   `badge_runtime_last_reset_expected()` is true. Compare against
   `badge_runtime_last_expected_reboot_generation()`.
3. Generate a nonzero random 24-bit peer, nonzero session byte, and sequence
   start without persistence or display.
4. Write the RTC record after every mutation.
5. Commit role and activation changes immediately.
6. Commit shield only when `badge_con_game_shield_checkpoint()` changes.
7. Never hold the runtime critical section while calling NVS.

Extend the existing RTC expected-reboot marker with a nonzero generation.
`badge_runtime_arm_expected_reboot()` increments it, finishes arming the
expected-reset marker, then invokes the registered hook outside every runtime
lock. `badge_con_runtime_init()` runs immediately after
`badge_runtime_init()`, registers the hook, and the hook rewrites the exact
game RTC record with the armed generation before the caller can restart.
During the next boot, `badge_runtime_init()` captures the consumed generation
before clearing the expected marker. Cold/invalid markers expose generation
zero and cannot authorize exact RTC restore.

For persistence failure, keep the prior in-RAM snapshot and return `false`.

- [ ] **Step 4: Implement exact `FOF_SET:game_seed` and read-only status**

Special-case `game_seed` before the generic string setter. On success emit
exactly:

```text
FOF_OK:game_seed
```

On failure emit the existing `FOF_ERROR:` prefix and do not mutate state.
`FOF_SAVE` is not part of the transaction.

Both full and low-memory `FOF_STATUS` serializers, plus read-only HTTP badge
status, include:

```json
{
  "game_seed": "infected",
  "game_state": "infected",
  "game_active": false,
  "game_shield": 0
}
```

Use one runtime snapshot per response. Add no HTTP mutation route and expose
no peer, session, MAC, or stable badge identifier.

- [ ] **Step 5: Add persistence failure, reboot, and status parity tests**

Cover absent/corrupt NVS, exact expected-reboot RTC restore, cold/panic reset
falling back to NVS decile, failed blob write preserving RAM, activation
commit, role-change commit, shield-decile commit, full/fallback USB parity,
HTTP parity, early-status defaults before runtime initialization, generation
increment/wrap-to-one, hook ordering after marker arm, matching-generation
restore, and stale-generation rejection.

- [ ] **Step 6: Run focused, full native, and status tests**

Run:

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final/esp32
/Users/billh/.platformio/penv/bin/pio test -e test -f test_badge_con_runtime_policy
/Users/billh/.platformio/penv/bin/pio test -e test -f test_badge_usb_phase_a_ingress
/Users/billh/.platformio/penv/bin/pio test -e test
cd ..
python -m pytest backend/tests/test_badge_http_status_contract.py backend/tests/test_badge_firmware_transport_contract.py -q
```

Expected: all commands pass.

- [ ] **Step 7: Checkpoint the task without committing**

Run:

```bash
git diff --check -- esp32/uplink/main/core/badge_con_runtime.h esp32/uplink/main/core/badge_con_runtime.c esp32/uplink/main/core/nvs_config.h esp32/uplink/main/core/nvs_config.c esp32/uplink/main/core/serial_config_ingress.c esp32/uplink/main/core/serial_config.c esp32/uplink/main/core/badge_runtime.h esp32/uplink/main/core/badge_runtime.c esp32/uplink/main/network/http_status.c esp32/uplink/main/main.c
```

Expected: no whitespace errors.

### Task 6: Easter Activation, Interface-Preserving Game Chrome, and Reset Priority

**Files:**

- Modify: `esp32/uplink/main/core/badge_easter_egg_runtime.c`
- Modify: `esp32/uplink/main/hw/display_st7735.c`
- Modify: `esp32/shared/badge_theme.h`
- Modify: `esp32/shared/badge_theme.c`
- Modify: `esp32/test/test_badge_easter_egg.c`
- Create: `esp32/test/test_badge_easter_egg_runtime.c`
- Modify: `esp32/test/test_badge_theme.c`
- Modify: `esp32/test/test_badge_power_chord.c`
- Modify: `esp32/platformio.ini`
- Modify: `esp32/test/test_runner.c`
- Modify: `backend/tests/test_badge_firmware_transport_contract.py`

**Interfaces:**

- Consumes: `badge_con_runtime_activate_after_easter()` and
  `badge_con_runtime_snapshot()` from Task 5; existing
  `badge_power_chord_update()`.
- Produces:

```c
typedef struct {
    uint16_t chrome_primary;
    uint16_t chrome_secondary;
    uint16_t chrome_accent;
    uint16_t chrome_text;
} badge_con_render_palette_t;

void badge_theme_derive_con_palette(
    const badge_theme_t *selected,
    badge_con_role_t role,
    badge_con_render_palette_t *out);
```

- [ ] **Step 1: Write red activation and render-palette tests**

Add a runtime test that includes
`../uplink/main/core/badge_easter_egg_runtime.c` under `UNIT_TESTING`, stubs
the monotonic clock and `badge_con_runtime_activate_after_easter()`, and proves
that only a terminal successful dismiss activates the game:

```c
s_now_ms = 1000;
badge_easter_egg_runtime_init();
TEST_ASSERT_TRUE(badge_easter_egg_runtime_trigger(
    BADGE_EASTER_EGG_SOURCE_WIFI_SSID));
TEST_ASSERT_EQUAL_UINT(0, s_game_activation_calls);
s_now_ms = 2000;
TEST_ASSERT_TRUE(badge_easter_egg_runtime_advance());
TEST_ASSERT_EQUAL_UINT(0, s_game_activation_calls);
s_now_ms = 3000;
TEST_ASSERT_TRUE(badge_easter_egg_runtime_advance());
TEST_ASSERT_EQUAL_UINT(1, s_game_activation_calls);
TEST_ASSERT_FALSE(badge_easter_egg_runtime_dismiss());
TEST_ASSERT_EQUAL_UINT(1, s_game_activation_calls);
```

Add a separate direct-dismiss test that triggers THANKS, calls
`badge_easter_egg_runtime_dismiss()`, and observes exactly one activation.
Inside both `badge_easter_egg_runtime_advance()` and
`badge_easter_egg_runtime_dismiss()`, detect a successful transition from a
visible presentation to `BADGE_EASTER_EGG_PHASE_CONSUMED`, retain its source,
release the Easter runtime critical section, and then call
`badge_con_runtime_activate_after_easter()`. The first THANKS-to-BOUNCE
advance does not activate. No NVS/game call occurs while the Easter lock is
held.

Add palette assertions that normal retains selected colors, infected derives
purple/green chrome, immune derives pink chrome, and no derived palette writes
the persisted theme. At brightness `100`, infected uses purple `0x79DD` and
green `0x3FE2`; immune uses pink `0xF9F5` and magenta `0xF81F`. Pass those
role constants through `badge_theme_apply_brightness()` and
`badge_theme_contrast_floor()` rather than adding palette fields or changing
the stored theme.

- [ ] **Step 2: Run focused tests and capture the red results**

Run:

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final/esp32
/Users/billh/.platformio/penv/bin/pio test -e test -f test_badge_easter_egg
/Users/billh/.platformio/penv/bin/pio test -e test -f test_badge_easter_egg_runtime
/Users/billh/.platformio/penv/bin/pio test -e test -f test_badge_theme
```

Expected: missing activation/palette seam.

- [ ] **Step 3: Hook terminal dismiss and preserve the existing interface**

Guard every Easter-to-game hook, game snapshot, derived-color call, and HUD
render with `FOF_DC34_GAME_CANARY=1`. The `.78` production render and button
behavior remain byte-for-byte on their existing paths.

Call `badge_con_runtime_activate_after_easter()` only after either runtime
advance or runtime dismiss successfully changes the machine to
`BADGE_EASTER_EGG_PHASE_CONSUMED`. Do not activate on trigger,
THANKS-to-BOUNCE advance, a failed transition, or cooldown rejection.

Render the selected theme and current four-lane dashboard exactly as before.
For active infected/immune state, derive only chrome colors at render time.
Add `SHIELD %3u%%` to the existing bottom health strip without changing the
lane enum, source enum, threat ordering, screen navigation, theme schema,
custom palette storage, or Android wire format. Immune always renders `100%`.

- [ ] **Step 4: Move the chord decision ahead of all single-button actions**

Split button processing into two passes. The first pass samples raw levels and
updates both debounced stable states without dispatching Easter/menu/gesture
actions. Then evaluate:

```c
bool both_held = ok_pressed && menu_pressed;
bool chord_allowed =
    !buttons[0].boot_ignored && !buttons[1].boot_ignored;
badge_power_chord_event_t power_event = badge_power_chord_update(
    &power_chord, ok_pressed, menu_pressed, chord_allowed, now_ms);
bool suppress_single_button_dispatch =
    both_held || power_event == BADGE_POWER_CHORD_RESET;
```

When `power_event == BADGE_POWER_CHORD_RESET`, set both `consume_release`
flags, cancel the B2 gesture, and enter the existing
`badge_usb_transport_host_active(25)` branch and existing
`badge_usb_recovery_restart()` app/ROM confirmation flow unchanged. Do not
replace that flow with a direct `esp_restart()`. When both buttons are held
but 10 seconds has not elapsed, dispatch no single-button action. Only the
second pass dispatches recorded stable press/release edges when suppression is
false. Require full release before either single-button gesture becomes
eligible. Do not restore the removed power-off mode.

- [ ] **Step 5: Add source contracts for four-lane and theme stability**

The backend contract test hashes or enumerates the pre-existing lane/source
constants and asserts they are unchanged. It also asserts game rendering does
not call the NVS theme setter and that no fifth lane or game detection class
exists. A button source contract asserts both stable states and
`badge_power_chord_update()` are evaluated before
`badge_easter_egg_runtime_advance()`, and that the existing host-active USB
confirmation plus `BADGE_USB_RESET_ROM`/`BADGE_USB_RESET_APP` paths remain.

- [ ] **Step 6: Run focused and contract tests**

Run:

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final/esp32
/Users/billh/.platformio/penv/bin/pio test -e test -f test_badge_easter_egg
/Users/billh/.platformio/penv/bin/pio test -e test -f test_badge_easter_egg_runtime
/Users/billh/.platformio/penv/bin/pio test -e test -f test_badge_theme
/Users/billh/.platformio/penv/bin/pio test -e test -f test_badge_power_chord
cd ..
python -m pytest backend/tests/test_badge_firmware_transport_contract.py -q
```

Expected: all commands pass.

- [ ] **Step 7: Checkpoint the task without committing**

Run:

```bash
git diff --check -- esp32/uplink/main/core/badge_easter_egg_runtime.c esp32/uplink/main/hw/display_st7735.c esp32/shared/badge_theme.h esp32/shared/badge_theme.c esp32/test/test_badge_easter_egg.c esp32/test/test_badge_easter_egg_runtime.c esp32/test/test_badge_theme.c esp32/test/test_badge_power_chord.c
```

Expected: no whitespace errors.

### Task 7: Isolated `.79` Canary Environments and Badge Queue Reclamation

**Files:**

- Modify: `esp32/shared/version.h`
- Modify: `esp32/uplink/CMakeLists.txt`
- Modify: `esp32/scanner/CMakeLists.txt`
- Modify: `esp32/uplink/platformio.ini`
- Modify: `esp32/scanner/platformio.ini`
- Create: `esp32/uplink/sdkconfig.esp32s3-fof_badge-con-crud-canary.defaults`
- Create: `esp32/scanner/sdkconfig.scanner-s3-fof_badge-con-crud-canary.defaults`
- Modify: `esp32/uplink/main/main.c`
- Modify: `esp32/uplink/main/comms/uart_rx.c`
- Modify: `esp32/uplink/main/comms/uart_rx.h`
- Modify: `esp32/scripts/firmware_version.py`
- Modify: `esp32/scripts/pio_verify_badge_uplink_build.py`
- Modify: `esp32/scripts/pio_verify_badge_scanner_build.py`
- Modify: `backend/tests/test_firmware_build_version.py`
- Modify: `backend/tests/test_badge_firmware_transport_contract.py`
- Modify: `backend/tests/test_esp32_platformio_contract.py`
- Modify: `scripts/fof_badge_flash.py`
- Modify: `scripts/test_fof_badge_flash.py`

**Interfaces:**

- Consumes: existing verified-artifact snapshots and platform map.
- Produces:

```c
#define FOF_VERSION_BADGE "0.64.78-badge-defcon34"
#define FOF_VERSION_BADGE_CANARY "0.64.79-badge-defcon34"
```

and a new explicit host platform key:

```python
"badge-trio-xiao-s3-con-crud-canary": {
    "hardware": "FoF Badge trio CON CRUD canary on Seeed XIAO ESP32-S3",
    "uplink_env": "uplink-s3-fof_badge-con-crud-canary",
    "uplink_name": "uplink-s3-fof_badge",
    "uplink_project": "fof_badge_uplink",
    "uplink_bin": UPLINK_DIR / (
        ".pio/build/uplink-s3-fof_badge-con-crud-canary/firmware.bin"
    ),
    "scanner_env": "scanner-s3-combo-fof_badge-con-crud-canary",
    "scanner_name": "scanner-s3-combo-fof_badge",
    "scanner_project": "fof_badge_scanner",
    "scanner_bin": SCANNER_DIR / (
        ".pio/build/scanner-s3-combo-fof_badge-con-crud-canary/firmware.bin"
    ),
    "hardware_type": "seeed_xiao_esp32s3",
    "slots": ("ble", "wifi"),
    "version_macro": "FOF_VERSION_BADGE_CANARY",
}
```

- [ ] **Step 1: Write red environment/version isolation tests**

Assert:

```python
assert expected_identity_for_env(
    header, "uplink-s3-fof_badge"
).version == "0.64.78-badge-defcon34"
assert expected_identity_for_env(
    header, "uplink-s3-fof_badge-con-crud-canary"
).version == "0.64.79-badge-defcon34"
assert expected_identity_for_env(
    header, "scanner-s3-combo-fof_badge"
).version == "0.64.78-badge-defcon34"
assert expected_identity_for_env(
    header, "scanner-s3-combo-fof_badge-con-crud-canary"
).version == "0.64.79-badge-defcon34"
```

Assert the CLI default remains `badge-trio-xiao-s3`, and the canary is used
only through an explicit `--platform badge-trio-xiao-s3-con-crud-canary`.
Add `version_macro:"FOF_VERSION_BADGE"` to the existing production platform,
change `repo_version(platform)` to parse only that platform's exact macro,
and test that a canary invocation never reads `.78` while a production
invocation never reads `.79`.

- [ ] **Step 2: Run the focused host tests and capture the red result**

Run:

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final
python -m pytest backend/tests/test_firmware_build_version.py backend/tests/test_esp32_platformio_contract.py scripts/test_fof_badge_flash.py -q
```

Expected: unknown canary environment/platform/version track.

- [ ] **Step 3: Add the two canary environments without changing production**

Both canary build flags include:

```text
-DFOF_BADGE_VARIANT
-DFOF_DC34_GAME_CANARY=1
```

The uplink canary selects the same board, partition table, offsets, target
name, project name, and hardware type as production badge uplink, but its
layered sdkconfig enables controller-only VHCI BLE and disables NimBLE,
Bluedroid, BLE scanning, GATT, security, and connections.

The generated uplink canary sdkconfig must contain:

```text
CONFIG_BT_ENABLED=y
CONFIG_BT_CONTROLLER_ONLY=y
CONFIG_BT_CONTROLLER_ENABLED=y
CONFIG_BT_CTRL_HCI_MODE_VHCI=y
CONFIG_BT_CTRL_BLE_MAX_ACT=1
CONFIG_BT_CTRL_BLE_ADV=y
CONFIG_BT_CTRL_BLE_SCAN=n
CONFIG_BT_CTRL_BLE_MASTER=n
CONFIG_BT_BLUEDROID_ENABLED=n
CONFIG_BT_NIMBLE_ENABLED=n
```

The scanner canary selects the same board, partition table, offsets, target
name, project name, hardware type, active scanning, and one-image dual-role
behavior as production badge scanner. It adds no advertising role and keeps
current NimBLE host/GATT investigation settings.

Extend both post-build verifier scripts so exact production and exact canary
environment names receive immutable artifact-layout verification.
Extend uplink/scanner root `CMakeLists.txt` environment dispatch so the canary
project remains `fof_badge_uplink`/`fof_badge_scanner` while `PROJECT_VER`
comes from `FOF_VERSION_BADGE_CANARY`. Extend
`firmware_version.py::_TARGET_PROJECT_HARDWARE_TRACK` with both exact canary
environment names on a `badge_canary` track.

- [ ] **Step 4: Remove only the proven-unused canary badge detection queue**

Add a source-contract test proving the badge route calls
`badge_ingest_detection()` and returns before `xQueueSend`. Then, only under:

```c
#if defined(FOF_BADGE_VARIANT) && defined(FOF_DC34_GAME_CANARY)
```

skip the `xQueueCreate(48, sizeof(drone_detection_t))`, pass `NULL` into
`uart_rx_init()`, and make badge-only queue backpressure helpers return the
direct-ingest result without touching a queue. Production environments retain
capacity `48`. Status exposes:

```json
{"detection_queue_capacity":0,"detection_queue_reclaimed_bytes":41472}
```

for canary and the existing capacity for production.

- [ ] **Step 5: Run host contracts and all four badge builds**

Run:

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final
python -m pytest backend/tests/test_firmware_build_version.py backend/tests/test_esp32_platformio_contract.py backend/tests/test_badge_firmware_transport_contract.py scripts/test_fof_badge_flash.py -q
cd esp32
/Users/billh/.platformio/penv/bin/pio run -d uplink -e uplink-s3-fof_badge
/Users/billh/.platformio/penv/bin/pio run -d scanner -e scanner-s3-combo-fof_badge
/Users/billh/.platformio/penv/bin/pio run -d uplink -e uplink-s3-fof_badge-con-crud-canary
/Users/billh/.platformio/penv/bin/pio run -d scanner -e scanner-s3-combo-fof_badge-con-crud-canary
```

Expected: all tests/builds pass; production descriptors contain `.78`, canary
descriptors contain `.79`, and immutable artifact verification runs for all
four badge builds.

- [ ] **Step 6: Checkpoint the task without committing**

Run:

```bash
git diff --check -- esp32/shared/version.h esp32/uplink/CMakeLists.txt esp32/scanner/CMakeLists.txt esp32/uplink/platformio.ini esp32/scanner/platformio.ini esp32/scripts/firmware_version.py esp32/scripts/pio_verify_badge_uplink_build.py esp32/scripts/pio_verify_badge_scanner_build.py scripts/fof_badge_flash.py
```

Expected: no whitespace errors.

### Task 8: Controller-Only Uplink VHCI Advertiser

**Files:**

- Create: `esp32/uplink/main/game/badge_con_vhci.h`
- Create: `esp32/uplink/main/game/badge_con_vhci.c`
- Create: `esp32/shared/badge_con_vhci_policy.h`
- Create: `esp32/shared/badge_con_vhci_policy.c`
- Create: `esp32/test/test_badge_con_vhci_policy.c`
- Modify: `esp32/uplink/main/CMakeLists.txt`
- Modify: `esp32/uplink/main/main.c`
- Modify: `esp32/uplink/main/comms/uart_rx.c`
- Modify: `backend/tests/test_badge_firmware_transport_contract.py`
- Modify: `esp32/platformio.ini`
- Modify: `esp32/test/test_runner.c`

**Interfaces:**

- Consumes: Task 1 advertisement builder; Task 5 game runtime identity/state;
  Task 4 matching self ack; and Task 9 campaign inhibit interface.
- Produces:

```c
typedef enum {
    BADGE_CON_VHCI_OFF = 0,
    BADGE_CON_VHCI_INIT_CONTROLLER,
    BADGE_CON_VHCI_SET_PARAMS,
    BADGE_CON_VHCI_SET_DATA,
    BADGE_CON_VHCI_ENABLE,
    BADGE_CON_VHCI_ADVERTISING,
    BADGE_CON_VHCI_DISABLE,
    BADGE_CON_VHCI_FAILED,
} badge_con_vhci_state_t;

typedef struct {
    badge_con_vhci_state_t state;
    bool controller_initialized;
    bool advertising;
    bool inhibited;
    uint8_t sequence;
    uint32_t last_frame_ms;
    uint32_t command_deadline_ms;
    uint8_t retries;
    const char *failure;
} badge_con_vhci_snapshot_t;

bool badge_con_vhci_init(uint32_t peer, uint8_t session);
void badge_con_vhci_set_role(badge_con_role_t role);
void badge_con_vhci_set_game_active(bool active);
void badge_con_vhci_set_self_ready(bool ready);
void badge_con_vhci_set_inhibited(bool inhibited);
void badge_con_vhci_poll(uint32_t now_ms);
void badge_con_vhci_snapshot(badge_con_vhci_snapshot_t *out);
```

- [ ] **Step 1: Write red pure state-machine tests with a fake transport**

Define a fake transport that records opcodes and injects Command Complete
events. The first test expects this exact order:

```text
0x2006 LE Set Advertising Parameters
0x2008 LE Set Advertising Data
0x200A LE Set Advertising Enable
```

It also proves advertising remains off until game active, self command sent to
both scanner slots, exact BLE-primary ack received, and campaign state permits
radio.

- [ ] **Step 2: Run the focused test and capture the red result**

Run:

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final/esp32
/Users/billh/.platformio/penv/bin/pio test -e test -f test_badge_con_vhci_policy
```

Expected: missing VHCI policy/state functions.

- [ ] **Step 3: Implement the allocation-free HCI policy**

Use non-connectable/non-scannable legacy advertising type `0x03`, public own
address type, all three advertising channels `0x07`, no filter, and exact
minimum/maximum interval `1600` units (`1000 ms`). Validate
Command Complete/Status opcode and status for every command. Retry each
command at most twice with a bounded deadline; the third timeout/error enters
terminal `BADGE_CON_VHCI_FAILED`.

Once advertising, advance sequence and update `0x2008` data once per
one-second payload epoch. Repeated controller events within one epoch carry
the same sequence and are expected duplicates. Role changes update the next
epoch. Inhibit sends `0x200A` disable before reporting radio quiesced.

- [ ] **Step 4: Implement the ESP-IDF controller-only adapter**

The adapter uses only:

```c
BT_CONTROLLER_INIT_CONFIG_DEFAULT()
esp_bt_controller_init()
esp_bt_controller_enable(ESP_BT_MODE_BLE)
esp_vhci_host_register_callback()
esp_vhci_host_check_send_available()
esp_vhci_host_send_packet()
```

It uses fixed static command/event arrays and no host stack. Before controller
initialization, require at least `24576` bytes free internal heap and a
`16384`-byte largest internal block. A failed gate records terminal canary
status and never starts a Wi-Fi fallback.

Remove the current unconditional `#error` only for
`FOF_DC34_GAME_CANARY`; retain it for every other uplink environment.

- [ ] **Step 5: Integrate with the existing low-priority runtime loop**

Do not create a task. In the existing display/runtime poll:

1. snapshot game state and pass `snapshot.active` to
   `badge_con_vhci_set_game_active()`;
2. send `crud_self` to both slots until sent;
3. wait for the exact slot-0 `crud_self_ack`;
4. sample update/radio inhibition;
5. set role, active state, readiness, and inhibit;
6. call `badge_con_vhci_poll(now_ms)`.

After a scanner reboot identity-generation change, clear self readiness,
resend both self commands, and suspend advertising until a fresh matching ack
arrives.

- [ ] **Step 6: Add uplink no-host source contracts**

Assert the game advertiser contains none of:

```text
nimble_port
esp_nimble
esp_bluedroid
esp_ble_gatt
esp_ble_gap_start_scanning
esp_ble_gap_set_device_name
ble_gap_adv_start
ble_gap_connect
```

Assert no new task creation, dynamic allocation, or Wi-Fi fallback appears in
the game directory.

- [ ] **Step 7: Run focused tests and the canary uplink build**

Run:

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final/esp32
/Users/billh/.platformio/penv/bin/pio test -e test -f test_badge_con_vhci_policy
cd ..
python -m pytest backend/tests/test_badge_firmware_transport_contract.py -q
cd esp32
/Users/billh/.platformio/penv/bin/pio run -d uplink -e uplink-s3-fof_badge-con-crud-canary
```

Expected: all commands pass.

- [ ] **Step 8: Checkpoint the task without committing**

Run:

```bash
git diff --check -- esp32/uplink/main/game/badge_con_vhci.h esp32/uplink/main/game/badge_con_vhci.c esp32/shared/badge_con_vhci_policy.h esp32/shared/badge_con_vhci_policy.c esp32/test/test_badge_con_vhci_policy.c esp32/uplink/main/main.c
```

Expected: no whitespace errors.

### Task 9: Durable Coordinator Terminal Ordering and Fail-Busy Radio Inhibit

**Files:**

- Modify: `esp32/shared/firmware_auto_policy.h`
- Modify: `esp32/shared/firmware_auto_policy.c`
- Modify: `esp32/shared/firmware_operation_token.h`
- Modify: `esp32/shared/firmware_operation_token.c`
- Modify: `esp32/uplink/main/network/fw_store.h`
- Modify: `esp32/uplink/main/network/fw_store.c`
- Modify: `esp32/uplink/main/main.c`
- Modify: `esp32/test/test_firmware_auto_policy.c`
- Modify: `esp32/test/test_firmware_operation_token.c`
- Modify: `backend/tests/test_badge_firmware_transport_contract.py`

**Interfaces:**

- Consumes: current durable manifest/slot states and firmware operation token.
- Produces:

```c
typedef enum {
    FW_CAMPAIGN_IDLE = 0,
    FW_CAMPAIGN_OPERATION_ACTIVE,
    FW_CAMPAIGN_PENDING,
    FW_CAMPAIGN_ALL_TERMINAL,
    FW_CAMPAIGN_DEPENDENCY_DEFERRED,
    FW_CAMPAIGN_UNKNOWN,
} fw_store_campaign_state_t;

typedef struct {
    fw_store_campaign_state_t state;
    fw_operation_owner_t owner;
    uint32_t operation_epoch;
    uint32_t manifest_generation;
    bool radio_inhibited;
} fw_store_campaign_snapshot_t;

typedef enum {
    FW_UPDATE_PREEMPT_QUIESCED = 0,
    FW_UPDATE_PREEMPT_WAITING_FOR_OWNER,
    FW_UPDATE_PREEMPT_BUSY,
} fw_update_preempt_result_t;

bool fw_store_campaign_state_sample(
    fw_store_campaign_snapshot_t *out);
fw_update_preempt_result_t fw_store_request_update_preemption(void);
bool fw_store_game_radio_must_yield(void);
bool fw_store_start_auto_update_coordinator(void);
```

- [ ] **Step 1: Change the Wi-Fi gate test to the required terminal policy**

Replace the old failure expectations with:

```c
TEST_ASSERT_TRUE(fof_auto_wifi_gate_open(
    true, FOF_AUTO_SLOT_CONVERGED));
TEST_ASSERT_TRUE(fof_auto_wifi_gate_open(
    true, FOF_AUTO_SLOT_CURRENT));
TEST_ASSERT_TRUE(fof_auto_wifi_gate_open(
    true, FOF_AUTO_SLOT_REFUSED));
TEST_ASSERT_TRUE(fof_auto_wifi_gate_open(
    true, FOF_AUTO_SLOT_FAILED));
TEST_ASSERT_TRUE(fof_auto_wifi_gate_open(
    true, FOF_AUTO_SLOT_NEWER_SKIPPED));
TEST_ASSERT_TRUE(fof_auto_wifi_gate_open(
    true, FOF_AUTO_SLOT_EXCLUDED));
TEST_ASSERT_FALSE(fof_auto_wifi_gate_open(
    true, FOF_AUTO_SLOT_RECOVERING));
```

- [ ] **Step 2: Run focused tests and capture the red result**

Run:

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final/esp32
/Users/billh/.platformio/penv/bin/pio test -e test -f test_firmware_auto_policy
/Users/billh/.platformio/penv/bin/pio test -e test -f test_firmware_operation_token
```

Expected: Wi-Fi-after-failure assertions fail and campaign APIs are absent.

- [ ] **Step 3: Implement terminal ordering and a permanent coordinator task**

Treat `EXCLUDED`, `CONVERGED`, `CURRENT`, `REFUSED`, `FAILED`, and
`NEWER_SKIPPED` as terminal. Wi-Fi may begin after any terminal BLE outcome.

Replace create/delete cycles and `s_auto_relay_worker_running` with one
statically allocated coordinator task created once after scanner UART
dependencies exist. It blocks on task notification and moves through:

```text
DEPENDENCY_DEFERRED -> IDLE -> RUNNING -> QUIESCING -> SUSPENDED
```

`auto_coordinator_start_worker()` becomes a non-allocating notification kick.
A temporarily missing dependency never poisons a committed manifest or burns
an attempt. Excluded-slot release occurs after a complete work cycle.

Split initialization into two explicit phases. The existing
`fw_store_init_auto_update_coordinator()` may restore durable state and
initialize locks only; it must not create or notify a worker. In `main.c`, call
`fw_store_start_auto_update_coordinator()` exactly once and only after both
`uart_rx_scanner_tx_lease_init()` and `uart_rx_start()` have succeeded. A
missing UART dependency returns `false`, leaves the task uncreated, and changes
neither the committed manifest nor any retry counter.

- [ ] **Step 4: Close the staging-to-worker radio gap**

Increment an atomic operation epoch at every begin/end edge. Set the radio
inhibit latch inside every successful firmware-operation begin and before
staging releases its operation token. Restore the latch from any nonterminal
durable campaign before radio initialization.

`fw_store_campaign_state_sample()` performs:

1. read operation epoch/owner;
2. lock and copy coordinator state;
3. reread operation epoch;
4. retry at most three times on drift;
5. return `FW_CAMPAIGN_UNKNOWN` with `radio_inhibited=true` on lock failure or
   repeated drift.

Never take the FreeRTOS coordinator mutex while holding the critical-section
operation lock. Game radio is permitted only for `IDLE` or `ALL_TERMINAL`.

Clear the inhibit latch only while holding the coordinator lock and only when
the operation owner is `FW_OPERATION_NONE` and the durable coordinator state
is `IDLE` or `ALL_TERMINAL`. Increment and publish the operation epoch on that
clear edge. Explicit abort and terminal failure use the same transition, after
their durable terminal record is committed. `UNKNOWN`, dependency-deferred,
and any partial/nonterminal state keep the latch set.

- [ ] **Step 5: Add race, preemption, and retry tests**

Cover preemption before attempt reservation, after reservation but before OTA
bytes, during active relay, task dependency failure, staging-token release
before worker wake, operation epoch drift, lock failure, bounded retries,
terminal BLE failure opening Wi-Fi, terminal inhibit clearing, unknown state
remaining inhibited, startup before/after UART readiness, and no manifest
poisoning.

- [ ] **Step 6: Run focused, full native, and contract tests**

Run:

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final/esp32
/Users/billh/.platformio/penv/bin/pio test -e test -f test_firmware_auto_policy
/Users/billh/.platformio/penv/bin/pio test -e test -f test_firmware_operation_token
/Users/billh/.platformio/penv/bin/pio test -e test
cd ..
python -m pytest backend/tests/test_badge_firmware_transport_contract.py -q
```

Expected: all commands pass.

- [ ] **Step 7: Checkpoint the task without committing**

Run:

```bash
git diff --check -- esp32/shared/firmware_auto_policy.h esp32/shared/firmware_auto_policy.c esp32/shared/firmware_operation_token.h esp32/shared/firmware_operation_token.c esp32/uplink/main/network/fw_store.h esp32/uplink/main/network/fw_store.c esp32/uplink/main/main.c esp32/test/test_firmware_auto_policy.c esp32/test/test_firmware_operation_token.c
```

Expected: no whitespace errors.

### Task 10: Rebooted Update-Maintenance Firmware Mode

**Files:**

- Create: `esp32/shared/badge_update_maintenance_policy.h`
- Create: `esp32/shared/badge_update_maintenance_policy.c`
- Create: `esp32/test/test_badge_update_maintenance_policy.c`
- Modify: `esp32/uplink/main/core/badge_runtime.h`
- Modify: `esp32/uplink/main/core/badge_runtime.c`
- Modify: `esp32/uplink/main/core/badge_usb_control_schema.h`
- Modify: `esp32/uplink/main/core/badge_usb_control_schema.c`
- Modify: `esp32/uplink/main/core/serial_config_ingress.h`
- Modify: `esp32/uplink/main/core/serial_config_ingress.c`
- Modify: `esp32/uplink/main/core/serial_config.c`
- Modify: `esp32/uplink/main/core/badge_usb_transport.c`
- Modify: `esp32/uplink/main/core/uplink_usb_ota.h`
- Modify: `esp32/uplink/main/core/uplink_usb_ota.c`
- Modify: `esp32/shared/firmware_json_schema_registry.h`
- Modify: `esp32/shared/firmware_json_schema_registry.c`
- Modify: `esp32/uplink/main/CMakeLists.txt`
- Modify: `esp32/uplink/main/main.c`
- Modify: `esp32/uplink/main/network/fw_store.c`
- Modify: `esp32/test/test_badge_usb_control_schema.c`
- Modify: `esp32/test/test_badge_runtime_policy.c`
- Modify: `esp32/test/test_badge_usb_stream.c`
- Modify: `backend/tests/test_badge_firmware_transport_contract.py`
- Modify: `esp32/platformio.ini`
- Modify: `esp32/test/test_runner.c`

**Interfaces:**

- Consumes: Task 9 preemption/campaign API; existing expected-reboot and USB
  bounded-response infrastructure.
- Produces:

```c
bool badge_runtime_prepare_update(const char session[17]);
bool badge_runtime_update_maintenance_active(void);
bool badge_runtime_update_session_matches(const char session[17]);
void badge_runtime_update_keepalive(uint32_t now_ms);
bool badge_runtime_update_inactivity_due(uint32_t now_ms);
bool badge_runtime_clear_update_maintenance(const char *reason);
```

- [ ] **Step 1: Write red session, marker, and boot-decision tests**

Require exactly 16 uppercase hexadecimal characters. Reject lowercase,
whitespace, punctuation, 15/17 characters, embedded NUL, and all-zero session.
The versioned/checksummed RTC marker records session, phase
(`PREPARING`, `REBOOT_ARMED`, or `ACTIVE`), expected-reboot generation,
bounded boot count, the canonical uplink manifest/commit summary, and CRC.
Never persist a monotonic uptime or `last_keepalive` value across a reboot. On
every accepted maintenance boot, initialize the volatile last-activity
timestamp from that boot's current `now_ms`.

The boot policy accepts only expected software/USB-reset sequences. Brownout,
power-on, watchdog, panic, corrupt marker, exhausted boot count, and emergency
safe mode do not enter update maintenance.

- [ ] **Step 2: Run focused tests and capture the red result**

Run:

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final/esp32
/Users/billh/.platformio/penv/bin/pio test -e test -f test_badge_update_maintenance_policy
/Users/billh/.platformio/penv/bin/pio test -e test -f test_badge_usb_control_schema
```

Expected: missing maintenance policy and unrecognized `prepare_update`.

- [ ] **Step 3: Add exact prepare command and drain-before-reboot response**

Accept exactly:

```text
FOF_CTL:{"cmd":"prepare_update","session":"0123456789ABCDEF"}
```

For the first valid session, write the RTC `PREPARING` marker before latching
sticky update preemption. If marker persistence fails, return a terminal error
without changing preemption, the firmware-operation token, or radio policy.
Once accepted, a background owner keeps nonblockingly polling
`fw_store_request_update_preemption()` for that exact session even if the USB
response is dropped or the host disconnects. A repeated same-session command
only observes/advances that existing owner.

Map the nonblocking preemption result to an exact wire response.
`FW_UPDATE_PREEMPT_QUIESCED` or the internal
`FW_UPDATE_PREEMPT_REBOOT_SAFE` transition writes `REBOOT_ARMED` and emits:

```text
FOF_UPDATE_MODE:{"ok":true,"phase":"rebooting","session":"0123456789ABCDEF","retryable":true,"reboot_required":true}
```

`FW_UPDATE_PREEMPT_WAITING_FOR_OWNER` and `FW_UPDATE_PREEMPT_BUSY` return
immediately while retaining the same `PREPARING` marker:

```text
FOF_UPDATE_MODE:{"ok":false,"phase":"waiting_for_owner","session":"0123456789ABCDEF","retryable":true,"reboot_required":false,"error":"firmware_operation_active"}
FOF_UPDATE_MODE:{"ok":false,"phase":"busy","session":"0123456789ABCDEF","retryable":true,"reboot_required":false,"error":"campaign_state_busy"}
```

The host retries the same session every 250 ms under one absolute 30-second
prepare deadline; firmware never waits inside the USB command handler. The
deadline is host-owned: expiry stops that host attempt but does not cancel the
badge's accepted background transition. If a different preparing or
maintenance session is already active, return:

```text
FOF_UPDATE_MODE:{"ok":false,"phase":"busy","session":"0123456789ABCDEF","retryable":false,"reboot_required":false,"error":"session_conflict"}
```

where `session` is the newly requested value. Change neither the active marker
nor its session.

If the host reconnects after losing the reboot receipt and repeats
`prepare_update` with the exact session already active in maintenance, return
the idempotent observation:

```text
FOF_UPDATE_MODE:{"ok":true,"phase":"active","session":"0123456789ABCDEF","retryable":false,"reboot_required":false}
```

Do not request preemption or schedule another reboot for this observation.

Once no firmware operation owns mutation and the coordinator is suspended,
prefer an exact epoch-bound VHCI OFF acknowledgment. If shutdown cannot be
proved because the HCI state is terminally uncertain, its event queue
overflowed, or the display-side VHCI owner stopped progressing, return the
internal `FW_UPDATE_PREEMPT_REBOOT_SAFE` outcome. This permits only a reboot,
never a flash byte; reset itself is the RF cutoff.

Use the existing bounded required-response emission/drain seam. Schedule
`badge_runtime_arm_expected_reboot("update_maintenance")` after the required
emission attempt; a failed response or bounded drain never cancels the
already-owned reboot. The background owner must execute the same bounded
reboot path when no USB handler remains. Repeating the same session is
idempotent, and every accepted `PREPARING` session reaches an expected reboot.

- [ ] **Step 4: Implement the distinct maintenance boot**

Compile the entire maintenance branch only for
`FOF_DC34_GAME_CANARY`. Before large allocations or any game controller call,
release controller memory only when Bluetooth is present:

```c
#if defined(FOF_DC34_GAME_CANARY) && CONFIG_BT_ENABLED
esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
#endif
```

Add the same compile guard to `main.c`, `CMakeLists.txt`, and the contract
tests so the unchanged `.78` production environment neither references nor
links Bluetooth controller symbols.

Accept only a `REBOOT_ARMED` marker after the matching expected software/USB
reset evidence; a stranded `PREPARING` marker never enters maintenance. Promote
the accepted marker to `ACTIVE` before exposing the maintenance status. In this
boot, start USB control, LCD maintenance status, scanner UART drivers,
firmware staging, and the durable coordinator. Skip game runtime effects,
advertiser initialization, Wi-Fi/backend/AP, GPS, normal RF detections, and
normal dashboard rendering. Report:

```json
{
  "recovery_mode": "update_maintenance",
  "update_session": "0123456789ABCDEF",
  "ble_initialized": false,
  "update_uplink": {
    "phase": "idle",
    "session": "0123456789ABCDEF",
    "version": "",
    "sha256": "",
    "size": 0,
    "partition": "",
    "received": 0
  },
  "update_scanner": {
    "phase": "idle",
    "session": "0123456789ABCDEF",
    "target": "",
    "sha256": "",
    "size": 0,
    "slot_mask": 0,
    "received": 0,
    "generation": 0
  },
  "update_campaign": {
    "generation": 0,
    "target_slot_mask": 0,
    "pending_mask": 0,
    "worker_running": false,
    "readiness_probes": [0, 0],
    "scanners": [
      {"slot": 0, "attempts": 0, "state": "idle"},
      {"slot": 1, "attempts": 0, "state": "idle"}
    ]
  }
}
```

`update_uplink` always has exactly those seven members. `idle` uses the empty
string/zero values above. `receiving` reports the active session, exact target
version, canonical 64-character lowercase SHA-256, total size, inactive
partition label, and accepted byte count. `committed` reports the same exact
manifest identity with `received == size`; its summary survives the
same-session OTA reboot in the checksummed RTC marker. It is evidence for
identity/reconciliation, never authorization to resume at `received`.

`update_scanner` always has exactly the eight members above. `receiving`
reports the active parser identity with canonical lowercase SHA-256,
`0 <= received < size`, an exact nonzero target slot mask, and generation
zero. `committed` reports an exact durable generation/SHA-256/size/target/slot
mask with nonzero generation and `received == size`. A missing manifest is the
empty/zero `idle` shape. A torn or unavailable runtime/NVS snapshot is rendered
with phase `unknown` and zero identity; `unknown` is deliberately outside the
host's accepted phase set and therefore authorizes no bytes. This object is
also reconciliation evidence, never offset-resume authorization.

`update_campaign` always has exactly the six top-level members above and
exactly two scanner entries with the three members shown. After a committed
scanner stage, the host requires its generation and target mask to equal
`update_scanner`, waits for a stopped worker and zero pending mask, and accepts
only `converged` or `current` for requested lanes. `newer_skipped`, `refused`,
and `failed` are terminal failures: the host sends no `finish_update`,
firmware returns to normal mode, and no downgrade is attempted. Readiness
probes and attempts remain bounded by the coordinator retry policy. This
compact receipt replaces full scanner telemetry during maintenance:
normal-mode status before `prepare_update` and after `finish_update` remains
the authoritative proof of immutable scanner identity, role/profile, UART
ingress, physical radio health, and rollback clearance.

Emergency `safe_usb` remains a higher-priority mode and keeps its existing
semantics.

For a newly booted pending-verify app, use a reduced maintenance health gate.
After at least 10 seconds, require one completed USB response, a live LCD task,
both UART workers alive, no emergency safe mode, at least 24 KiB free internal
heap, and at least a 16 KiB largest internal block before calling
`esp_ota_mark_app_valid_cancel_rollback()`. If all gates are not true within
60 seconds, record a terminal update failure, make a bounded failure-response
attempt, clear the maintenance marker, and call
`esp_ota_mark_app_invalid_rollback_and_reboot()`; never mark the app valid on a
timeout or degraded gate.

- [ ] **Step 5: Gate all OTA/staging entrypoints**

Inside maintenance, permit uplink inactive-partition OTA and scanner staging.
Outside maintenance, `uplink_ota_begin` and scanner-stage begin reject before
calling any operation-token, preemption, radio, parser, partition, or staging
mutation. They accept no binary bytes. Uplink begin returns its existing exact nine-member
`FOF_UPLINK_OTA:` failure shape with `phase:"error"`, `partition:"none"`,
zero counts, `retryable:true`, `reboot_required:true`, and
`error:"update_maintenance_required"`. Scanner-stage begin returns its existing
exact `FOF_FW_UPLOAD:{"ok":false,"error":"update_maintenance_required"}` shape.
Neither path creates a session; the host must issue `prepare_update` first.

Inside maintenance, accept the exact matching-session
`FOF_CTL:{"cmd":"uplink_ota_abort","session":"0123456789ABCDEF"}` command.
Abort and erase only the current inactive-partition parser, retain the
maintenance marker, and emit the existing exact `FOF_UPLINK_OTA:` `aborted`
receipt. A missing parser is an idempotent aborted success; a session mismatch
changes nothing and returns the exact nonretryable `session_conflict` update
receipt.

Add these exact terminal controls:

```text
FOF_CTL:{"cmd":"finish_update","session":"0123456789ABCDEF"}
FOF_UPDATE_MODE:{"ok":true,"phase":"finishing","session":"0123456789ABCDEF","retryable":false,"reboot_required":true}

FOF_CTL:{"cmd":"abort_update","session":"0123456789ABCDEF"}
FOF_UPDATE_MODE:{"ok":true,"phase":"aborting","session":"0123456789ABCDEF","retryable":false,"reboot_required":true}
```

Both require an exact matching active session and use the same
required-response drain before expected reboot. `finish_update` fails busy
until all success gates are true. `abort_update` fails busy while a firmware
operation owns the mutation token.

The exact busy receipts are:

```text
FOF_UPDATE_MODE:{"ok":false,"phase":"busy","session":"0123456789ABCDEF","retryable":true,"reboot_required":false,"error":"success_gates_pending"}
FOF_UPDATE_MODE:{"ok":false,"phase":"busy","session":"0123456789ABCDEF","retryable":true,"reboot_required":false,"error":"firmware_operation_active"}
```

Refresh the volatile maintenance activity deadline after every validated
`FOF_PING`, `FOF_STATUS`, or control command; every accepted uplink-OTA or
scanner-stage begin/chunk/end; and each durable coordinator relay-progress
edge. The host sends a validated `FOF_PING` or `FOF_STATUS` at least every
15 seconds while idle or transferring. Define inactivity as 120 seconds since
the last refresh. Inactivity may clear maintenance only when no firmware
operation owns the token and the coordinator is neither running nor
recovering; otherwise it remains armed and the timer is refreshed by progress.

Keep the maintenance marker across the committed uplink OTA reboot. Clear it
only after rollback validity, both requested scanner lanes terminal, successful
lane identity/health/role/radio/rollback proof, and no operation owner.
Explicit abort, terminal failure, or bounded inactivity with no active
operation clears the marker and performs an expected reboot. Durable scanner
retry records remain intact.

- [ ] **Step 6: Add lifecycle and failure tests**

Cover exact response ordering,
`QUIESCED`/`REBOOT_SAFE`/`WAITING_FOR_OWNER`/`BUSY`, PREPARING persistence
before sticky preemption, marker failure changing no runtime state, repeated
same session, conflicting session, failed or dropped response delivery,
disconnect after the first waiting/busy receipt, background completion with no
host, HCI disable timeout, HCI event overflow, a stalled display-side owner,
monotonic keepalive rebasing, every activity refresh edge, 120-second
inactivity, same-session OTA reboot, exact idle/receiving/committed uplink
status, scanner staging, both terminal lanes, BLE never initialized, raw
normal-mode begin changing no radio/operation state, the 10/60-second
pending-verify gates, safe-mode precedence, host disappearance,
active-operation inactivity deferral, explicit abort, terminal failure, and
return to persisted game state.

- [ ] **Step 7: Run focused, full native, and contract tests**

Run:

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final/esp32
/Users/billh/.platformio/penv/bin/pio test -e test -f test_badge_update_maintenance_policy
/Users/billh/.platformio/penv/bin/pio test -e test -f test_badge_usb_control_schema
/Users/billh/.platformio/penv/bin/pio test -e test -f test_badge_runtime_policy
/Users/billh/.platformio/penv/bin/pio test -e test
cd ..
python -m pytest backend/tests/test_badge_firmware_transport_contract.py -q
```

Expected: all commands pass.

- [ ] **Step 8: Checkpoint the task without committing**

Run:

```bash
git diff --check -- esp32/shared/badge_update_maintenance_policy.h esp32/shared/badge_update_maintenance_policy.c esp32/uplink/main/core/badge_runtime.h esp32/uplink/main/core/badge_runtime.c esp32/uplink/main/core/badge_usb_control_schema.h esp32/uplink/main/core/badge_usb_control_schema.c esp32/uplink/main/core/serial_config_ingress.h esp32/uplink/main/core/serial_config_ingress.c esp32/uplink/main/core/serial_config.c esp32/uplink/main/core/badge_usb_transport.c esp32/uplink/main/core/uplink_usb_ota.h esp32/uplink/main/core/uplink_usb_ota.c esp32/uplink/main/CMakeLists.txt esp32/uplink/main/main.c
```

Expected: no whitespace errors.

### Task 11: Host Same-Uplink Reconnect and Byte-Zero Retry

**Files:**

- Modify: `scripts/fof_badge_flash.py`
- Modify: `scripts/test_fof_badge_flash.py`
- Modify: `scripts/test_fof_badge_flash_phase_a_json.py`
- Modify: `scripts/test_fof_badge_flash_phase_a_serial.py`
- Modify: `scripts/test_usb_descriptor_binding.py`
- Modify: `scripts/verify_badge_usb_hardening.py`
- Modify: `scripts/test_verify_badge_usb_hardening.py`

**Interfaces:**

- Consumes: Task 10 `prepare_update` protocol and maintenance status; existing
  immutable `UsbDescriptorRecord`, hardware ID, trusted USB location, frozen
  artifacts, and transactional upload methods.
- Produces:

```text
@dataclass(frozen=True)
class _PostUplinkExpectation:
    expected_hardware_id: str
    expected_version: str
    expected_partition: str
    expected_sha256: str
    expected_size: int
    pre_version: str | None
    pre_partition: str | None
    mutation_expected: bool
    source: str
    update_session: str

BadgeSerial.prepare_update_maintenance(
    self, session: str, *, deadline: float
) -> dict[str, Any]

BadgeSerial.reconnect_same_uplink(
    self, *, deadline: float
) -> dict[str, Any]

BadgeSerial.reconcile_uplink_ota(
    self, expected: Mapping[str, Any]
) -> Literal["committed", "restart_from_zero"]

BadgeSerial.reconcile_scanner_stage(
    self, expected: Mapping[str, Any]
) -> Literal["committed", "restart_from_zero"]
```

- [ ] **Step 1: Write red exact-schema and reconnect tests**

Add phase-specific schema IDs. The three successful receipts
`UPDATE_MODE_REBOOTING`, `UPDATE_MODE_FINISHING`, and
`UPDATE_MODE_ABORTING` each have exactly:

```python
{
    "ok": _HostJsonWireType.BOOL,
    "phase": _HostJsonWireType.ASCII_TOKEN,
    "session": _HostJsonWireType.ASCII_TOKEN,
    "retryable": _HostJsonWireType.BOOL,
    "reboot_required": _HostJsonWireType.BOOL,
}
```

`UPDATE_MODE_WAITING` and `UPDATE_MODE_BUSY` have those same five members plus
exactly:

```python
{"error": _HostJsonWireType.ASCII_TOKEN}
```

Value validators enforce these exact tuples:

```text
rebooting         ok=true  retryable=true  reboot_required=true
finishing         ok=true  retryable=false reboot_required=true
aborting          ok=true  retryable=false reboot_required=true
waiting_for_owner ok=false retryable=true  reboot_required=false
busy              ok=false retryable=true|false reboot_required=false
```

`waiting_for_owner` permits only `firmware_operation_active`. Retryable `busy`
permits only `campaign_state_busy`, `success_gates_pending`, or
`firmware_operation_active`; nonretryable `busy` permits only
`session_conflict`. Tests reject missing, duplicate, extra, wrong-type,
lowercase-session, an unrecognized phase/error, and every mismatched
ok/retryable/reboot tuple. Valid `ok:false` waiting/busy receipts and valid
`retryable:false` finishing/aborting receipts must pass.

- [ ] **Step 2: Run focused tests and capture the red result**

Run:

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final
python -m pytest scripts/test_fof_badge_flash_phase_a_json.py scripts/test_fof_badge_flash_phase_a_serial.py -q
```

Expected: missing phase-specific update-mode schemas and maintenance methods.

- [ ] **Step 3: Insert preparation after proof and before mutation**

In `_usb_flow_impl`, keep artifact freezing, selected-uplink application
proof, hidden hardware identity, trusted USB location, and pre-mutation
validator unchanged. Immediately afterward generate:

```python
session = secrets.token_hex(8).upper()
```

Require 16 uppercase hex and nonzero. Send the exact prepare command, validate
the exact response, and retry valid `waiting_for_owner` or retryable `busy`
receipts with the same session every 250 ms under the firmware's absolute
30-second preparation deadline. Treat `session_conflict`, malformed receipts,
or deadline expiry as terminal host failure. Only after an exact `rebooting`
receipt, close, wait for re-enumeration, and reconnect to the same hardware ID
at the same trusted location under the outer absolute update deadline. Prove
`recovery_mode == "update_maintenance"`, exact session, and `ble_initialized
is False` before sending any OTA/staging bytes.

For the initial canary migration only, if the proven source is exactly:

```text
version=0.64.78-badge-defcon34
firmware_name=uplink-s3-fof_badge
app_project=fof_badge_uplink
hardware_type=seeed_xiao_esp32s3
target_version=0.64.79-badge-defcon34
```

and `prepare_update` is rejected as an unknown command, use the existing
hardened direct inactive-partition uplink OTA. This exception is valid because
the exact `.78` build contract proves Bluetooth is compiled out. After fresh
`.79` application/rollback/USB health proof, generate a new session, issue
`prepare_update`, reconnect to `.79` maintenance, and only then stage scanner
bytes. Do not apply the exception to unknown, older, newer, same-version,
different-project, different-hardware, or Bluetooth-capable source images.

- [ ] **Step 4: Make reconnect tolerant of bounded boot races**

`reconnect_same_uplink()` loops over descriptor appearance, open failure,
startup logs, and status-not-ready races until the absolute deadline. Each
candidate must match both immutable serial/hardware ID and trusted location.
A different device, location change, malformed status, or deadline expiration
fails closed.

- [ ] **Step 5: Implement byte-zero reconciliation**

After a transport failure before a terminal receipt:

- Uplink reconciliation reads a fresh `FOF_STATUS` and strictly validates the
  Task 10 seven-member `update_uplink` object. Exact `committed`
  session/version/SHA-256/size/partition with `received == size` accepts the
  commit.
- Exact `receiving` for the same session and manifest sends:

  ```text
  FOF_CTL:{"cmd":"uplink_ota_abort","session":"0123456789ABCDEF"}
  ```

  and requires the existing exact `FOF_UPLINK_OTA:` aborted receipt with
  `ok:true`, `phase:"aborted"`, the reported inactive partition and total,
  `credit_bytes:0`, `retryable:true`, `reboot_required:false`, and an empty
  error before creating a fresh begin and retransmitting byte zero.
- Exact `idle` for the same maintenance session creates a fresh begin and
  retransmits byte zero. An uplink session/manifest/partition mismatch,
  malformed summary, impossible byte count, or ambiguous committed state
  fails closed.
- Scanner reconciliation accepts only an exact committed
  generation/SHA-256/size/target/slot mask. No active scanner parser and no
  matching commit creates a fresh begin and retransmits byte zero. An active
  mismatched parser, partial ambiguous commit, identity drift, or session
  mismatch fails closed.

Never resume `received` or an offset. Bound reconnects, dependency-reboot
retries, upload restarts, and total deadline. Send a validated `FOF_PING` or
`FOF_STATUS` at least every 15 seconds during host-side idle/reconnect waits;
accepted data chunks and coordinator progress satisfy the same keepalive
contract during transfer.

- [ ] **Step 6: Keep maintenance through both scanner lanes**

Scanner-only and full-badge USB flows both prepare maintenance. Preserve
`update_session` through uplink OTA reboot, scanner stage, BLE-lane terminal
proof, Wi-Fi-lane terminal proof, and final convergence. Send a new exact
`FOF_CTL:{"cmd":"finish_update","session":"<active-session>"}` control only
after rollback clearance, requested terminal lanes, fresh identities,
command/radio/role health, and no mutation owner. Require the exact
`FOF_UPDATE_MODE:` finishing response.
Reconnect after the expected final reboot and prove `recovery_mode:"normal"`
plus the prior persisted game state.

- [ ] **Step 7: Make terminal lane failure bounded and explicit**

On a terminal BLE- or Wi-Fi-lane failure, stop sending update bytes and never
send `finish_update`. Expect firmware to durably record the terminal failure,
clear maintenance, and re-enumerate in normal mode. Reconnect to the same
hardware ID and trusted location, prove the update session is absent,
`recovery_mode == "normal"`, the prior game state is restored, and the failed
lane's durable receipt is still reportable; then raise `FlashError` with that
receipt.

If no normal-mode re-enumeration occurs within 15 seconds and the same badge is
still reachable in the matching maintenance session, send the exact
`abort_update` command, accept only the exact `aborting` receipt, and wait for
the bounded normal reboot. A missing badge, changed identity/location, malformed
receipt, or second deadline expiry fails immediately. No failure path waits
forever or silently reports success.

- [ ] **Step 8: Add interruption and boundedness tests**

Cover unplug before uplink receipt, unplug after exact uplink commit, reboot
during scanner stage, reboot during BLE relay, BLE terminal failure followed
by Wi-Fi attempt, dependency-deferred coordinator, same-device rebind,
wrong-device appearance, location drift, stale session, ambiguous commit,
byte-zero retransmit, maintenance finish, exact `.78`-to-`.79` bootstrap
followed by mandatory maintenance before scanner bytes, rejection of every
near-miss bootstrap identity, automatic terminal-failure reboot, bounded abort
fallback, and no infinite loop.

- [ ] **Step 9: Run all host USB hardening tests**

Run:

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final
python -m pytest scripts/test_fof_badge_flash.py scripts/test_fof_badge_flash_phase_a_json.py scripts/test_fof_badge_flash_phase_a_serial.py scripts/test_usb_descriptor_binding.py scripts/test_verify_badge_usb_hardening.py -q
python scripts/verify_badge_usb_hardening.py
```

Expected: all tests and verifier checks pass.

- [ ] **Step 10: Checkpoint the task without committing**

Run:

```bash
git diff --check -- scripts/fof_badge_flash.py scripts/test_fof_badge_flash.py scripts/test_fof_badge_flash_phase_a_json.py scripts/test_fof_badge_flash_phase_a_serial.py scripts/test_usb_descriptor_binding.py scripts/verify_badge_usb_hardening.py scripts/test_verify_badge_usb_hardening.py
```

Expected: no whitespace errors.

### Task 12: Factory Role Selection, Fresh Reboot Proof, and Opaque Receipt

**Files:**

- Modify: `tools/badge_flasher/cli.py`
- Modify: `tools/badge_flasher/flash.py`
- Modify: `tools/badge_flasher/verify.py`
- Modify: `tools/badge_flasher/models.py`
- Modify: `tools/badge_flasher/records.py`
- Modify: `tools/badge_flasher/public_output.py`
- Modify: `tools/badge_flasher/tests/test_cli.py`
- Modify: `tools/badge_flasher/tests/test_flash.py`
- Modify: `tools/badge_flasher/tests/test_verify.py`
- Modify: `tools/badge_flasher/tests/test_records.py`
- Modify: `tools/badge_flasher/tests/test_redaction.py`
- Modify: `docs/badge-factory-flasher.md`
- Modify: `docs/badge/README.md`

**Interfaces:**

- Consumes: Task 5 exact seed command/status; existing erase-all flash/readback
  and complete runtime gate.
- Produces:

```text
GAME_SEEDS = ("normal", "infected", "immune")

provision_game_seed(
    uplink: UsbDevice,
    game_seed: str,
    *,
    timeout_s: float = 30,
    serial_factory: Callable[[str], Any] | None = None,
) -> None

verify_status(
    status: dict[str, Any],
    assignment: TopologyAssignment,
    version: str,
    game_seed: str,
) -> dict[str, Any]

wait_for_runtime(
    uplink: UsbDevice,
    assignment: TopologyAssignment,
    version: str,
    game_seed: str,
    *,
    timeout_s: float = 60,
    serial_factory: Callable[[str], Any] | None = None,
) -> dict[str, Any]

@dataclass(frozen=True, slots=True)
class BatchResult:
    badge_id: str
    version: str
    bundle_sha256: str
    passed: bool
    phase: str
    assignment: TopologyAssignment
    devices: Sequence[FlashEvidence]
    runtime: Mapping[str, Any]
    game_seed: str
    receipt: str | None
    error: str | None = None

ManufacturingLedger.record_failure(
    self,
    *,
    version: str,
    bundle_sha256: str,
    phase: str,
    error: str,
    game_seed: str,
) -> None
```

- [ ] **Step 1: Write red CLI and verifier tests**

Assert `--game-role` choices/default, show the selected role before Enter, and
require:

```python
assert status["game_seed"] == selected
assert status["game_state"] == selected
assert status["game_active"] is False
assert type(status["game_shield"]) is int
assert status["game_shield"] == 0
```

Boolean shield must fail because `bool` is a subclass of `int`.

- [ ] **Step 2: Run focused tests and capture the red result**

Run:

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final
python -m unittest tools.badge_flasher.tests.test_cli tools.badge_flasher.tests.test_verify -v
```

Expected: unknown CLI option and missing game-status validation.

- [ ] **Step 3: Implement the post-erase seed transaction**

After all three existing erase/write/readback steps:

1. reset scanner leaves in the existing order;
2. open the uplink native console and prove fresh `FOF_PONG`;
3. send `FOF_SET:game_seed=<selection>`;
4. require exact `FOF_OK:game_seed`;
5. send `FOF_REBOOT`;
6. require exact `FOF_REBOOT:OK`;
7. close and reopen;
8. rerun the complete runtime health gate with exact seed/current/inactive/zero
   proof.

Run this transaction for every batch, including the default `normal` choice;
never treat normal as “leave NVS unchanged.” Clear the serial input before
closing after `FOF_REBOOT:OK`, reopen a new handle, discard boot logs, and
require a response counter/boot identity newer than the pre-reboot evidence.

Any timeout, `FOF_ERROR`, wrong acknowledgment key, missing reboot receipt,
disconnect, missing field, wrong role, wrong type, or stale pre-reboot status
fails the batch. Released firmware that rejects `game_seed` fails rather than
downgrading the proof.

- [ ] **Step 4: Implement private ledger role and public random receipt**

Generate a receipt only after fresh PASS using cryptographic host randomness
and alphabet:

```python
CROCKFORD = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
receipt = "rcpt_" + "".join(
    secrets.choice(CROCKFORD) for _ in range(8)
)
```

Store selected role, receipt, and the four safe game fields in JSONL with
existing private hardware evidence. Preserve the current CSV header and row
shape as the append-only rework index. Failure records contain selected role
and `receipt:null`. Every `record_failure()` caller passes the already selected
`game_seed`, including failures that happen before topology assignment or
`BatchResult` construction.

Extend `runtime_evidence()`'s explicit allowlist with only:

```text
game_seed
game_state
game_active
game_shield
```

Nearby RF observations and unlisted status members remain excluded.

Replace public PASS with:

```text
PASS // GAME ROLE infected // RECEIPT rcpt_K7M2Q9W4
```

Never derive or display the receipt from MAC, `badge_id`, hardware ID, bundle
hash, time, role, or device state.

Route the entire operator transcript—not just PASS—through
`public_output.py` redaction. Replace raw-device references in `IDENT`,
`VERIFY`, `GRAPH`, `FLASH`, prompts, warnings, exceptions, and failure summaries
with fixed role aliases (`UPLINK`, `BLE-SCANNER`, `WIFI-SCANNER`, `BADGE`) plus
the opaque receipt only after PASS. Private JSONL keeps the existing hardware
evidence. No stdout or stderr path may render a MAC in colon, hyphen, dotted,
or compact form, a six-hex `badge_id`, native hardware ID, or bundle-derived
identifier.

- [ ] **Step 5: Add command-order, ledger, and redaction tests**

Tests prove exact UART bytes/order for all three selections, including an
active `FOF_SET:game_seed=normal` for the default; no PASS after seed failure;
rejection of a buffered pre-reboot `FOF_STATUS` delivered after
`FOF_REBOOT:OK`; acceptance only after a fresh boot/status proof; the four
game fields in `runtime_evidence()` and JSONL while unrelated fields remain
excluded; `game_seed` plumbing through every early `record_failure()` call;
unchanged CSV shape; receipt alphabet/length; no fabricated failure receipt;
no MAC-derived public label; and continued redaction of colon, hyphen, dotted,
and compact MAC forms. Capture complete stdout and stderr for both a successful
three-device run and failures in discovery, identity, graph, flash, verify,
seed, and ledger phases. Assert every line uses only the fixed role aliases and
contains no raw MAC, compact twelve-hex MAC, six-hex `badge_id`, or hardware
ID, while the corresponding private ledger fixture still retains its expected
hardware evidence.

- [ ] **Step 6: Document the canary/factory boundary**

Document the three roles, normal default, one scanner artifact, seed/reboot
proof, opaque receipt, and the fact that CLI/firmware support may land while
the embedded production factory ZIP remains unchanged. Physical canary role
tests use explicit local canary artifacts and USB seeding; embedded factory
output is not called game-capable until promotion.

- [ ] **Step 7: Run focused and full factory suites**

Run:

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final
python -m unittest tools.badge_flasher.tests.test_cli tools.badge_flasher.tests.test_verify -v
python -m unittest tools.badge_flasher.tests.test_records tools.badge_flasher.tests.test_redaction -v
python -m unittest discover -s tools/badge_flasher/tests -p 'test_*.py' -v
```

Expected: all tests pass.

- [ ] **Step 8: Checkpoint the task without committing**

Run:

```bash
git diff --check -- tools/badge_flasher/cli.py tools/badge_flasher/flash.py tools/badge_flasher/verify.py tools/badge_flasher/models.py tools/badge_flasher/records.py tools/badge_flasher/public_output.py tools/badge_flasher/tests/test_cli.py tools/badge_flasher/tests/test_flash.py tools/badge_flasher/tests/test_verify.py tools/badge_flasher/tests/test_records.py tools/badge_flasher/tests/test_redaction.py docs/badge-factory-flasher.md docs/badge/README.md
```

Expected: no whitespace errors.

### Task 13: Automated Regression, Memory, Artifact, and Promotion Block

**Files:**

- Modify: `esp32/scripts/verify_badge_uplink_build.py`
- Modify: `esp32/scripts/verify_badge_scanner_build.py`
- Modify: `scripts/verify_badge_usb_hardening.py`
- Modify: `scripts/test_verify_badge_usb_hardening.py`
- Modify: `.github/workflows/esp32-web-flasher.yml`
- Create: `docs/badge/con-crud-canary-acceptance.md`

**Interfaces:**

- Consumes: all preceding tasks.
- Produces: no firmware API; produces a fail-closed verification ledger and
  explicit canary acceptance document.

- [ ] **Step 1: Write red verifier tests for the memory/radio contract**

Require canary build metadata to prove:

```text
normal free internal heap >= 24576 bytes
normal largest internal block >= 16384 bytes
normal minimum-ever internal heap >= 12288 bytes
badge detection queue capacity == 0
uplink NimBLE == disabled
uplink Bluedroid == disabled
uplink BLE scan == disabled
uplink BLE connections == disabled
uplink VHCI == enabled
scanner BLE advertising == disabled
scanner active scanning == enabled
scanner GATT investigation == enabled
```

Require update-maintenance runtime samples to meet or exceed the recorded `.78`
pre-transfer free-heap and largest-block baseline. Preserve every current task
stack floor.

- [ ] **Step 2: Run verifier tests and capture the red result**

Run:

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final
python -m pytest backend/tests/test_firmware_build_version.py scripts/test_verify_badge_usb_hardening.py -q
```

Expected: canary memory/radio assertions are absent.

- [ ] **Step 3: Implement fail-closed build and runtime checks**

Parse the generated canary sdkconfig and map/size artifacts, require exact
controller-only settings, require immutable images/layouts, and reject any
production artifact containing `.79` or `FOF_DC34_GAME_CANARY`. Extend the USB
hardening verifier to capture normal and maintenance heap/stack/status fields
and apply the numeric thresholds.

- [ ] **Step 4: Add the CI tests but block publishing**

CI may build/test the canary environments and upload private workflow
artifacts. It must not copy them into `_site`, web-flasher manifests, release
assets, factory ZIPs, tags, or GitHub Releases. Add an assertion that the
publish/deploy job references only the unchanged production badge
environments.

- [ ] **Step 5: Run the complete automated gate**

Run:

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final/esp32
/Users/billh/.platformio/penv/bin/pio test -e test
/Users/billh/.platformio/penv/bin/pio run -d uplink -e uplink-s3
/Users/billh/.platformio/penv/bin/pio run -d uplink -e uplink-s3-fof_badge
/Users/billh/.platformio/penv/bin/pio run -d uplink -e uplink-s3-fof_badge-con-crud-canary
/Users/billh/.platformio/penv/bin/pio run -d scanner -e scanner-s3-combo
/Users/billh/.platformio/penv/bin/pio run -d scanner -e scanner-s3-combo-fof_badge
/Users/billh/.platformio/penv/bin/pio run -d scanner -e scanner-s3-combo-fof_badge-con-crud-canary
cd ..
python -m pytest backend/tests -q
python -m pytest scripts/test_fof_badge_flash.py scripts/test_fof_badge_flash_phase_a_json.py scripts/test_fof_badge_flash_phase_a_serial.py scripts/test_usb_descriptor_binding.py scripts/test_verify_badge_usb_hardening.py -q
python -m unittest discover -s tools/badge_flasher/tests -p 'test_*.py' -v
cd android
./gradlew testDebugUnitTest assembleDebug
```

Expected: every command passes. Record counts, binary sizes, map headroom,
normal/update heap gates, largest blocks, min-ever heap, and stack floors in
`docs/badge/con-crud-canary-acceptance.md`.

- [ ] **Step 6: Prove the release/factory artifacts remain unchanged**

Run:

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final
git diff --name-only -- esp32/web-flasher tools/badge_flasher/firmware .github/workflows
git status --short -- esp32/web-flasher tools/badge_flasher/firmware
```

Expected: no embedded firmware or web-flasher release artifact changed. The
workflow diff may contain only canary test/build steps with no deployment
input.

- [ ] **Step 7: Checkpoint the task without committing**

Run:

```bash
git diff --check
```

Expected: no whitespace errors anywhere in the dirty worktree.

### Task 14: Two-Badge Canary Flash and Physical Acceptance

**Files:**

- Modify: `docs/badge/con-crud-canary-acceptance.md`
- Do not modify production/factory firmware artifacts.

**Interfaces:**

- Consumes: explicit canary platform, USB maintenance update path, seed
  command, and all automated gates.
- Produces: physical evidence only; no release promotion.

- [ ] **Step 1: Resolve exactly two complete badge identities read-only**

With both badge uplinks connected, list application USB descriptors and prove
two distinct uplink hardware IDs and trusted USB locations. Do not expose
those IDs in public output or the acceptance document; label them `canary-A`
and `canary-B` only. Query fresh application/scanner status through each
uplink and require exact `.78` uplink plus both `.78` scanner identities before
counting the initial flash as a strict-newer cycle; a different starting
version stops the physical matrix and is recorded without mutation.

- [ ] **Step 2: Flash both badges exclusively through their uplink USB**

For each badge, run the hardened flasher with explicit:

```bash
python scripts/fof_badge_flash.py --platform badge-trio-xiao-s3-con-crud-canary --transport usb --only all --port <selected-uplink-port> --bind-selected-uplink
```

The command must update the uplink, stage the single scanner image, relay it to
both UART lanes, and prove all three `.79` applications. Direct scanner USB
flashing is forbidden for this acceptance step. This initial strict-newer
`.78` to `.79` operation is update cycle one for each badge.

- [ ] **Step 3: Seed roles and activate through an existing Easter path**

Set `canary-A` to infected and `canary-B` to immune using exact
`FOF_SET:game_seed=<selected-role>`, reboot, and prove inactive seeded status. Trigger
SSID `GameChangersAI-67` or the exact Hell Remote ID on each badge, dismiss
the Easter presentation, and prove `game_active:true`.

- [ ] **Step 4: Prove encounter rules and no false/self effects**

Verify:

1. neither badge encounters itself;
2. `-61 dBm` or weaker produces no state change;
3. two strong distinct packets produce no state change;
4. the third strong distinct packet applies the first effect;
5. immune proximity adds `10` cure per qualified packet to infected;
6. cure at `100` changes infected to normal with `50` shield;
7. infected proximity drains normal shield by `10`;
8. the zero-reaching packet is absorbed and the next qualified packet infects;
9. immune remains immune with `100%`;
10. the bottom HUD and purple/green or pink chrome appear without changing
    lane content or saved custom palettes.

- [ ] **Step 5: Prove normal badge behavior during the game**

On both badges verify DJI Wi-Fi detection, Remote ID, BLE privacy detections,
four-lane ordering, Android USB status, theme selection, custom palette,
display controls, and absence of game frames from normal detections. Confirm
BLE active scan-response/GATT privacy coverage remains operational.

- [ ] **Step 6: Prove update preemption and interruption recovery**

While both games are active:

1. update `canary-A` through its uplink USB with the explicit
   `--recovery-rewrite-same-version` contract; this is its cycle two;
2. prove advertising stops before OTA bytes;
3. prove maintenance re-enumeration and same-device binding;
4. unplug once during uplink upload and prove byte-zero retry;
5. reboot once during scanner relay and prove durable recovery;
6. prove BLE and Wi-Fi scanner lanes both reach terminal healthy `.79`;
7. prove prior game role/shield/activation returns after final reboot.

- [ ] **Step 7: Complete three consecutive update cycles per badge**

Count Task 14 Step 2 as cycle one on both badges and Step 6 as cycle two on
`canary-A`. Run the remaining one cycle on `canary-A` and two cycles on
`canary-B` with the flasher's explicit
`--recovery-rewrite-same-version` contract so both ledgers contain exactly
three consecutive complete cycles and the final installed version remains
`.79`. Every cycle must still rewrite one uplink and both scanners through the
single uplink USB and pass fresh identity/radio/rollback proof. Do not change
`FOF_VERSION_BADGE`, factory assets, tags, or release artifacts.

- [ ] **Step 8: Prove reset priority in four contexts**

Hold OK+Menu continuously for 10 seconds from the Easter presentation, active
game/dashboard, normal menu, and USB-attached state. Each context must reboot
once, ignore earlier single-button actions, require full release, and restore
persisted game state.

- [ ] **Step 9: Run the 30-minute simultaneous soak**

Keep game advertising, both scanners, LCD, and Android USB status/control
active for 30 minutes. Record start/end:

```text
free internal heap
largest internal block
minimum-ever internal heap
main/display/USB/BLE-UART/Wi-Fi-UART stack floors
crash count
watchdog count
detection queue capacity
game packet counts/drops
normal detection counts
```

Acceptance requires no crash, watchdog, boot loop, detection flood,
monotonic heap decline, or threshold violation.

- [ ] **Step 10: Close the canary ledger without promoting**

Record PASS/FAIL for every matrix item and all three update cycles in
`docs/badge/con-crud-canary-acceptance.md`. End with:

```text
PROMOTION STATUS: BLOCKED — awaiting explicit owner approval
FACTORY BUNDLE: unchanged
GITHUB RELEASE: not created
TAG/PUSH/MERGE: not performed
```

Do not call the firmware DEFCON-safe or factory-ready until the user
explicitly approves promotion after reviewing this ledger.

---

## Execution Order and Stop Conditions

Execute tasks in this dependency order:

```text
1 -> 2 -> 3 -> 7 -> 5 -> 6 -> 4 -> 9 -> 10 -> 11 -> 12 -> 8 -> 13 -> 14
```

Stop before any hardware mutation if a red test did not fail for the intended
missing behavior, any previously green test regresses, a production build
identity changes, a canary build misses its immutable artifact check, or a
memory threshold fails. Execute Task 14 only after the complete Task 13 gate
is green and exactly two intended badge uplinks are privately bound.

At every task boundary, inspect the scoped diff before continuing. Do not
clean, reset, stash, or rewrite the existing dirty worktree. If an edit
overlaps prior user work, preserve the prior behavior and extend the existing
test contract rather than replacing it.
