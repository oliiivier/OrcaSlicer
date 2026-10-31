Test build of the fork [oliiivier/OrcaSlicer](https://github.com/oliiivier/OrcaSlicer) (branch `ai-assistant`):
Snapmaker Orca with an AI assistant. Unsigned and unofficial: neither Snapmaker nor Anthropic distributes it.

## New in v2.4.1-ai.3

- **The assistant works on the whole project**, always through the slicer's own commands and after your
  confirmation:
  - files: new project, open, import models, save, export the G-code, the sliced 3MF or the STL;
  - plates: switch, add, delete an empty plate, rename, bed type and print sequence, move an object to another
    plate, slice all plates;
  - presets: select a printer, process or filament preset, save a user preset, add or remove a filament slot,
    change its colour;
  - height ranges with their own settings, and a pause or filament change at a given layer;
  - support painting on overhangs (by angle) and clearing paint;
  - the projects of the *Calibration* menu (temperature, flow, pressure advance, retraction, max volumetric
    speed, VFA);
  - undo and redo of the slicer's history.
- **It looks at the sliced preview**: it switches to the *Preview* tab, picks a view and a layer range, and
  captures what you see.
- **During a print**: start the sliced plate (through the slicer's print dialog), pause, resume, cancel, and
  exclude objects or the prime tower (Klipper's *Exclude objects*).
- **New prompt `prepare_print`**: checks the open project and prepares it for printing.
- **Parallel tool calls**: a slice, a print watch or a filament unload no longer blocks the other tools.
- **Security**: the chat gives each Claude Code it starts a token of its own, revoked when it stops. Other MCP
  clients only get access when *Allow other MCP clients* is checked in *Help → AI Assistant*.
- **Translated**: the assistant's windows and messages are in the 19 languages of the slicer (machine
  translations).
- The chat shows `/compact`, `/clear` and the output of commands such as `/context`.
- Tools grouped by domain (30 → 16, `action` parameter): instructions you wrote that name old tools must be
  updated by hand (list of the changed names in [FORK.md](https://github.com/oliiivier/OrcaSlicer/blob/ai-assistant/FORK.md)).

## Already in v2.4.1-ai.2

- Object editing like the *Prepare* tab (scale, rotate, cut, split, add a modifier…), with one confirmation
  and one undo step.
- Your instructions, read at the start of every conversation, and a folder of Markdown notes the assistant
  keeps.
- Snapmaker's wiki and support site queried live, with the page and its date cited.
- Choice of the Claude model in the chat.

## Already in v2.4.1-ai.1

- **Chat with Claude inside the slicer** (*Claude* button in the top bar): floating or docked next to the 3D
  view, one conversation per tab, capture of the 3D view or of the window. Needs
  [Claude Code](https://claude.com/claude-code) installed, signed in once in a terminal (`claude`).
- **MCP server** (*Help → AI Assistant (MCP server)*): Claude, Gemini or another MCP client reads the project,
  explains the warnings, proposes settings (each confirmed in the slicer), follows the connected printer and
  adapts a project made for another printer.

## Install

| System | File | Note |
|---|---|---|
| Linux | `.AppImage` | `chmod +x`, then run it |
| Arch Linux, CachyOS | `.pkg.tar.zst` | `sudo pacman -U <file>`; run `snapmaker-orca` |
| Windows | `.exe` (installer) or `_portable.zip` | SmartScreen: *More info → Run anyway* |

No macOS build for now.

## To test on each system

1. Ask the assistant to save the project under a new name, then to export the G-code: confirmation, files
   written.
2. Ask it to add a pause at a layer of the sliced plate: the pause shows on the preview slider.
3. Ask it to select another filament preset, then to save it as a user preset.
4. Ask it to show the sliced preview of the first layers: the *Preview* tab opens on those layers.
5. The *Claude* button opens the chat and turns the MCP server on; *Help → AI Assistant* shows the Claude
   Code in use ("Used by the chat: …"), especially for an npm install (Windows `claude.cmd`).
6. Switching floating ↔ docked (button in the chat header) without a blank page or a crash.
7. *3D view* and *Window* captures.
8. Tabs are back after restarting the slicer.

Tested on Linux only so far.
