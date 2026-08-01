# Factory Flasher Per-Badge Final Promotion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the double-click macOS factory flasher prompt for every badge's role and flash the exact hardware-accepted `0.67.2-badge-defcon34` firmware from its validated offline bundle.

**Architecture:** Keep the existing three-board topology, direct-ROM flash, seed, reboot, and verification pipeline unchanged. Add one pure per-badge role-selection boundary in the host CLI, plus one explicit accepted-release profile in the deterministic bundle builder; then replace the embedded ZIP with an archive built from the already-accepted canary outputs.

**Tech Stack:** Python 3 `argparse`/`unittest`, existing `tools.badge_flasher` modules, deterministic ZIP packaging, zsh double-click wrapper, PlatformIO build outputs, Git.

## Global Constraints

- Uplink version must remain exactly `0.67.2-badge-defcon34`.
- Accepted uplink application must remain exactly 1,468,464 bytes with SHA-256 `78ef3b6dafe61e8e2fdc3fb28447372aaf76da38cd57ca0961828bbbdc08c434`.
- Accepted scanner application must remain exactly 1,216,800 bytes with SHA-256 `2d0e84501baf3bc929eed03a0b9c1f0272ed66baa9b81dd4513d6dc3fa2c032b`.
- Do not compile or edit firmware source, firmware versions, game balance, scanner behavior, display behavior, or USB/UART transport.
- Do not push, tag, merge, publish, create a GitHub release, or update the public web flasher.
- Preserve `.camera-before-zoom.jpg` as unrelated untracked user content.
- Interactive role selection has no default and occurs before every badge's first hardware probe.
- User-facing `HUMAN`, `INFECTED`, and `HEALER` map exactly to firmware seeds `normal`, `infected`, and `immune`.
- The existing JSONL ledger remains the private source of truth for role, three MACs, version, bundle hash, and receipt.
- Every production behavior change follows a witnessed red-green test cycle.

---

### Task 1: Per-Badge BBS Role Menu and Fail-Closed CLI

**Files:**
- Modify: `tools/badge_flasher/cli.py`
- Modify: `tools/badge_flasher/tests/test_cli.py`
- Modify: `tools/badge_flasher/tests/test_redaction.py`

**Interfaces:**
- Consumes: existing `_prompt_operator(message: str) -> str`, `paint()`, `phase()`, `GAME_SEEDS`, `run_one()`, and `ManufacturingLedger`.
- Produces: `prompt_game_role(plain: bool) -> str`; `run_one(..., game_role: str, ...) -> BatchResult`; parser default `game_role=None`.

- [ ] **Step 1: Add failing tests for numeric selection and confirmation**

Add focused tests to `tools/badge_flasher/tests/test_cli.py`:

```python
def test_prompt_game_role_maps_every_number_to_canonical_seed(self) -> None:
    for selected, expected in (("1", "normal"), ("2", "infected"), ("3", "immune")):
        with (
            mock.patch("builtins.input", side_effect=[selected, "y"]),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(cli.prompt_game_role(plain=True), expected)

def test_prompt_game_role_retries_invalid_selection_and_confirmation(self) -> None:
    transcript = io.StringIO()
    with (
        mock.patch(
            "builtins.input",
            side_effect=["", "9", "3", "maybe", "n", "2", "Y"],
        ),
        contextlib.redirect_stdout(transcript),
    ):
        self.assertEqual(cli.prompt_game_role(plain=True), "infected")
    self.assertGreaterEqual(transcript.getvalue().count("SELECT [1-3]"), 2)

def test_prompt_game_role_q_exits_without_a_default(self) -> None:
    with (
        mock.patch("builtins.input", return_value="q"),
        contextlib.redirect_stdout(io.StringIO()),
        self.assertRaises(KeyboardInterrupt),
    ):
        cli.prompt_game_role(plain=True)
```

Also assert that plain output contains `HUMAN`, `INFECTED`, `HEALER`,
`RED`, `GREEN`, and `HOT PINK`, while ANSI output contains an escape prefix.

- [ ] **Step 2: Run the menu tests and verify RED**

Run:

