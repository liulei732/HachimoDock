# Device-local records and text notebooks

For small numeric logs, inventories or game positions, use record actions. The authoring input is JSON. The compiler emits an ordinary foreground component program in runtime/widget.json. The device runs the same integer VM used by the authoring preview. Device operation and reboot recovery require no desktop, network, serial service or credential.

## Plain-text notebook

The [notebook example](../examples/local-notebook) is a complete component package directory. It works offline with the native form keyboard and saves committed strings under the component ID. Each row has a stable name such as `note1`. The form uses the ordinary private JSON save domain. The numeric record actions below use the separate program snapshot domain.

From the repository root, generate two installable package directories with the [notebook authoring script](../scripts/text_notebook.py). Choose fresh output directories. Validate each directory with the PetUI CLI. Install the first version, enter text and then update to the second with the same component ID.

```sh
python3 pc/skills/petui/scripts/text_notebook.py notebook-v1 \
  --id creator-notes --keyboards latin pinyin --max-bytes 40
python3 pc/skills/petui/scripts/text_notebook.py notebook-v2 \
  --id creator-notes --version 2.0.0 --keyboards latin pinyin latin --max-bytes 40
petui validate notebook-v1
petui validate notebook-v2
```

Choose one to four fixed rows and a limit of 1–63 **UTF-8 bytes per row**. Each row has a Latin, numeric or bounded Pinyin keyboard. Empty strings represent unused rows. The form accepts single-line plain text. On the device, use the stick to select a row and SW1 to edit. Turn the knob to choose a key group, press it to append, use the stick to select a letter or candidate, and hold the knob to erase. In Pinyin mode SW1 accepts the candidate first. SW1 with no pending candidate commits the drafts together. SW2 clears a pending Pinyin composition. With no composition pending, it discards the draft. To clear a row, erase its contents and commit.

For an additive upgrade keep the component ID, `persist.version`, state and existing row names. Append rows at the end. Old rows retain their strings and new rows start empty. The native restore checks existing keys, types and byte bounds before publishing. Dropping a saved row prevents cold restore. General key renaming and arbitrary text transformations are outside this authoring path. The firmware coalesces ordinary edits before writing its private A/B store, so abrupt power loss during that window can restore the previous complete save. A successful preserve-save removal on the corrected firmware synchronizes the latest committed edit.

To exercise the notebook on a candidate board, set `PORT`, `BOARD_ID` and `EVIDENCE` as described in the board instructions below. Use a separate, new JSONL filename for each run.

```sh
python3 firmware/tools/record_checkin_hil.py --notebook \
  --port "${PORT:?set PORT}" --board "${BOARD_ID:?set BOARD_ID}" \
  --log "${EVIDENCE:?set EVIDENCE}"
cat "$EVIDENCE"
```

The script installs a fresh private ID and drives the native form actions to commit Latin and Chinese notes. It verifies their exact text and persisted-field digest on cold foreground entry, upgrades the same ID with an empty third row, edits it, then checks preserve/reboot/reinstall and clear/reinstall. The reported view revision belongs to the display. Text saves expose a digest, while numeric record programs also expose a durable snapshot revision on cold readback.

## Numeric check-in authoring

From the repository root, this command regenerates the supplied check-in package's runtime.

```sh
python3 pc/skills/petui/scripts/local_records.py \
  firmware/tests/fixtures/record-checkin/source.json \
  > firmware/tests/fixtures/record-checkin/package/runtime/widget.json
```

Keep the authoring source outside the package. The compiled package follows the existing component-program installation and capability checks. Its schema is selected automatically from the required cell and instruction counts. Verify the target announces widgetProgram or widgetPrograms for that exact engine, the reported code, cell and fuel budgets, and widgetProgramQuantum. The check-in log and its logical upgrade both fit the v1 engine on the four public profiles.

## Record layout and actions

Add a records object to an ordinary program-compatible app surface. The check-in source is a complete example.

```json
{
  "version": 1,
  "words": 2,
  "user_slots": 4,
  "cache_slots": 1,
  "outputs": ["status", "key", "savedDay", "kind"],
  "actions": {
    "work": {"inputs": ["day"], "batch": [["append", "user", "day", 1]]},
    "browse": {"inputs": ["cursor"], "batch": [["next", "user", "cursor"]]},
    "erase": {"inputs": ["cursor"], "batch": [["delete", "user", "cursor"]]}
  }
}
```

The component owns its two fixed tables. Each table uses zero-based integer keys. Each present row has 1–6 signed 32-bit words. A program can reserve separate budgets for irreplaceable user records and rebuildable cache rows. Both tables persist in the same snapshot. The cache has explicit deletion and no automatic eviction. Installed JSON, module and asset bytes remain subject to the package loader's separate limits. Records support numeric data and creator-defined encodings; the check-in example stores day of year and activity. Its day is editable on the device.

