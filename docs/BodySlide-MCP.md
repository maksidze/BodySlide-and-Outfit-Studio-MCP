# BodySlide MCP

BodySlide has its own MCP server, separate from Outfit Studio. Start it with:

```powershell
.\BodySlide.exe --mcp
# Optional explicit port:
.\BodySlide.exe --mcp --mcp-port 47824
```

Connect an MCP client using Streamable HTTP at `http://127.0.0.1:47824/mcp`.
Outfit Studio continues to use port 47823. Both can run together. Only loopback
connections are accepted. The existing protocol's Origin checks apply.

In BodySlide Settings, enable **MCP Server** and choose its port to start it
automatically. Settings are stored under `MCP/Enabled` and `MCP/Port` in
`BodySlide.xml`. Optional `MCP/Token` requires `Authorization: Bearer <token>`.
The command-line server is not started for CLI batch builds or preview-only mode.

## Tools

| Tool | Result |
| --- | --- |
| `get_status` | Game enum, project/output paths, current UI selection, server URL |
| `list_outfits` | Buildable outfit names, OSP sources, output bases, groups, base projects, stacked add-ons; `filter`, `regex`, `offset`, `limit` |
| `list_groups` | Groups and member names |
| `list_presets` | Preset names, XML sources and assigned groups |
| `get_build_selection` | Saved output winners and per-project zap choices |
| `plan_build` | Server-owned plan, preflight errors, outputs, existing files and conflicts |
| `resolve_conflicts` | New plan ID with additional explicit choices or replacement preference rules |
| `build_batch` | Execute a ready plan once; report success/failure and actual written files per outfit |
| `get_build_report` | Retrieve the result by plan ID, including after a disconnected build call |

The catalog reflects BodySlide's loaded projects. Use the window's refresh control
after installing or removing projects, then create a new plan.

## Example

Arguments to `plan_build`:

```json
{
  "groups": ["My Outfits"],
  "preset": "My Preset",
  "targetDir": "D:/BodySlide Output",
  "tri": true,
  "prefer": ["Physics"],
  "zaps": {"My Armor Physics": {"Hide Belt": true}}
}
```

Select by `outfits`, `groups`, `filter` (optionally `regex`), or explicitly
`all: true`. Groups and filter narrow the selected outfits. A named stackable
add-on selects its base project, which includes its installed add-ons.

`preset` defaults to the current selected preset name, loaded independently of
the UI sliders. `preset: ""` uses project defaults. `presetFile` loads an
additional XML file, absolute or relative to BodySlide; its presets take priority
over installed presets of the same name. Unknown preset names are rejected.

`targetDir` defaults to the configured output directory and can point to the game
data folder or a mod manager's output mod. Existing outputs can be overwritten.
`tri`, `forceNormals`, `conformAddOns` default to false;
`clippingFixStrength` defaults to 0 and accepts 0..100. `zaps` overrides visible
per-outfit zap choices on top of the saved choices. Project defaults and presets
continue to supply all other slider values.

Inspect `ready`, `errors`, `outfits`, `conflicts`, `selected` and `skipped`. Each
outfit lists its source, input NIF, planned output paths and preflight messages.
Missing resources on a skipped losing variant do not block the selected winner.

To select a winner explicitly, pass the **actual relative filenames** listed in
`conflicts` to `resolve_conflicts`:

```json
{
  "planId": "plan-1",
  "choices": {
    "meshes/armor/example/body_0.nif": "My Armor Physics",
    "meshes/armor/example/body_1.nif": "My Armor Physics",
    "meshes/armor/example/body.tri": "My Armor Physics"
  }
}
```

Use the returned **new** plan ID for `build_batch`:

```json
{"planId": "plan-2"}
```

## Conflict and execution rules

* Conflict detection compares actual NIF and morph filenames, case-insensitively
  for ASCII names with slash normalization. It catches weighted/single-mesh
  collisions and shared Starfield `morph.dat` outputs.
* Priority is explicit filename choice, valid saved output choice, then ordered
  `prefer` substrings of outfit names. Only a unique match wins. A tie in the
  first matching preference stays unresolved. There is no first-project fallback.
* An outfit builds all its outputs or is skipped entirely. Winners requiring
  incompatible parts of different variants are rejected.
* Planning, resolution and MCP builds do not persist output/zap choices to
  `BuildSelection.xml`. Existing saved choices are copied into the plan.
* Plans snapshot presets, zaps and options. UI slider edits do not affect builds.
  Changed project resources, game or skeleton selection require a new plan.
  Resource freshness uses paths, file sizes and modification timestamps.
* Unsafe relative output paths and outputs escaping the target through links
  are rejected. Builds also refuse to overwrite their input dependencies.
* Execution uses the existing BodySlide batch pipeline in unattended mode,
  sequentially, with no conflict, progress or completion dialogs. The window is
  disabled while building. MCP progress is emitted when the client requests it.
* Each plan executes once. A failed or partial build may already have written
  files; its report lists those files. Re-plan to retry. There is no transactional
  rollback, background job API, or cancellation in this first stage.
* Existing non-body TRI files are preserved and reported as warnings. With
  `tri: false`, unattended builds preserve existing TRI files.
* The latest 16 plans and their reports are retained in memory. Restarting
  BodySlide clears them. `get_build_report` becomes available after the current
  synchronous build finishes; a disconnected client does not cancel the build.

MCP operations refuse to run while a modal dialog or another BodySlide operation
is active. This server handles BodySlide output variants; mod manager priority
changes and conflict resolution between installed mods require separate integration.

## Validation

Build with `BSOS_BUILD_TESTS=ON`, then run CTest. The `[bodyslide]` Catch2 cases
cover path normalization, conflicting actual filenames, saved choices, preference
ties, stale choices and contradictory low/high winners.

The optional live integration test starts an isolated BodySlide process, creates
synthetic NIF/slider inputs, calls its HTTP MCP, checks preset/zap geometry and
TRI morphs, and exercises partial failure and stale-plan rejection:

```powershell
python tests/BodySlideMcpSmokeTest.py --bodyslide build/mcp/Release/BodySlide.exe --tests build/mcp/Release/BSOSTests.exe --work-dir build/mcp-smoke
```

The process is terminated at the end; generated inputs, logs and reports remain
under the chosen work directory for inspection. No installed game data is used.