```bash
/Users/billh/.platformio/penv/bin/python -m unittest \
  tools.badge_flasher.tests.test_cli.CliSequenceTests.test_prompt_game_role_maps_every_number_to_canonical_seed \
  tools.badge_flasher.tests.test_cli.CliSequenceTests.test_prompt_game_role_retries_invalid_selection_and_confirmation \
  tools.badge_flasher.tests.test_cli.CliSequenceTests.test_prompt_game_role_q_exits_without_a_default -v
```

Expected: FAIL because `cli.prompt_game_role` does not exist.

- [ ] **Step 3: Implement the minimal role-menu helper**

Add the fixed choices near `_prompt_operator`:

```python
ROLE_MENU = {
    "1": ("HUMAN", "normal", RED),
    "2": ("INFECTED", "infected", GREEN),
    "3": ("HEALER", "immune", PURPLE),
}

def prompt_game_role(plain: bool) -> str:
    while True:
        print_user_visible(paint(
            "+==================================================+\n"
            "| GAMECHANGERS AI // SELECT NEXT BADGE             |\n"
            "+==================================================+\n"
            "| [1] HUMAN       // RED                           |\n"
            "| [2] INFECTED    // GREEN                         |\n"
            "| [3] HEALER      // HOT PINK                      |\n"
            "+==================================================+",
            CYAN + BOLD,
            plain,
        ))
        selected = _prompt_operator(
            paint("SELECT [1-3] > ", GOLD + BOLD, plain)
        ).strip().lower()
        if selected == "q":
            raise KeyboardInterrupt
        choice = ROLE_MENU.get(selected)
        if choice is None:
            phase("ROLE", "choose 1, 2, 3, or Q", plain)
            continue
        label, seed, color = choice
        while True:
            confirm = _prompt_operator(
                paint(
                    f"ARM NEXT BADGE AS {label}? [Y/N] > ",
                    color + BOLD,
                    plain,
                )
            ).strip().lower()
            if confirm == "y":
                return seed
            if confirm == "n":
                break
            phase("ROLE", "answer Y or N", plain)
```

Use the requested user-facing color words in the rendered text even though
the existing ANSI constants retain their terminal palette names.

- [ ] **Step 4: Run the menu tests and verify GREEN**

Run the three-test command from Step 2 plus the new plain/ANSI assertion.
Expected: all selected tests PASS.

- [ ] **Step 5: Add failing tests for per-iteration roles and unsafe automation**

Replace the old default-role test and add:

```python
def test_parser_has_no_silent_role_default(self) -> None:
    self.assertIsNone(cli.parser().parse_args([]).game_role)

def test_yes_requires_once_and_explicit_role_before_bundle_selection(self) -> None:
    for argv in (["--yes"], ["--yes", "--once"], ["--yes", "--game-role", "normal"]):
        with (
            mock.patch.object(cli, "choose_bundle") as choose_bundle,
            contextlib.redirect_stdout(io.StringIO()),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            self.assertEqual(cli.main(argv), 2)
        choose_bundle.assert_not_called()

def test_two_interactive_badges_use_two_independent_roles(self) -> None:
    assignments = (
        TopologyAssignment("A0:B1:C2:D3:E4:01", "A0:B1:C2:D3:E4:02", "A0:B1:C2:D3:E4:03"),
        TopologyAssignment("A0:B1:C2:D3:E4:11", "A0:B1:C2:D3:E4:12", "A0:B1:C2:D3:E4:13"),
    )
    roles: list[str] = []
    def fake_run(_args, _plain, bundle, *, game_role, **_kwargs):
        roles.append(game_role)
        index = len(roles) - 1
        return BatchResult(
            badge_id=f"badge-{index}", version=bundle.version,
            bundle_sha256=bundle.bundle_sha256, passed=True,
            phase="complete", assignment=assignments[index], devices=(),
            runtime={}, game_seed=game_role,
            receipt=f"rcpt_0000000{index}",
        )
    with (
        tempfile.TemporaryDirectory() as temp,
        mock.patch.object(cli, "choose_bundle", return_value=SimpleNamespace(
            version="0.67.2-badge-defcon34", bundle_sha256="f" * 64,
        )),
        mock.patch.object(cli, "prompt_game_role", side_effect=["normal", "immune", KeyboardInterrupt]),
        mock.patch.object(cli, "run_one", side_effect=fake_run),
        mock.patch("builtins.input", return_value=""),
        contextlib.redirect_stdout(io.StringIO()),
    ):
        self.assertEqual(cli.main(["--plain", "--offline", "--records", temp]), 130)
    self.assertEqual(roles, ["normal", "immune"])
```