| Batch operation | Result |
| --- | --- |
| `["put", space, key, word, ...]` | Insert or replace one complete row. |
| `["append", space, word, ...]` | Reserve the lowest free key. Must be first in the batch. A full table leaves every row intact. Keys can be reused after deletion. |
| `["get", space, key]` | Read the exact row. |
| `["next", space, key]` | Find the first occupied key at or above key. Continue with returned key + 1. The table capacity is a valid end cursor. |
| `["delete", space, key]` | Clear presence and all words. Repeating deletion is safe. |
| `["map_add", space, column, delta]` | During a declared migration, add a constant to a zero-based column of each occupied row. Follow it with a final operation. Overflow rolls back the entire migration. |

Each action can reference up to three declared integer input variables. Use their names in the operand list, or use int32 constants. Inputs are captured at dispatch. Up to eight operations commit together. Reads finish a batch. Outputs are status, key and the final row's words. Status 1 means a row, 0 means absent/end/deleted, −1 means an invalid dynamic key, −2 means full, and −3 means schema mismatch. On these non-row outcomes key is −1 and all word outputs are zero. Allow those values in the bound UI variables' limits. A result outside the configured projection, VM overflow, exhausted fuel or revoked owner prevents transaction publication. Package-defined actions cannot choose the native save ID or access pairing state.

The layout uses one schema cell plus `(1 + words) × (user_slots + cache_slots)` cells. All operations must fit 256 cells, 1024 instructions, 4096 fuel and the installed profile's JSON byte budget. Small programs use at most 64 cells and 256 instructions. The underlying program store has eight shared scoped RAM entries with the shipping no-eviction policy. Exhaustion can deny a cold activation until restart. Durable snapshots live in the private A/B store. Host acceptance precedes its deferred write, which waits at least 30 seconds and can be delayed by audio or other storage work. A restart before a new checkpoint is durable can restore the previous complete snapshot.

## Upgrade and identity

Keep the component ID, row width and both quotas stable across compatible releases. The physical layout is encoded in the native snapshot schema. A layout change fails activation against the old snapshot. Reserve spare rows or fields before shipping. The app's logical version lives in cell zero. An ordinary action initializes a fresh table at the current version. Existing versions require an explicit migration. For the check-in upgrade, actions use activity codes 11 and 12. The upgrade action transforms every occupied old row and records the new version in the same transaction.

```json
{"from_version":1,"batch":[["map_add","user",1,10],["put","cache",0,2,0],["next","user",0]]}
```

Applying this migration twice returns schema mismatch on the second attempt. A downgraded app likewise cannot change newer rows through the compiled API. The authorized same-ID package is the data owner; native checks isolate different component IDs, while the SDK handles compatible versions of the same owner. Records are private device saves, unsuitable for account secrets. Preserve-save uninstall retains them for same-ID reinstall. Explicit clear-save uninstall advances the program domain's durable deletion epoch and releases its cached checkpoint.

## Check-in fixture and board evidence

The test package directory is `firmware/tests/fixtures/record-checkin/package`. Its screen shows input day, browsing cursor, action status, last recorded day, row key and activity. Work and rest append distinct records. Browse skips deleted rows. The adjacent upgrade.json supplies the second release's transformations.

The HIL driver uses a fresh random component ID and checks the board identity, available catalog slot and program capabilities for both fixture releases. It adds two rows, reboots, opens the app and verifies the saved revision, hash, day, category and row key before sending another input. It upgrades the same ID and reads both migrated rows. After a preserve-save removal, it reboots immediately and verifies the latest revision on reinstall. It also checks clear-save removal, removes its fixture and restores the original selection and screen.

Run from the repository root with Python 3 and pyserial. Set `PORT` to the board's serial port, `BOARD_ID` to its reported device ID and `EVIDENCE` to a new JSONL filename in an existing directory. An interrupted run leaves the fixture ID in that log for inspection. The HIL driver sends each delete once and waits up to 120 seconds for its correlated ACK. Python bench callers can choose a shorter deadline with `delete_ack(bench, component, preserve, timeout_seconds=60)` (1–120 whole seconds). A timeout leaves the ACK outcome unknown.

```sh
python3 firmware/tools/record_checkin_hil.py \
  --port "${PORT:?set PORT}" --board "${BOARD_ID:?set BOARD_ID}" \
  --log "${EVIDENCE:?set EVIDENCE}"
cat "$EVIDENCE"
```
