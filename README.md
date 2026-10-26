**English** | [繁體中文](README.zh-TW.md)

# Beam

**A fast, keyboard-first launcher for Windows and Linux (GNOME).**
Press a hotkey, start typing, and open apps, switch windows, run commands, do quick math, or call your own plugins written in any language.

![Platforms](https://img.shields.io/badge/platform-Windows%2010%2F11%20%7C%20Linux%20(GNOME)-blue)
![Language](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus)
![License](https://img.shields.io/badge/license-MIT-green)

- **Windows 10 / 11**: native C++ (Win32 + Direct2D). One small `.exe`, no runtime to install.
- **Linux (Ubuntu 26.04 / GNOME 50, Wayland)**: native C++ (GTK 4), installed as a `.deb`. See [Linux](#linux-ubuntu-2604--gnome).

Both versions share the same search, calculator, favorites, settings file and plugin protocol, so a plugin can work on both.

> **Note:** Beam's interface is currently in **Traditional Chinese**. Built-in commands also respond to English names (`lock`, `sleep`, `shutdown`, `switch windows`, `new plugin`, …).

---

## Contents

- [Features](#features)
- [Install](#install)
- [Usage](#usage)
- [Favorites](#favorites)
- [Plugins](#plugins)
- [Configuration](#configuration)
- [Linux (Ubuntu 26.04 / GNOME)](#linux-ubuntu-2604--gnome)
- [Building from source](#building-from-source)
- [Project layout](#project-layout)
- [Security and privacy](#security-and-privacy)
- [Known limitations](#known-limitations)
- [License](#license)

## Features

- **App search** with fuzzy matching, abbreviations (`vsc` → Visual Studio Code) and Chinese app names
- **Window switcher**: find any open window by title or app and jump to it
- **Calculator**: `12*(3+4)^2`, `0xff+1`, `1920x1080`; integers also shown in hex and binary
- **Quicklinks**: `gh react`, `yt lofi`, `tr hello` (keyword + search text), fully customizable
- **Run shell commands**: `> ipconfig /all` (Windows) or `> htop` (Linux)
- **Browse paths** with Tab completion: `C:\`, `%APPDATA%`, `~/Downloads`, `$HOME`
- **Open URLs** directly: `github.com`, `localhost:3000`
- **Web search** as the last result for any text
- **System commands**: lock, sleep, shut down, restart, sign out, empty trash… (destructive ones ask you to press Enter again)
- **Favorites** you can pin and reorder, plus "recently used" and learned ranking (type `c` → Chrome once, and it comes first next time)
- **Plugins in any language**: JSON over stdin/stdout, with templates, a debug log and a CLI test tool
- **IME-friendly**: while an input method (Zhuyin, Cangjie, …) is composing, Enter / arrows / Esc go to the IME, not to Beam

## Install

### Windows

1. Download `Beam.exe` (and `beam-plugin.exe` if you want to write plugins) from the [Releases](../../releases) page, or [build it yourself](#building-from-source).
2. Put it in its own folder (for example `%LOCALAPPDATA%\Programs\Beam\`) and run `Beam.exe`.
3. The first run opens the settings window. After that Beam lives in the system tray.

> The `.exe` is not code-signed, so SmartScreen may warn on first run: choose **More info → Run anyway**. If you downloaded a zip, right-click it → **Properties → Unblock** before extracting.

### Linux

```sh
sudo apt install ./beam-launcher_1.2.4_amd64.deb
```

Then open **Beam** from the app grid once. That first start registers the hotkey with GNOME; see [first start](#first-start) for details.

## Usage

Press **Alt+Space** (configurable) and start typing.

| Type | Result |
|---|---|
| `chrome`, `vsc`, `notepad`, `記事本` | Applications. On Windows: Start menu and Store apps. On Linux: installed apps, including Snap and Flatpak |
| A window title or app name | Open windows; Enter switches to it |
| `12*(3+4)^2`, `0xff+1`, `1920x1080` | Calculator; Enter copies the result |
| `gh react`, `yt lofi`, `tr hello`, `map taipei` | Quicklinks (keyword + text) |
| `> ipconfig /all`, `> htop` | Run a command. Windows: in `cmd` (Ctrl+Enter uses PowerShell). Linux: in your terminal (Ctrl+Enter runs it in the background, Ctrl+Shift+Enter with `sudo`) |
| `C:\`, `%APPDATA%` / `/etc`, `~/Downloads`, `$HOME` | Browse files and folders; Tab completes |
| `github.com`, `localhost:3000` | Open the URL |
| Anything else | The last result is a web search |
| `lock`, `sleep`, `shutdown`, `restart`, `empty trash`, … | Built-in commands (power commands ask for a second Enter) |
| A plugin keyword + space, e.g. `repo ` | Enter that plugin and search as you type (see [Plugins](#plugins)) |

With an empty search box Beam shows your **favorites**, **recently used** items and your open windows.

### Keyboard shortcuts

| Key | Action |
|---|---|
| ↑ ↓ / Ctrl+N Ctrl+P / PgUp PgDn | Move the selection |
| Enter | Main action (open / switch / copy result) |
| Ctrl+K | Action panel (run as admin, show in folder, copy path, close window, …) |
| Ctrl+Shift+Enter | Windows: run as administrator |
| Ctrl+Shift+E / Ctrl+Shift+C | Show in file manager / copy path |
| Ctrl+W | Close the selected window |
| Tab | Autocomplete (paths, quicklink and plugin keywords) |
| Ctrl+Shift+F | Add to / remove from favorites |
| Ctrl+Shift+↑ / ↓ | Reorder favorites (on Windows, Ctrl+Alt+↑ / ↓ also works) |
| Esc | Back → clear → hide |
| Backspace (empty box) | Leave the plugin or window list |

You can also set a dedicated **window switcher hotkey** (`windowSwitcherHotkey`). It opens the window list directly with the previous window preselected, like Alt+Tab.

## Favorites

Anything can be pinned: apps, built-in commands, quicklinks, plugin commands. Select it and press **Ctrl+Shift+F** (or Ctrl+K → "Add to favorites"). Favorites always appear at the top of an empty search, in the order you choose: select one and press **Ctrl+Shift+↑ / ↓** to move it. While searching, pinned items are marked with ★ and ranked a little higher.

The order is saved in `favorites.json` in the config folder.

## Plugins

A plugin is a folder inside the plugins directory (Windows `%APPDATA%\Beam\plugins\`, Linux `~/.config/beam/plugins/`) with a `plugin.json` and a program in **any language**. Beam writes one JSON request to the program's stdin and reads one JSON response from its stdout. The response can show a list, run an action (open, copy, run, set the search text, call the plugin back) or display long text.

```python
# main.py: echo back whatever you type
import json, sys
query = json.load(sys.stdin)["query"]
print(json.dumps({"items": [{"title": f"You typed: {query}", "copy": query}]}))
```

```json
{ "name": "Echo", "keyword": "echo", "run": "main.py" }
```

Save both files in `plugins/echo/`, then type `echo ` in Beam.

### Developer tools

| Tool | What it does |
|---|---|
| `new plugin` (in Beam) | Pick a name and a template (Windows: Python / Node.js / PowerShell; Linux: Python / Node.js / Shell); the folder opens when it's created |
| `plugin log` (in Beam) | The last 30 runs: command line, stdin, stdout, stderr, exit code and duration |
| `reload plugins` / `plugins folder` | Changes to `plugin.json` reload automatically; these are the manual versions |
| `beam-plugin new NAME --lang python` | Create a plugin from a template on the command line |
| `beam-plugin run NAME "query" [--callback 1.2] [--raw]` | Run a plugin the same way Beam does and print the parsed result; also checks icons, shortcuts and items with no action |
| `beam-plugin list` / `doc` / `dir` | List plugins and `plugin.json` errors, print the protocol docs, print the plugins folder |

The full protocol (request and response fields, actions, icons, encodings, command mode, multi-command plugins, Windows vs. Linux differences) is in [PLUGINS.md](PLUGINS.md) (Traditional Chinese). Beam also copies it into your plugins folder on start.

## Configuration

**Windows:** tray icon → **Settings…** for the hotkey, launch at startup, theme and search engine.
**Linux:** type `beam settings` in Beam to open the settings file in your text editor.

Advanced options live in `settings.json` (Windows `%APPDATA%\Beam\`, Linux `~/.config/beam/`). Changes apply as soon as you save, and comments are allowed:

```jsonc
{
  "hotkey": "Alt+Space",
  "windowSwitcherHotkey": "Ctrl+Alt+Space",   // "" = disabled
  "launchAtStartup": true,
  "includeWindowsInSearch": true,
  "theme": "auto",                            // auto | dark | light
  "searchEngineUrl": "https://www.google.com/search?q={query}",
  "maxResults": 40,
  // Extra folders to index, 3 levels deep
  // (Windows: .exe .lnk .bat .cmd .ps1 .msc; Linux: executable files)
  "extraFolders": ["D:\\Tools", "~\\scripts"],
  "quicklinks": [
    { "name": "GitHub search", "keyword": "gh", "target": "https://github.com/search?q={query}&type=repositories" },
    { "name": "Jira", "keyword": "j", "target": "https://example.atlassian.net/browse/{query}" },
    { "name": "Open project in VS Code", "keyword": "code", "target": "code", "arguments": "\"D:\\src\\{query}\"" },
    { "name": "Edit hosts", "target": "notepad.exe", "arguments": "C:\\Windows\\System32\\drivers\\etc\\hosts", "runAsAdmin": true }
  ]
}
```

- Windows paths in JSON need double backslashes (`\\`). On Linux just write `"~/scripts"` or `"/opt/tools"`.
- `{query}` is URL-encoded automatically inside URLs.
- `runAsAdmin` only applies on Windows.

Other files in the same folder: `favorites.json` (favorites), `usage.json` (usage history, safe to delete), `plugins/` (plugins), `plugin-data/` (each plugin's own data), and `beam.log` (Windows error log; on Linux errors go to the journal: `journalctl --user -g beam`).

## Linux (Ubuntu 26.04 / GNOME)

### First start

When you open Beam for the first time it:

1. **Registers the global hotkey.** Wayland doesn't let normal apps grab keys, so Beam adds a GNOME custom shortcut called "Beam" (Alt+Space → `beam --toggle`) under *Settings → Keyboard → Custom Shortcuts*. GNOME normally uses Alt+Space for the window menu; Beam frees it and tells you.
2. **Enables autostart** (`~/.config/autostart/local.beam.Launcher.desktop`) so the first hotkey press is instant. Set `"launchAtStartup": false` to turn this off. The hotkey starts Beam even if it isn't running.
3. **Enables the "Beam Window Switcher" GNOME Shell extension.** On Wayland only GNOME Shell can see other apps' windows, so window switching goes through this small extension. GNOME only loads new extensions at login, so **log out and back in once** before window switching works. Everything else works right away.

If you change `hotkey` / `windowSwitcherHotkey` in `settings.json`, Beam updates the GNOME shortcut automatically and warns you about conflicts with other GNOME shortcuts.

### Differences from the Windows version

- Built-in commands: lock screen, sleep, power off, restart, log out, trash, empty trash, open terminal, set up window switching.
- `> command` runs in bash in your default terminal (Ptyxis, GNOME Terminal, …) and leaves you in the shell afterwards. Ctrl+Enter runs it in the background; Ctrl+Shift+Enter adds `sudo`.
- The Ctrl+K panel for apps lists the actions from their `.desktop` file (for example a browser's "New Window" or "New Private Window").
- Plugin templates are Python / Node.js / Shell (bash).
- Beam covers the whole work area with a transparent overlay while open; clicking outside the panel closes it. Switching input methods (Super+Space) does not close Beam.

### Uninstall

```sh
beam --unregister            # remove the shortcut, autostart and extension; give Alt+Space back to GNOME
sudo apt remove beam-launcher
rm -rf ~/.config/beam        # also delete settings, favorites and plugins
```

### Alt+Space doesn't work?

Run:

```sh
beam --doctor
```

It checks whether Beam has been started, whether its GNOME shortcut still exists, whether anything else uses the same keys (GNOME itself, other custom shortcuts, extensions, or app shortcuts registered through GNOME), and whether GNOME's shortcut service (`gsd-media-keys`) is running. It also repairs Beam's own shortcut.

Common causes:

- **Beam hasn't been opened yet.** Installing the `.deb` alone doesn't create the shortcut; the first start does.
- **GNOME refused the key.** `gsd-media-keys` grabs each shortcut only once. If the key still belonged to something else at that moment (e.g. the window menu Beam just freed), it logs "Failed to grab accelerator" and never retries. Beam looks for that message and asks it to re-register (up to 3 times, then shows a notification). Logging out and back in also re-registers every shortcut.
- **Another app holds it.** GNOME 50 lets apps register global shortcuts (you'll see a permission prompt; remove them under *Settings → Apps*). These only work while that app runs, so Alt+Space may work only some of the time. If nothing explains it, `beam --doctor` prints a snippet for Looking Glass (Alt+F2 → `lg`) that asks GNOME Shell who owns the key.

### Other commands

`beam --windows` (window list), `beam --show`, `beam --quit`, `beam --doctor`, `beam --version`.
For debugging, start with `BEAM_DEBUG=1 beam` to print show/hide and focus changes.

## Building from source

Requires **CMake 3.20+** and a **C++20** compiler.

**Windows (Visual Studio 2022)**

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
```

**Windows, cross-compiled on Linux (MinGW-w64)**

```sh
sudo apt install g++-mingw-w64-x86-64-posix cmake ninja-build
cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
cmake --build build-win
```

Both produce `Beam.exe` and `beam-plugin.exe`.

**Linux (Ubuntu 26.04)**

```sh
sudo apt install build-essential cmake ninja-build pkg-config libgtk-4-dev
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build-linux
packaging/linux/build-deb.sh build-linux    # creates beam-launcher_<version>_amd64.deb
```

Needs GTK 4.14 or newer (Ubuntu 26.04 ships GTK 4.22 and GNOME 50). You can also `sudo cmake --install build-linux` instead of building a `.deb`.

### Tests

```sh
cmake -S . -B build-tests -DBEAM_BUILD_TESTS=ON && cmake --build build-tests
./build-tests/beam_tests          # core logic: matching, calculator, JSON, hotkeys, favorites, plugin protocol (any OS)
./build-tests/beam_plugin_tests   # Linux: runs real bash / python3 / node plugins through the plugin runner
```

`tests/gnome/` contains end-to-end tests that run inside a headless GNOME Shell 50 (shortcuts, focus, Zhuyin input, window switching, launching apps). **Run them only in a throwaway container**: they replace `HOME` and your GNOME settings. See [tests/gnome/README.md](tests/gnome/README.md).

## Project layout

```
src/core/     Portable logic: fuzzy ranking, calculator, JSON, hotkeys, settings, usage, favorites, plugin protocol
src/plugin/   Plugin host shared by both platforms: loading, scheduling, debug log, templates, beam-plugin CLI
src/win/      Windows: launcher window, Direct2D renderer, app index, window list, icons, search,
              plugin processes, settings dialog, tray, app controller
src/linux/    Linux: GTK 4 launcher, app index, GNOME shortcuts, window switching, search,
              plugin processes, system actions, app controller
data/linux/   .desktop file, icons, GNOME Shell extension (beam-windows@local.beam)
packaging/    .deb packaging script
res/          Windows icon, manifests (PerMonitorV2 DPI), dialog resources
tests/        Unit tests, plugin runner tests, GNOME integration tests
PLUGINS.md    Plugin protocol reference (also embedded in the binaries)
```

## Security and privacy

- **Plugins are programs on your computer, and Beam runs them with your user permissions.** Only install plugins you trust, the same as any script you download.
- Beam passes `{query}` to plugins as a single argument, without a shell. Where an argument would be run as code (`bash -c`, `python -c`, `cmd`, PowerShell without `-File`, …) Beam leaves `{query}` out. The safest way for a plugin to read the query is always stdin.
- Plugin authors: if your plugin puts data from the internet into `open` or `run` actions, check it first. Beam executes those actions exactly as given.
- **Window switcher extension (Linux):** it exposes a D-Bus interface inside GNOME Shell (`/local/beam/Windows`) that lists, focuses and closes windows, and it only answers the Beam process. That check is best effort; any program running as your user could get around it, so think of the extension as giving up some of Wayland's window privacy. If you don't need window switching, leave it disabled.
- Beam has **no telemetry and makes no network requests** of its own. URLs and web searches open in your browser.
- `usage.json` stores which result you picked for which search text, locally, to improve ranking. Delete it anytime.

## Known limitations

- **Windows:** if Alt+Space is already taken (e.g. by PowerToys Run), Beam tells you; pick another hotkey in settings.
- **Windows:** when Beam runs without admin rights, Windows blocks some actions on elevated windows (such as closing them). Switching to them still works.
- **Linux:** the global hotkey and window switching only work on **GNOME** (Ubuntu 26.04's default desktop). On other desktops, bind `beam --toggle` to a shortcut yourself; window switching isn't available.
- The UI is in Traditional Chinese for now.

## License

[MIT](LICENSE)