Add a failure-path test proving the current iteration's role is passed to
`ledger.record_failure`, and an explicit `--game-role infected` test proving
the menu helper is never called.

- [ ] **Step 6: Run the new loop/safety tests and verify RED**

Run:

```bash
/Users/billh/.platformio/penv/bin/python -m unittest \
  tools.badge_flasher.tests.test_cli \
  tools.badge_flasher.tests.test_redaction -v
```

Expected: FAIL because the parser still defaults to `normal`, `run_one` reads
`args.game_role`, and the role remains outside the loop.

- [ ] **Step 7: Thread an explicit role through one badge operation**

Change the signature and all seed uses:

```python
def run_one(
    args: argparse.Namespace,
    plain: bool,
    bundle: FactoryBundle | None = None,
    *,
    game_role: str,
    forbidden_macs: set[str] | None = None,
    known_passed_macs: set[str] | None = None,
) -> BatchResult:
    if game_role not in GAME_SEEDS:
        raise ValueError("factory game role is invalid")
```

Replace all `args.game_role` references inside `run_one` with `game_role`.
Change `--game-role` to `default=None`.

Before banner, ledger construction, or bundle selection, reject:

```python
if args.yes and (not args.once or args.game_role is None):
    print_user_visible(
        "ERROR: --yes requires --once and an explicit --game-role",
        file=sys.stderr,
    )
    return 2
```

Inside each `while True` iteration, set:

```python
current_role = args.game_role or prompt_game_role(plain)
phase("ROLE", f"GAME ROLE {current_role}", plain)
```

Pass `game_role=current_role` into `run_one` and the same
`current_role` into `record_failure`. Catch `EOFError` and
`KeyboardInterrupt` around selection/connect/removal prompts and return `130`
without writing a fabricated failure. Remove the duplicate bottom connection
prompt; after a completed iteration, ask only for removal/continuation before
returning to the role menu.

- [ ] **Step 8: Run CLI/redaction tests and verify GREEN**

Run the command from Step 6.
Expected: all CLI and redaction tests PASS with no warning or traceback.

- [ ] **Step 9: Commit the menu task**

```bash
git add tools/badge_flasher/cli.py \
  tools/badge_flasher/tests/test_cli.py \
  tools/badge_flasher/tests/test_redaction.py
git commit -m "factory: prompt for every badge role"
```

---

### Task 2: Exact Accepted-Release Bundle Profile

**Files:**
- Modify: `scripts/build_badge_factory_bundle.py`
- Modify: `scripts/test_build_badge_factory_bundle.py`

**Interfaces:**
- Consumes: current PlatformIO output layouts and `parse_firmware_identity`.
- Produces: `build_bundle(output: Path, requested_version: str | None = None, profile: str = "production") -> Path`; CLI `--profile production|con-crud-0.67.2`; strict accepted app pins.

- [ ] **Step 1: Add failing behavioral tests for accepted pins**

Replace source-string-only coverage with import-based tests:

```python
from scripts import build_badge_factory_bundle as builder

def minimal_app(version: str, project: str, payload: bytes = b"") -> bytes:
    image = bytearray(0x20 + 112)
    image[0] = 0xE9
    struct.pack_into("<I", image, 0x20, 0xABCD5432)
    image[0x30:0x50] = version.encode().ljust(32, b"\0")
    image[0x50:0x70] = project.encode().ljust(32, b"\0")
    return bytes(image) + payload

def test_accepted_pin_rejects_wrong_size_hash_and_version(self) -> None:
    with tempfile.TemporaryDirectory() as temp:
        app = Path(temp) / "firmware.bin"
        data = minimal_app("0.67.2-badge-defcon34", "fof_badge_uplink")
        app.write_bytes(data)
        pin = builder.AcceptedApplication(
            version="0.67.2-badge-defcon34",
            size=len(data),
            sha256=hashlib.sha256(data).hexdigest(),
        )
        builder.verify_accepted_application(app, "fof_badge_uplink", pin)
        with self.assertRaisesRegex(RuntimeError, "size"):
            builder.verify_accepted_application(
                app, "fof_badge_uplink", dataclasses.replace(pin, size=len(data) + 1)
            )
        with self.assertRaisesRegex(RuntimeError, "SHA-256"):
            builder.verify_accepted_application(
                app, "fof_badge_uplink", dataclasses.replace(pin, sha256="0" * 64)
            )
        app.write_bytes(minimal_app("0.67.1-badge-defcon34", "fof_badge_uplink"))
        with self.assertRaisesRegex(RuntimeError, "version"):
            builder.verify_accepted_application(app, "fof_badge_uplink", pin)

def test_final_profile_names_exact_accepted_outputs(self) -> None:
    profile = builder.BUILD_PROFILES["con-crud-0.67.2"]
    self.assertEqual(profile["uplink"][0].name, "uplink-s3-fof_badge-con-crud-canary")
    self.assertEqual(profile["scanner"][0].name, "scanner-s3-combo-fof_badge-con-crud-canary")
    self.assertEqual(builder.ACCEPTED_APPLICATIONS["uplink"].sha256, "78ef3b6dafe61e8e2fdc3fb28447372aaf76da38cd57ca0961828bbbdc08c434")
    self.assertEqual(builder.ACCEPTED_APPLICATIONS["scanner"].sha256, "2d0e84501baf3bc929eed03a0b9c1f0272ed66baa9b81dd4513d6dc3fa2c032b")
```

- [ ] **Step 2: Run builder tests and verify RED**

Run:

```bash
/Users/billh/.platformio/penv/bin/python -m unittest \
  scripts.test_build_badge_factory_bundle -v
```

Expected: FAIL because `AcceptedApplication`, `BUILD_PROFILES`,
`ACCEPTED_APPLICATIONS`, and `verify_accepted_application` do not exist.

- [ ] **Step 3: Implement the accepted profile and pin verifier**

Add:

```python
from dataclasses import dataclass

@dataclass(frozen=True, slots=True)
class AcceptedApplication:
    version: str
    size: int
    sha256: str

ACCEPTED_VERSION = "0.67.2-badge-defcon34"
ACCEPTED_APPLICATIONS = {
    "uplink": AcceptedApplication(
        ACCEPTED_VERSION,
        1_468_464,
        "78ef3b6dafe61e8e2fdc3fb28447372aaf76da38cd57ca0961828bbbdc08c434",
    ),
    "scanner": AcceptedApplication(
        ACCEPTED_VERSION,
        1_216_800,
        "2d0e84501baf3bc929eed03a0b9c1f0272ed66baa9b81dd4513d6dc3fa2c032b",
    ),
}
```

Split the current `BUILDS` into `BUILD_PROFILES` with a shared probe:

```python
BUILD_PROFILES = {
    "production": {
        "uplink": (... / "uplink-s3-fof_badge", "uplink-s3-fof_badge", "fof_badge_uplink"),
        "scanner": (... / "scanner-s3-combo-fof_badge", "scanner-s3-combo-fof_badge", "fof_badge_scanner"),
    },
    "con-crud-0.67.2": {
        "uplink": (... / "uplink-s3-fof_badge-con-crud-canary", "uplink-s3-fof_badge", "fof_badge_uplink"),
        "scanner": (... / "scanner-s3-combo-fof_badge-con-crud-canary", "scanner-s3-combo-fof_badge", "fof_badge_scanner"),
    },
}
```

Keep the current probe tuple in every selected build mapping. Implement:

