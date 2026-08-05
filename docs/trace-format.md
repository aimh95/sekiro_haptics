# Trace and config JSON formats

This document is the schema reference for the three JSON-based formats the
replay pipeline uses: the JSONL signal trace, the presets file, and the
mappings file. See [docs/ARCHITECTURE.md](ARCHITECTURE.md) for how these
feed into `ReplaySignalSource` → `IGameEventDetector` → `EventMapping` →
`HapticPreset` → `HapticScheduler`.

## JSON library

This project hand-rolls its own minimal JSON parser
(`include/sekiro_haptics/Json.hpp`, `src/Json.cpp`,
`sekiro_haptics::json::JsonValue`/`ParseJson`) instead of adding a
third-party dependency.

**Why:** every schema below is flat objects, at most one array of objects
(`presets`/`mappings`), and at most one level of nested object
(`legacyRumble`). A recursive-descent parser handles that -- and arbitrary
deeper nesting, for free, since the recursion doesn't need to know how deep
it's allowed to go -- in a few hundred lines. There was no real complexity
saved by scoping the parser down to "flat only," and this project's own bar
is "no unnecessary dependencies." The hand-rolled parser is original code,
so there are no license or attribution concerns (unlike, e.g., the vendored
byte-layout reference cited in `docs/ARCHITECTURE.md`'s HIDAPI section);
this repository does not currently have a `LICENSE` file at all, and adding
one is unrelated to this change.

**Documented fallback:** if a future schema needs real nesting depth,
streaming parse, or strict schema validation beyond what's practical to
hand-roll, [nlohmann/json](https://github.com/nlohmann/json) (MIT license,
single header) is the natural choice -- it can be pulled in via CMake
`FetchContent` the same way HIDAPI already is for the DualSense transport
(see `docs/ARCHITECTURE.md`).

**Known limitations** (by design, not oversights): `ParseJson` does not
support `\uXXXX` unicode escapes, comments, or trailing commas. A JSON
value using any of these is reported as a parse error rather than silently
mis-parsed. This is acceptable because every file this parser reads is
hand-authored or tool-authored config/trace data for this project, not
arbitrary external JSON.

## JSONL signal trace format

Schema version: `sekiro_haptics::trace::kSchemaVersion = 1`
(`include/sekiro_haptics/replay/TraceJsonl.hpp`). Bump this constant and
update this document if the required-field set or ordering policy below
ever changes.

One JSON object per line (newline-delimited JSON / JSONL), one line per
`GameSignal`:

```json
{"timestampUs":1000,"signal":"player.block_state","value":"active"}
{"timestampUs":1020,"signal":"enemy.attack_contact","value":"true"}
{"timestampUs":1030,"signal":"player.hp_delta","value":"0"}
{"timestampUs":1040,"signal":"manual.perfect_deflect","value":"true"}
```

### Fields

| Field | Required | Type | Notes |
|---|---|---|---|
| `timestampUs` | yes | JSON number | Microseconds. Missing or non-numeric → `TraceReadResult::MissingTimestamp`. |
| `signal` | yes | JSON string, non-empty | Stable trace-schema identifier (see "Signal name vocabulary" below). Missing, non-string, or empty → `TraceReadResult::MissingSignal`. |
| `value` | no | any JSON scalar | Stringified regardless of JSON type (`true`/`false` for bools, decimal text for numbers). Defaults to `""` if absent. Arrays/objects as a `value` are not supported (stringify to `""`). |
| *(anything else)* | no | any JSON scalar | Preserved into `GameSignal::extra[key]`, stringified the same way as `value`. A trace author can attach ad hoc fields a detector may or may not look at, without the schema needing to anticipate every future need. |

Blank lines (empty or whitespace-only) are skipped silently. Lines that
fail to parse as JSON at all, or parse but aren't a JSON object, are
`TraceReadResult::MalformedJson` with an error string of the form
`"line N: <reason>"`.

### Ordering policy

`timestampUs` must be **non-decreasing** from one non-blank line to the
next (ties are allowed). A line whose timestamp is less than the previous
line's is reported as `TraceReadResult::OutOfOrderTimestamp` -- the trace
is not silently reordered. This is a rolling check against the immediately
preceding line, not a global maximum.

### Signal name vocabulary

Signal names are stable string identifiers for **this project's trace
schema** -- they describe what a trace file can record, not any claim
about real Sekiro internals (no memory addresses, offsets, or engine event
IDs are implied by any name below or anywhere else in this repo):

```
xinput.rumble
player.hp_delta
player.posture_delta
player.animation
effect.applied
audio.sfx
manual.perfect_deflect
manual.take_damage
```

Only `manual.perfect_deflect` and `manual.take_damage` currently have a
consumer (`ManualLabelEventDetector`, which is test-only -- see
`docs/testing.md`). The others exist as placeholders for future signal
observations a real detector would need to look at.

## Presets JSON format

Loaded by `PresetRepository::LoadFromFile()`
(`include/sekiro_haptics/presets/PresetRepository.hpp`):

```json
{
  "presets": [
    {
      "presetId": "sharp_metal_v1",
      "displayName": "Sharp Metal Impact",
      "legacyRumble": {
        "left": 0.45,
        "right": 0.85,
        "durationMs": 28
      }
    }
  ]
}
```

Top level must be a JSON object with a `presets` array; otherwise the
whole load fails (`PresetLoadOutcome::ok == false`,
`PresetLoadOutcome::fatalError` set). Each array entry is validated and
loaded independently -- one bad entry does not fail the whole file.

### Per-entry validation policy

| Condition | Outcome |
|---|---|
| Entry is not a JSON object | Rejected (recorded in `errors`, not loaded) |
| Missing/empty `presetId` | Rejected |
| Missing/non-string `displayName` | Rejected |
| Missing/non-object `legacyRumble` | Rejected |
| Missing/non-numeric `legacyRumble.durationMs` | Rejected |
| `legacyRumble.durationMs <= 0` | Rejected -- treated as a corrupt fixture, not coerced |
| `legacyRumble.left`/`right` outside `[0, 1]` | **Clamped** into range; entry is still loaded; the clamp is recorded in `errors` as a warning |
| `legacyRumble.left`/`right` absent | Defaults to `0.0`; no warning |

`presetId` is the schema's growth path for new game events -- see the
"Migration" section of `docs/ARCHITECTURE.md`. The current hardware only
supports legacy left/right rumble, so `legacyRumble` is the only preset
representation today; it is deliberately nested under its own key (rather
than flattened onto the entry) so a future preset kind (PCM haptics,
adaptive-trigger commands) can be added as a sibling key without changing
this schema's existing fields or `GameEvent`/`EventMapping`.

## Mappings JSON format

Loaded by `MappingRepository::LoadFromFile()`
(`include/sekiro_haptics/presets/MappingRepository.hpp`):

```json
{
  "mappings": [
    {
      "gameId": "sekiro",
      "eventId": "combat.perfect_deflect",
      "presetId": "sharp_metal_v1"
    }
  ]
}
```

Same top-level/per-entry-independence rules as presets. Each entry
requires non-empty `gameId`, `eventId`, and `presetId` strings; a missing
or empty one rejects that entry only.

**`MappingRepository` does not check that `presetId` refers to a preset
that actually exists** -- presets and mappings are loaded independently of
each other and independently of load order. Cross-referencing (detecting a
mapping that points at a nonexistent preset) is `ReplayPipeline`'s job,
reported as `PipelineError::MappingReferencesMissingPreset` when it
happens during a replay.
