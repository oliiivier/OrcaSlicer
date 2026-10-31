# Fork oliiivier/OrcaSlicer — AI assistant

Fork of [Snapmaker/OrcaSlicer](https://github.com/Snapmaker/OrcaSlicer) that embeds an
[MCP](https://modelcontextprotocol.io) server: an assistant (Claude, Gemini…) can analyse the project and the
configuration, recommend settings and apply them after confirmation in the slicer.
This fork is not meant to be merged upstream: the goal is to take in upstream updates with as few conflicts as
possible.

## Branches

| Branch               | Role                                                                  |
|----------------------|-----------------------------------------------------------------------|
| `main`               | Exact mirror of `upstream/main`. **No fork commits on it.**           |
| `ai-assistant`       | Default branch of the fork: upstream + the fork's features.           |
| `feat/*`             | Feature development, PRs into `ai-assistant`.                         |

## Rules to limit conflicts

1. All of the fork's code lives in folders that do not exist upstream:
   - `src/slic3r/AIAssistant/` (C++ and `AIAssistant.cmake`)
   - `resources/ai_assistant/` (documentation generated for the MCP server)
   - `resources/web/ai_assistant/` (chat page) and `resources/images/topbar_claude.svg`
   - `.github/workflows/fork_release.yml` (fork release builds)
   - `scripts/fork/`
   - `docs/assets/` (README images)
2. Upstream files are only touched at the **hook points** below, always wrapped in
   `#ifdef SNAPMAKER_AI_ASSISTANT // fork`. If a new hook point is needed, add it to this list.
3. Always integrate upstream with a **merge** (never rebase `ai-assistant`, never squash upstream merges).
4. Translations: the fork's strings are **appended at the end** of `localization/i18n/Snapmaker_Orca.pot` and of
   the `.po` files (upstream files), never inserted in the middle, and `localization/i18n/list.txt` lists the
   fork's sources. If upstream reruns `msgmerge` and the merge conflicts, take their version and then append the
   fork's entries again at the end of the file.

### Hook points in upstream code

| File                            | Change                                                        |
|---------------------------------|---------------------------------------------------------------|
| `src/slic3r/CMakeLists.txt`     | `include(AIAssistant/AIAssistant.cmake OPTIONAL)` after `encoding_check(libslic3r_gui)` |
| `src/slic3r/GUI/MainFrame.cpp`  | include of `AIAssistant.hpp` + call to `append_menu_items()` at the end of `generate_help_menu()` |
| `src/slic3r/GUI/BBLTopbar.cpp`  | include of `AIAssistant.hpp` + `add_topbar_button()` after *Calibration* in `Init()`, `rescale_topbar_button()` in `Rescale()` |
| `src/slic3r/GUI/GLCanvas3D.cpp` | include of `ScreenCapture.hpp` + `ScreenCapture::on_canvas_frame()` right before `SwapBuffers()` in `render()` |
| `src/slic3r/GUI/GUI_ObjectList.cpp` | Linux: slicer colours for the object list (the GTK theme can be light while the slicer is dark) |
| `README.md`                     | Section *This fork: an AI assistant inside the slicer*, in one block right after the introduction sentence |
| `src/slic3r/GUI/GUI_App.cpp`    | `SplashScreen`: widens the splash screen so that the name and version fit (wider Linux fonts); `fit_width()` in `MakeBitmap()`, logo centred in `Decorate()`, image shown as soon as it is created |

List the actual differences with upstream (should stay short):

```bash
git diff upstream/main...ai-assistant --stat -- . ':!src/slic3r/AIAssistant' ':!resources/ai_assistant' \
    ':!resources/web/ai_assistant' ':!resources/images/topbar_claude.svg' ':!scripts/fork' ':!FORK.md' \
    ':!.github/workflows/fork_release.yml'
```

## Syncing with upstream

GitHub Actions is **disabled** on this fork (a repository setting, not a file change): the workflows inherited
from upstream (`.github/workflows/`) stay in the tree, untouched, but do not run, including those upstream adds
later. Syncing and building are done locally:

```bash
scripts/fork/sync-upstream.sh        # merges upstream/main into ai-assistant and resets main to upstream/main
scripts/fork/sync-upstream.sh -b     # same, then builds (dependencies already built)
scripts/fork/sync-upstream.sh -b -p  # same, then pushes main and ai-assistant to origin
```

On a conflict, the script stops and lists the files involved (normally only the hook points): resolve them,
`git commit`, then run the script again.

Re-enable Actions if needed: `gh api -X PUT repos/oliiivier/OrcaSlicer/actions/permissions -F enabled=true`.

## Release (Linux, Windows builds)

```bash
scripts/fork/release.sh v2.4.1-ai.1   # publishes the pushed head of ai-assistant as a pre-release
```

The script first pushes the tag with git and creates the pre-release (the workflow token cannot create a tag on
a commit that changes `.github/workflows`, which upstream merges bring). It enables Actions for the release
only: it disables every workflow except `fork_release.yml` (including those upstream has added since), runs it,
waits for it to finish (several hours: dependencies are rebuilt, the fork has no cache) and then disables
Actions. `fork_release.yml` reuses upstream's build workflows, which publish nothing outside their `main`, and
attaches the AppImage, the pacman package (the AppImage's content repackaged in an Arch Linux container), and
the Windows installer and zip to the pre-release (macOS dropped for now: no arm64 runner available), with
`scripts/fork/release-notes.md` as its text. The packages are not signed (no certificate in the fork) and the
upload of Sentry symbols fails without its secrets, without blocking the release.

### pacman package (Arch Linux, CachyOS)

```bash
./build_linux.sh -s                    # builds the slicer (build/package)
scripts/fork/arch/build-package.sh     # package build/snapmaker-orca-ai-<version>-x86_64.pkg.tar.zst
scripts/fork/arch/build-package.sh Snapmaker_Orca_Linux_*.AppImage   # or from an AppImage of the fork
sudo pacman -U build/snapmaker-orca-ai-*.pkg.tar.zst
```

`scripts/fork/arch/PKGBUILD` packages the local build (the AppImage's content) into `/opt/snapmaker-orca-ai`
with `/usr/bin/snapmaker-orca`, the desktop entry and the icons; the version follows the commit
(`2.4.1.r<n>.g<hash>`). It provides `snapmaker-orca` and so replaces an upstream AppImage package that is
already installed.

`build_linux.sh` downloads the `snapmaker_control` gateway and replaces `resources/web/flutter_web` with the
Snapmaker Connect pages of the same version (`.github/connect-bundle.lock.json`): these tracked files then show
as modified, and they must **not** be restored (the old version no longer handles signing in to the Snapmaker
account). `build-package.sh` refuses a `build/package` whose pages are not the right version.

## MCP server

*Help → AI Assistant (MCP server)* menu: enabling, port (13700 by default), access for other MCP clients, path
of Claude Code for the chat, and configuration to copy into Claude Code, Gemini CLI or Claude Desktop.
The server listens only on `127.0.0.1`, requires `Authorization: Bearer <token>` and refuses requests coming
from a browser (`Origin`/`Host` headers).

Tokens: the built-in chat gives each CLI it starts a random token (`McpService::Grant`), kept in memory and
revoked when the CLI stops. Other clients only have access if the *Allow other MCP clients* option is checked:
their token is then kept in `ai_assistant/mcp_token` (readable by the user only) and renewed on every
activation. Earlier versions kept it in `Snapmaker_Orca.conf`: it is moved out of there at startup, with
external access left enabled. A program running under the same account can still read these tokens; only a
sandbox (Flatpak, firejail...) prevents it.

| File | Role |
|---|---|
| `McpServer` | Streamable HTTP + JSON-RPC transport, independent of the UI (testable on its own) |
| `McpTools` | Registry of tools and prompts, execution on the UI thread |
| `McpActions` | Shared conventions of the write tools: confirmation, result shape, references (object, part, plate, slot), file paths, test mode |
| `McpIssues` | Collects the slicer's errors and warnings (`get_issues`) |
| `McpChanges` | Validation, confirmation and application of settings (`apply_changes`) |
| `McpChangesDialog` | Confirmation dialog: one checkbox per setting, setting help and the model's comment on hover |
| `McpManufacturerDocs` | Snapmaker's online documentation: wiki and support site (`manufacturer_docs`) |
| `McpMaterials` | Toolheads, filament slots, mixed filaments, per-item assignment |
| `McpNotes` | User instructions and shared folder of Markdown notes (`notes`) |
| `McpObjects` | Prepare-tab operations on objects, after confirmation (`edit_objects`) |
| `McpPresets` | Presets and filament slots through the sidebar's lists and buttons (`edit_presets`) |
| `McpSlicing` | Slicing of the current plate or all plates and summary of the result (`slice`) |
| `McpProject` | Files, plates, calibrations and starting a print (`edit_project`, `edit_plates`, `calibrate`, `control_printer` start_print) |
| `McpRender` | PNG render of the bed from a named view, capture of the sliced preview (`get_view`) |
| `McpPrinter` | Read-only access to the connected printer (`get_printer` status, project/printer mismatches) |
| `McpPrinterLive` | Live reads (`printer.objects.query`), camera photo and print watching (`get_printer` snapshot, watch) |
| `McpPrinterControl` | Commands sent to the printer (`control_printer`): unloading toolheads, pause, resume and cancel |
| `PrinterAutoConnect` | Automatic connection at startup when only one printer is online |
| `ProjectAdapt` | Adapting a project to the connected printer (`adapt_to_printer`) and notification |
| `McpExperiences` | Local log of print experiences (`experiences`) |
| `McpDocs` | Setting documentation shipped with the application (`get_docs`) |
| `McpService` | Server lifecycle (start, stop on close) |
| `McpSettings` | `[ai_assistant]` section of the application configuration |
| `McpDialog` | Settings dialog |

Tools (grouped by domain, the `action` parameter picks the operation): `get_issues`, `get_project`,
`get_config`, `get_docs` (setting, page), `apply_changes`, `edit_presets` (select, save, add_slot, remove_slot,
set_color), `edit_objects`, `edit_plates`, `edit_project` (new, open, import, save, export), `undo` (undo, redo),
`adapt_to_printer`, `slice`, `get_view` (model, preview), `calibrate`, `get_printer` (status, snapshot, watch),
`control_printer` (start_print, pause, resume, cancel, unload_filament), `notes` (list, read, write),
`experiences` (search, record, report), `manufacturer_docs` (search, read). The chat shows the action next to the
tool name.

Names changed since the 30-tool version: `describe_setting` / `read_documentation` → `get_docs`,
`render_view` → `get_view`, `get_printer_status` / `printer_snapshot` / `watch_print` → `get_printer`,
`start_print` / `control_print` / `unload_filament` → `control_printer`, `list_notes` / `read_note` / `write_note`
→ `notes`, `record_experience` / `report_outcome` / `search_experiences` → `experiences`,
`search_manufacturer_docs` / `read_manufacturer_page` → `manufacturer_docs`, `new_project` / `open_project` /
`import_models` / `save_project` / `export_file` → `edit_project`. User-written instructions (MCP dialog) that
mention old names are not rewritten: update them by hand.

Prompts: `prepare_print`, `adapt_project`, `diagnose_print`, `fix_issues`, `optimize_settings`,
`capitalize_experiences`. In Claude Code they are called `/mcp__snapmaker-orca__<prompt>` (type `/` then the
name) and are only loaded at startup: restart Claude Code if the slicer was not open at that time.

Automatic connection (checkbox in the MCP dialog, on by default): at startup, for 2 minutes, if no printer is
connected and only one is online (`query.device.list`), the slicer connects to it (`action.device.connect`, like
the Device tab). A single attempt; never if several are online.

Adapting a project made for another printer (MakerWorld 3MF…): `adapt_to_printer` tool and `adapt_project`
prompt, plus a notification in the slicer when the open project targets a different model than the connected
printer (checkbox in the MCP dialog, on by default; watched by a timer, without a hook point). After
confirmation: preset of the connected printer with its nozzle, process with the closest layer height, and each
filament slot moved to the closest loaded toolhead (same material, then closest colour). On the U1, slot N prints
with toolhead N by default: the project takes one slot per toolhead and everything follows (painting, object
filaments, volumes and ranges, per-feature filaments). The original process settings that differ are returned so
that the assistant can restore the model's intent.

`apply_changes` shows each proposed setting with a checked box, the old and new values and the model's comment
(`comment`); hovering also shows the setting's help. The user applies everything or unchecks values: those are
returned to the model under `unchecked` and recorded in the experience log (`declined_by_user`), for following
up on the print.

User instructions: a field of the MCP dialog, saved in `<data_dir>/ai_assistant/instructions.md` (also editable
outside the slicer) and appended to the server's instructions, so read at the start of every conversation (the
built-in chat as well as external clients). Notes: a folder of Markdown files (by default
`<data_dir>/ai_assistant/notes`, configurable, *Open* button) that the assistant lists, reads and adds to;
writing can be disabled in the dialog. Names stay inside the folder (subfolders with `/`, no `..` and no hidden
files, `.md` extension enforced), 1 MB maximum per note.

Manufacturer documentation, queried live rather than copied (it changes often, loses its images and videos in
Markdown, and is not ours): search in the wiki (public GraphQL API of Wiki.js) and the support site (Zendesk
Help Center API), reading a page as text with its last update date. Only `wiki.snapmaker.com` and
`support.snapmaker.com` are contacted, in the background (the tool waits for the answer without blocking the
UI), and only if the checkbox in the MCP dialog is checked (on by default). The assistant summarises what is
useful in the notes, with the address and the date.

Presets (`edit_presets`): selection goes through the sidebar list, like a click (the slicer itself asks what to
do with unsaved changes and sets nozzles, plates and flush volumes); saving goes through the button of the
settings tab, with a given name (a user preset, never the name of a system preset); adding and removing a slot
through the filament list's commands (removal says in the confirmation which objects change slot); the colour
like the slot's colour picker (solid colour). `get_project.presets` gives the selected presets, those offered by
the lists (visible and compatible) and the unsaved changes.

Conventions of the tools that change something (`McpActions`): a single form of confirmation (reason given by
the model, list of operations, Yes/No question; `apply_changes` keeps its checkbox dialog, files and printing
the slicer's dialogs). The result is always `done`; refusal `declined: true`, timeout `timed_out: true`; list of
operations: `applied`, `failed` (operation and error), `not_run`. Input errors stay MCP errors (`isError`). The
instructions sent to the client are one paragraph per tool domain (`INSTRUCTIONS` in `McpTools.cpp`).

`apply_changes` refuses `post_process` (scripts run on the computer when the G-code is exported), even when
confirmed.

`edit_objects` chains the Prepare tab's operations on objects: scale (factor or size), rotate, move, centre,
drop to bed, mirror, cut horizontally, split into objects, duplicate, delete, auto-orient, arrange the current
plate, add a part, a negative part, a modifier or a support blocker / enforcer (cube, cylinder, cone, sphere with
the given size and position), delete a part, change the filament of an object or a part, rename, move to another
plate (same position on the plate, like a drag and drop). The list is confirmed at once by the user and forms a
single undo step (plus one per orientation or arrangement, background jobs that the tool waits for). Operations
go through the selection and the 3D canvas, like the mouse and the gizmos; an open gizmo blocks the tool. The
selection is cleared before operations that delete or replace an object or a part (the object list would redraw
it on deleted volumes). Cut parts: `<name>_A` (top), `<name>_B` (bottom).

Height ranges (`edit_objects` add_height_range, remove_height_range): like *Height range* in the object list,
with layer height, filament (`extruder`) and per-region settings (walls, infill, surfaces, speeds); ranges do not
overlap, and the object list is rebuilt for the object. Layer events (`edit_plates` add_layer_event,
remove_layer_event): pause or filament change to a slot, at a layer of the current sliced plate (number or
closest height), written to `Model::plates_custom_gcodes` like the preview slider; never free G-code. `undo` undoes
or redoes steps of the slicer's history (Ctrl+Z / Ctrl+Y) after a confirmation that lists their names.

Calibrations (`calibrate`): the parameters of the dialog of each entry of the *Calibration* menu, with the same
checks, then the same slicer function (`Plater::calib_*`), which closes the current project. Automatic painting
(`edit_objects` paint_supports): downward-facing faces less than `angle_deg` from horizontal (excluding faces
resting on the bed) become support enforcers or blockers, keeping existing paint; `clear_paint` clears the paint
of supports, seam, colours or fuzzy skin.

Files and plates: `edit_project` calls the slicer's commands. New project, opening, importing and saving to a
given path, and exports to a given path (G-code of the current plate copied as sliced, without the
post-processing scripts; sliced 3MF of the current plate; STL of all objects as one mesh) are confirmed by a
question; an existing file is only replaced with `overwrite: true`. Without a path, or for *Save as*
(`save_as`), the slicer's file dialog serves as confirmation. The slicer itself asks to save a modified project
before closing it. Given paths must be absolute (`~` accepted). `edit_plates` selects (without confirmation),
adds, deletes (an empty plate only: otherwise its objects would end up off the plates), renames plates and sets
their bed type and print sequence, like the plate settings dialog. `get_project` lists the plates; `slice` with
`all_plates` acts like *Slice all*.

Printer access, through the Snapmaker Connect connection already opened by the application; two checkboxes in
the MCP dialog, off by default:

- **Read**: status (`query.device.objects`, or `printer.objects.query` for what changes quickly), camera photo
  (capture URL of the webcam declared to Moonraker, `server.webcams.list`, usually
  `http://<ip>/webcam/snapshot.jpg` on the local network; not `camera.start_monitor`, whose stream stays active
  ~6 min after stopping), bounded watching of a print. Serial numbers, network, accounts and secrets are removed
  before sending to the model.
- **Control** (requires read), `control_printer`, confirmed in the slicer on every request: `start_print` opens
  the slicer's print dialog for the sliced plate (like the *Print plate* button: the user checks the name and the
  filament mapping, then starts); `control_print` pauses, resumes or cancels the print (`printer.print.pause`,
  `resume`, `cancel`) then waits for the new state; `unload_filament`, refused during a print, confirmed in the
  slicer on every request. Same sequence as the U1's screen: `AUTO_FEEDING_BATCH ACTION=START`, then
  `ACTION=DOING STAGE=prepare EXTRUDER=n UNLOAD=1` (the firmware heats by itself). The printer's screen then sends
  `STAGE=doing` as soon as `unload_heat_finish` (the tool only sends it after 15 s without it being relayed), and
  `ACTION=STOP` ends it (heaters off), including if the call is abandoned. The firmware stops at
  `unload_finish`: the filament is out of the nozzle but stays in the toolhead, and the user pulls it out by
  hand. Flexible filaments (`filament_soft`) are never unloaded.

The documentation comes from `doc/` (OrcaSlicer wiki): `scripts/fork/build-setting-docs.py` links each setting
to its section through the help links of `Tab.cpp` and produces `resources/ai_assistant/setting_docs.json`,
shipped with the application (no network access). `sync-upstream.sh` regenerates and commits it after each
merge. Prompts (slash commands in Claude Code / Gemini CLI): `prepare_print`, `adapt_project`, `diagnose_print`,
`fix_issues`, `optimize_settings`, `capitalize_experiences`.

### Orca features out of the server's reach (to do)

Implementation plan and tool grouping: [`src/slic3r/AIAssistant/PLAN.md`](src/slic3r/AIAssistant/PLAN.md).

- [x] Files and project: new project, open a project, import models (STL, 3MF, STEP), save the project, export
  the G-code, the STL or the sliced 3MF.
- [x] Plates: switch plate, add one, delete one, move an object to another plate, per-plate settings (bed type,
  print sequence), slice all plates.
- [x] Printing: start printing the sliced plate, pause, resume, cancel.
- [ ] Filament loading (lead: same `AUTO_FEEDING_BATCH` sequence as unloading).
- [x] Presets: save a user preset, pick another printer, process or filament preset, add or remove a filament
  slot, change its colour.
- [x] Height ranges with their own settings. (Remaining: variable layer height.)
- [x] Pause or filament change at a layer (no custom G-code: it would go to the printer as is).
- [x] Automatic support painting (overhangs filled by angle) and clearing paint.
- [ ] Hand painting (dropped: the model cannot aim at triangles) and 3D view tools (text, SVG, measure, angled
  cut or cut with connectors, simplify, repair, booleans, lay on face, brim ears).
- [x] Sliced preview as an image (toolpaths, supports, layer range).
- [x] Calibrations (temperature, flow rate, pressure advance, retraction, max volumetric speed, VFA).
- [ ] Application preferences, Snapmaker account and MakerWorld (dropped).
- [x] Undo by the assistant (`undo`).

Deliberately left out: heating, moving the axes and homing the printer.

### Print experiences

1. **Local log** (`<config folder>/ai_assistant/experiences/*.json`, outside the repository): one record per
   diagnosis. The server stores the facts (printer, materials, issues, settings changed or declined through
   `apply_changes`), the model the symptom and the diagnosis, then the actual result of the print
   (`experiences` report, with the printer's report if access is enabled).
2. **Capitalisation**: the `capitalize_experiences` prompt run in Claude Code from this repository. It groups
   the confirmed records and writes `resources/ai_assistant/guides/exp_*.md` (format and rules:
   `resources/ai_assistant/guides/README.md`), then waits for the diff to be reviewed.
3. **Injection**: after review, commit. The guides ship in `setting_docs.json` and are served by `get_docs` page
   (type `experience`).

`get_view` (model) shows the model (not the supports or the toolpaths); `get_view` (preview) switches to the
Preview tab, picks the view and the slider's layer range, then captures the 3D view as displayed (legend and
sliders included): the view changes on screen and the window must be visible. The server handles requests in
parallel (8 threads): a `slice` (up to 10 min), print watching or unloading does not block the other tools; their
execution stays serialised on the UI thread. Claude Code abandons an MCP call after 60 s by default: the built-in
chat passes `"timeout"` (31 min) in its MCP config, and an external client must be started with
`MCP_TOOL_TIMEOUT` (value shown in the MCP settings).

Multiple toolheads (Snapmaker U1, 4 toolheads): `get_project.materials` describes the toolheads and each slot
(preset, type, colour, temperature, toolhead, used on the plate); per-slot/per-toolhead settings are returned as
arrays; `apply_changes` targets a slot (`filament_slot`) or a toolhead (`toolhead`). Only one filament preset can
have unsaved changes at a time: the assistant must have the previous one saved before moving to another slot.

The instructions sent to the client enforce the order: blocking errors, then warnings, then optimisations. Every
change goes through a confirmation in the slicer; per-object overrides are undone with Ctrl+Z, and preset changes
stay unsaved edits.

### Test bench

CMake option `SNAPMAKER_AI_TEST_HOOKS` (off by default), which only affects the `Debug` configuration: the
packages and the release workflow build `Release` and never contain any of it. It builds the test mode: with
`SNAPMAKER_AI_TEST_CONFIRM=yes|no` in the environment, every confirmation of the assistant (shared question,
settings dialog, adaptation) gets that answer without being shown, `new_project` discards changes without
asking, and a permanent red notification signals the mode.

```bash
PATH=$HOME/.venvs/orca-build/bin:$PATH cmake -S . -B build -DSNAPMAKER_AI_TEST_HOOKS=ON
PATH=$HOME/.venvs/orca-build/bin:$PATH cmake --build build --config Debug --target Snapmaker_Orca -j 8
python3 -I scripts/fork/mcp-test.py                  # all untagged scenarios
python3 -I scripts/fork/mcp-test.py 05-objects       # one scenario
python3 -I scripts/fork/mcp-test.py --tags printer   # plus those that need the real printer (read-only)
```

The script copies the user's configuration to a temporary folder (port 13799, external access enabled with a
random token, printer disabled unless `--tags printer`, notes and log in the copy), starts one instance per
answer needed (`--datadir`, the launcher's Zink environment under Wayland + NVIDIA), plays the scenarios
`scripts/fork/mcp-tests/*.json` (call, expected result as a subset of the JSON), stops the instance by its process
group and writes `build/mcp-test-report.md` with the size of `tools/list` and of the instructions. The models come
from a copy of `resources/handy_models`. Tags: `printer` (real printer, read-only), `network` (Snapmaker's online
documentation), `cut` (cutting triggers an assertion in upstream's `cut_mesh()` in Debug, on cases that Release
handles).

## Claude chat

*Claude* button in the top bar (or *Help → Chat with Claude*): a chat with Claude inside the slicer, floating or
docked to the right of the 3D view (button in the chat header, choice remembered). It runs the Claude Code CLI
installed on the machine (`claude -p` in stream-json, signed in once in a terminal) and connects it to the MCP
server, which it enables if it is off. The CLI has no other tool (`--tools=`, no shell, files or web) and runs
in an empty folder (`ai_assistant/chat`). `claude` is looked for in the PATH, the usual install locations, then
the login shell's PATH (nvm, npm prefix…); on Windows, npm's `claude.cmd` shim is read to launch its target
without `cmd.exe`. The path can be forced in the *AI Assistant* dialog (`claude_path`).

Tabs: one conversation (Claude Code session, resumed with `--resume`) per tab, saved with what they display in
`ai_assistant/chats.json`. The CLI of an inactive tab with no reply in progress is stopped, then restarted on its
next message. Buttons capture the 3D view (read back from OpenGL) or the whole window, attached to the next
message. Model choice under the input field, as in the VS Code extension: list given by the CLI (`initialize`
control request, kept in `chats.json`), choice remembered (`chat_model` in the `ai_assistant` section of the
configuration), passed with `--model` at launch and applied to CLIs already running with `set_model`. The MCP
configuration given to the CLI (with its token) is written to `ai_assistant/claude_mcp_<tab>.json`, readable by
the user only and deleted when the CLI stops. The chat page is `resources/web/ai_assistant/chat.html`.

Tested on Linux only: finding `claude` on Windows and macOS, moving the page between window and pane, and
capturing the whole window (screenshot) are still to be checked on those systems.

| File | Role |
|---|---|
| `ClaudeProcess` | Starts the CLI, writes and reads JSON messages line by line, interruption, model |
| `ChatPanel` | Chat page, tabs and sessions, floating window or wxAUI pane of the `Plater` |
| `ScreenCapture` | Capture of the 3D view and of the main window |

## Building

The assistant is enabled by default. To build without it (and check that the fork stays neutral):

```bash
cmake -DSNAPMAKER_AI_ASSISTANT=OFF ...
```