```python
def verify_accepted_application(
    path: Path,
    expected_project: str,
    pin: AcceptedApplication,
) -> None:
    identity = parse_firmware_identity(path.read_bytes())
    if identity is None or identity.project != expected_project:
        raise RuntimeError(f"{path}: project identity mismatch")
    if identity.version != pin.version:
        raise RuntimeError(f"{path}: accepted version mismatch")
    if path.stat().st_size != pin.size:
        raise RuntimeError(f"{path}: accepted size mismatch")
    if sha256(path) != pin.sha256:
        raise RuntimeError(f"{path}: accepted SHA-256 mismatch")
```

Pass the selected build mapping into `layout_for`. When profile
`con-crud-0.67.2` is selected, call the verifier before copying either app.
Add `profile` to `build_bundle`, and add:

```python
parser.add_argument(
    "--profile",
    choices=tuple(BUILD_PROFILES),
    default="production",
)
```

- [ ] **Step 4: Run builder tests and verify GREEN**

Run the command from Step 2.
Expected: all builder tests PASS.

- [ ] **Step 5: Commit the builder-profile task**

```bash
git add scripts/build_badge_factory_bundle.py \
  scripts/test_build_badge_factory_bundle.py
git commit -m "factory: pin accepted 0.67.2 artifacts"
```

---

### Task 3: Promote the Embedded Offline Bundle and Double-Click Flow

**Files:**
- Modify: `tools/badge_flasher/resources/badge-factory-flasher-embedded.zip`
- Modify: `tools/badge_flasher/tests/test_bundles.py`
- Modify: `flash-badges.command`
- Modify: `docs/badge-factory-flasher.md`
- Modify: `docs/badge/con-crud-canary-acceptance.md`

**Interfaces:**
- Consumes: Task 2 `con-crud-0.67.2` profile and existing strict `load_bundle`.
- Produces: embedded/offline bundle version `0.67.2-badge-defcon34`; double-click offline default; documented local promotion evidence.

- [ ] **Step 1: Add a failing embedded-resource acceptance test**

Add to `tools/badge_flasher/tests/test_bundles.py`:

```python
def test_embedded_factory_resource_is_exact_accepted_0672(self) -> None:
    embedded = load_bundle(
        Path(__file__).resolve().parents[1]
        / "resources/badge-factory-flasher-embedded.zip",
        source="embedded-test",
    )
    self.assertEqual(embedded.version, "0.67.2-badge-defcon34")
    expected = {
        "uplink": "78ef3b6dafe61e8e2fdc3fb28447372aaf76da38cd57ca0961828bbbdc08c434",
        "scanner": "2d0e84501baf3bc929eed03a0b9c1f0272ed66baa9b81dd4513d6dc3fa2c032b",
    }
    for role, digest in expected.items():
        app = next(
            part for part in embedded.layout(role)["parts"]
            if part["path"] == f"{role}/firmware.bin"
        )
        self.assertEqual(app["sha256"], digest)
```

Add a behavioral wrapper test to
`scripts/test_build_badge_factory_bundle.py`. It creates an executable fake
PlatformIO Python below a temporary `HOME`, launches the real
`flash-badges.command`, and asserts on the arguments that executable receives:

```python
def test_double_click_wrapper_executes_offline_before_user_arguments(self) -> None:
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        fake_python = root / ".platformio/penv/bin/python"
        fake_python.parent.mkdir(parents=True)
        capture = root / "argv.txt"
        fake_python.write_text(
            "#!/bin/zsh\nprintf '%s\\n' \"$@\" > \"$FOF_TEST_ARGV\"\n",
            encoding="utf-8",
        )
        fake_python.chmod(0o755)
        result = subprocess.run(
            [str(REPO_ROOT / "flash-badges.command"), "--plain", "--once"],
            cwd=REPO_ROOT,
            env={
                **os.environ,
                "HOME": str(root),
                "FOF_TEST_ARGV": str(capture),
            },
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            capture.read_text(encoding="utf-8").splitlines(),
            [
                "scripts/fof_badge_factory.py",
                "--offline",
                "--plain",
                "--once",
            ],
        )
```

This test executes the wrapper boundary without invoking the real flasher or
touching hardware.

- [ ] **Step 2: Run resource tests and verify RED**

Run:

```bash
/Users/billh/.platformio/penv/bin/python -m unittest \
  tools.badge_flasher.tests.test_bundles \
  scripts.test_build_badge_factory_bundle -v
```

Expected: FAIL because the embedded bundle is still `0.64.76` and the wrapper
does not force offline selection.

- [ ] **Step 3: Build the accepted bundle twice and prove determinism**

Run:

```bash
/Users/billh/.platformio/penv/bin/python \
  scripts/build_badge_factory_bundle.py \
  --profile con-crud-0.67.2 \
  --version 0.67.2-badge-defcon34 \
  --output /tmp/badge-factory-flasher-0.67.2-a.zip

/Users/billh/.platformio/penv/bin/python \
  scripts/build_badge_factory_bundle.py \
  --profile con-crud-0.67.2 \
  --version 0.67.2-badge-defcon34 \
  --output /tmp/badge-factory-flasher-0.67.2-b.zip

shasum -a 256 \
  /tmp/badge-factory-flasher-0.67.2-a.zip \
  /tmp/badge-factory-flasher-0.67.2-b.zip
```

Expected: both bundle SHA-256 values are identical. Abort promotion if they
differ.

- [ ] **Step 4: Replace the embedded resource and make the wrapper offline**

Copy the first deterministic ZIP over
`tools/badge_flasher/resources/badge-factory-flasher-embedded.zip`.

Change the final wrapper line to:

```zsh
exec "$PIO_PYTHON" scripts/fof_badge_factory.py --offline "$@"
```

Do not modify either accepted `firmware.bin`.

- [ ] **Step 5: Update operator and acceptance documentation**

In `docs/badge-factory-flasher.md`, document:

- per-badge `1/2/3` selection and confirmation;
- HUMAN/INFECTED/HEALER mappings;
- double-click offline behavior;
- JSONL record location;
- `--game-role` as the explicit scripted override; and
- `--yes --once --game-role ROLE` as the only unattended form.

In `docs/badge/con-crud-canary-acceptance.md`, append the exact `.67.2`
version, two accepted application hashes, all-three-badge physical acceptance
summary, owner approval date `2026-07-29`, local-only promotion status, and a
statement that GitHub/web assets remain unchanged.

- [ ] **Step 6: Run resource tests and verify GREEN**

Run the command from Step 2.
Expected: all bundle and builder tests PASS.

- [ ] **Step 7: Inspect the promoted resource directly**

Run:

```bash
/Users/billh/.platformio/penv/bin/python -c '
from pathlib import Path
from tools.badge_flasher.bundles import load_bundle
b = load_bundle(
    Path("tools/badge_flasher/resources/badge-factory-flasher-embedded.zip"),
    source="final-audit",
)
print(b.version)
for role in ("uplink", "scanner"):
    app = next(
        p for p in b.layout(role)["parts"]
        if p["path"] == f"{role}/firmware.bin"
    )
    print(role, app["size"], app["sha256"])
'
```

Expected exact output values:

```text
0.67.2-badge-defcon34
uplink 1468464 78ef3b6dafe61e8e2fdc3fb28447372aaf76da38cd57ca0961828bbbdc08c434
scanner 1216800 2d0e84501baf3bc929eed03a0b9c1f0272ed66baa9b81dd4513d6dc3fa2c032b
```

- [ ] **Step 8: Commit the local promotion**

```bash
git add tools/badge_flasher/resources/badge-factory-flasher-embedded.zip \
  tools/badge_flasher/tests/test_bundles.py \
  flash-badges.command \
  docs/badge-factory-flasher.md \
  docs/badge/con-crud-canary-acceptance.md
git commit -m "v0.67.2: promote accepted badge factory bundle"
```

---

### Task 4: Full Host Verification and Physical Factory Canary

**Files:**
- Modify only if evidence must be appended: `docs/badge/con-crud-canary-acceptance.md`
- Do not modify: firmware source or accepted firmware binaries

**Interfaces:**
- Consumes: promoted embedded resource and per-badge CLI.
- Produces: release evidence authorizing the 42-badge production run.

- [ ] **Step 1: Run the complete factory-tool suite**

Run:

```bash
/Users/billh/.platformio/penv/bin/python -m unittest discover \
  -s tools/badge_flasher/tests -v

/Users/billh/.platformio/penv/bin/python -m unittest \
  scripts.test_build_badge_factory_bundle -v
```

Expected: zero failures and zero errors.

- [ ] **Step 2: Run frozen firmware identity and strict artifact verification**

Run:

```bash
/Users/billh/.platformio/penv/bin/python \
  esp32/scripts/verify_firmware_versions.py --repo-root .

/Users/billh/.platformio/penv/bin/python \
  esp32/scripts/verify_badge_scanner_build.py \
  --build-dir esp32/scanner/.pio/build/scanner-s3-combo-fof_badge-con-crud-canary \
  --partition-source esp32/scanner/partitions_s3_scanner_8mb.csv \
  --sdkconfig esp32/scanner/sdkconfig.scanner-s3-combo-fof_badge-con-crud-canary \
  --canary-production-build-dir esp32/scanner/.pio/build/scanner-s3-combo-fof_badge

/Users/billh/.platformio/penv/bin/python \
  esp32/scripts/verify_badge_uplink_build.py \
  --build-dir esp32/uplink/.pio/build/uplink-s3-fof_badge-con-crud-canary \
  --partition-source esp32/uplink/partitions_s3_fof_badge_8mb.csv \
  --sdkconfig esp32/uplink/sdkconfig.uplink-s3-fof_badge-con-crud-canary \
  --canary-production-build-dir esp32/uplink/.pio/build/uplink-s3-fof_badge
```

Expected: version verifier and both strict build verifiers PASS.

- [ ] **Step 3: Prove no accepted firmware byte changed**

Run:

```bash
shasum -a 256 \
  esp32/uplink/.pio/build/uplink-s3-fof_badge-con-crud-canary/firmware.bin \
  esp32/scanner/.pio/build/scanner-s3-combo-fof_badge-con-crud-canary/firmware.bin

git diff --check
git status --short
```

Expected hashes are the two Global Constraints. Status may include only the
intentional committed work plus unrelated `.camera-before-zoom.jpg`; no
firmware source or build output is staged.

- [ ] **Step 4: Exercise the menu without hardware mutation**

Launch the double-click path from a terminal with no badge connected:

```bash
./flash-badges.command --plain --once
```

Verify the role menu appears before the connection prompt. Enter invalid input,
decline one confirmation, then select a role and stop with Ctrl-C before
connecting hardware. Confirm exit `130` and no new PASS/FAIL JSONL row.

- [ ] **Step 5: Run one physical end-to-end factory canary**

Require one complete three-board badge connected by all three USB ports. Run:

```bash
./flash-badges.command --plain --once --allow-rework
```

Choose the operator-requested canary role from the interactive menu. Do not use
`--game-role`, because this test must cover the human menu path.

PASS requires:

- exact topology assignment;
- verified readback for two scanners and one uplink;
- exact `0.67.2-badge-defcon34` on all three boards;
- exact selected post-reboot seed and inactive game;
- healthy BLE-primary and Wi-Fi-primary scanner roles;
- live USB/UART/radios, clear rollback state, and no recovery mode;
- one fsync-backed JSONL PASS row containing all three MACs, selected seed,
  bundle SHA-256, and receipt; and
- no false PASS output on any failure.

- [ ] **Step 6: Record physical factory evidence if the canary passes**

Append the factory receipt, selected role, embedded bundle SHA-256, and PASS
timestamp to `docs/badge/con-crud-canary-acceptance.md`. Run:

```bash
git diff --check
git add docs/badge/con-crud-canary-acceptance.md
git commit -m "v0.67.2: accept final factory canary"
```

Do not commit the private manufacturing JSONL/CSV ledger.

- [ ] **Step 7: Final release audit**

Run the full Task 4 Steps 1–3 commands again after the evidence-only commit.
Confirm the branch remains local and no push, tag, release, merge, or public
manifest change occurred.

Only after this gate may the operator start the 42-badge production run.
