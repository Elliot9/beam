[English](README.md) | **繁體中文**

# Beam

啟動器：按一個快捷鍵叫出搜尋框，輸入就能開應用程式、切換視窗、跑指令，也能釘選最愛、用任何語言寫外掛。

- **Windows 10 / 11**：原生 C++（Win32 + Direct2D），單一 exe、不需安裝任何 runtime。
- **Linux（Ubuntu 26.04 / GNOME 50，Wayland）**：原生 C++（GTK 4），`.deb` 安裝。見〈[Linux](#linuxubuntu-2604--gnome)〉。

兩個版本共用同一套搜尋、計算機、最愛、設定檔與外掛協定，外掛可以兩邊通用。

## 使用

1. 從 [Releases](../../releases) 下載：Windows 是 `Beam-1.2.4-windows-x64.zip`（先在內容中「解除封鎖」再解壓縮，把 `Beam` 資料夾放到自己的位置，例如 `%LOCALAPPDATA%\Programs\Beam\`），Linux 是 `beam-launcher_1.2.4_amd64.deb`（安裝方式見〈[Linux](#linuxubuntu-2604--gnome)〉）。
   Windows：執行 `Beam.exe`，第一次會開設定視窗，之後常駐在系統匣。Linux：從應用程式清單開啟「Beam」。
2. 按 **Alt+Space**（可改）叫出 Beam，開始輸入。

| 輸入 | 結果 |
|---|---|
| `chrome`、`vsc`、`記事本`、`notepad` | 應用程式（模糊比對、縮寫、中文名稱皆可）。Windows 是開始功能表／Store 應用程式，Linux 是應用程式清單裡的程式（含 Snap、Flatpak） |
| 視窗標題或程式名稱 | 已開啟的視窗，Enter 直接切換過去 |
| `12*(3+4)^2`、`0xff+1`、`1920x1080` | 計算機，Enter 複製結果（整數另顯示 hex / binary） |
| `gh react`、`yt lofi`、`tr hello` | 快速連結（keyword + 搜尋內容） |
| `> ipconfig /all`、`> htop` | 執行指令。Windows：在 cmd 執行，Ctrl+Enter 改用 PowerShell。Linux：在終端機執行，Ctrl+Enter 在背景執行，Ctrl+Shift+Enter 用 sudo |
| `C:\`、`~\Downloads`、`%APPDATA%`（Windows）／`/etc`、`~/Downloads`、`$HOME`（Linux） | 瀏覽路徑，Tab 補完 |
| `github.com`、`localhost:3000` | 直接開網址 |
| 其他任何文字 | 最後一列是網路搜尋 |
| 「切換視窗」「鎖定畫面」「睡眠」「關機」「清空資源回收筒／垃圾桶」… | 內建指令（關機類需再按一次 Enter 確認） |
| 外掛的 keyword + 空白（例如 `repo `） | 進入外掛，邊打邊查（見下方〈外掛〉） |

空白輸入時依序顯示「最愛」、「最近使用」與目前開啟的視窗。Beam 會記住你在某個輸入下選了什麼（例如 `c` → Chrome），之後排在最前面。

### 鍵盤

| 鍵 | 動作 |
|---|---|
| ↑ ↓ / Ctrl+N Ctrl+P / PgUp PgDn | 移動選取 |
| Enter | 主要動作（開啟／切換／複製結果） |
| Ctrl+K | 動作面板（以系統管理員執行、在檔案總管中顯示、複製路徑、關閉視窗…） |
| Ctrl+Shift+Enter | Windows：以系統管理員身分執行 |
| Ctrl+Shift+E / Ctrl+Shift+C | 在檔案總管（檔案管理員）中顯示 / 複製路徑 |
| Ctrl+W | 關閉選取的視窗 |
| Tab | 補完（路徑、快速連結與外掛 keyword） |
| Ctrl+Shift+F | 加入最愛 / 從最愛移除 |
| Ctrl+Shift+↑ / ↓ | 在「最愛」中調整順序（Windows 也可以用 Ctrl+Alt+↑ / ↓） |
| Esc | 返回 → 清空 → 隱藏 |
| Backspace（搜尋框空白時） | 離開外掛或視窗清單，回到上一層 |

另外可以設定「直接切換視窗」的快捷鍵（`windowSwitcherHotkey`），按下就直接進入視窗清單（不帶篩選時預選上一個視窗，跟 Alt+Tab 一樣）。

中文輸入法（注音、倉頡…）組字時，Enter、方向鍵、Esc 都交給輸入法，不會觸發 Beam 的動作。

## 最愛

任何應用程式、內建指令、快速連結或外掛指令都能釘選：選取後按 **Ctrl+Shift+F**（或 Ctrl+K →「加入最愛」）。最愛會固定顯示在空白搜尋的最上方，順序由你決定：在「最愛」裡選一個項目，按 **Ctrl+Shift+↑ / ↓** 上下移動（Windows 也可以用 Ctrl+Alt+↑ / ↓；GNOME 把 Ctrl+Alt+方向鍵拿去切換工作區）。搜尋時，釘選的項目會標上 ★ 並稍微往前排。

順序存在設定資料夾的 `favorites.json`。

## 外掛

外掛是外掛資料夾（Windows `%APPDATA%\Beam\plugins\`、Linux `~/.config/beam/plugins/`）底下的一個資料夾：一個 `plugin.json` 加上任何語言的程式。Beam 把一個 JSON 請求寫到它的 stdin，從 stdout 讀回一個 JSON，畫成清單、執行動作（開啟、複製、執行、填入搜尋、回呼外掛）或顯示長文字。

```python
# main.py：輸入什麼就回什麼
import json, sys
query = json.load(sys.stdin)["query"]
print(json.dumps({"items": [{"title": f"你輸入了「{query}」", "copy": query}]}, ensure_ascii=False))
```

```json
{ "name": "回聲", "keyword": "echo", "run": "main.py" }
```

存檔後在 Beam 輸入 `echo ` 就能用。

**開發工具**

| 工具 | 用途 |
|---|---|
| Beam 裡的「建立新外掛」 | 輸入名稱、選範本（Windows：Python / Node.js / PowerShell；Linux：Python / Node.js / Shell），建立後直接打開資料夾 |
| 「外掛偵錯紀錄」 | 最近 30 次執行的命令列、stdin、stdout、stderr、結束代碼與耗時 |
| 「重新載入外掛」/「開啟外掛資料夾」 | 改了 `plugin.json` 會自動重新載入，這兩個指令是手動版 |
| `beam-plugin new 名稱 --lang python` | 命令列建立範本 |
| `beam-plugin run 名稱 "查詢" [--callback 1.2] [--raw]` | 用和 Beam 一樣的方式執行一次，印出解析結果，並檢查圖示、快捷鍵、沒有動作的項目 |
| `beam-plugin list` / `doc` / `dir` | 列出外掛與 `plugin.json` 的錯誤、顯示協定說明、顯示外掛資料夾 |

完整協定（請求／回應欄位、動作、圖示、編碼、command 模式、多指令外掛、Windows 與 Linux 的差別）見 [PLUGINS.md](PLUGINS.md)；Beam 啟動時也會把它複製到外掛資料夾裡。

## 設定

Windows：系統匣圖示 → **設定…**：快捷鍵（點欄位後直接按組合鍵）、開機啟動、主題、搜尋引擎。Linux：在 Beam 輸入「Beam 設定」，用文字編輯器開啟設定檔。

進階設定在 `settings.json`（Windows `%APPDATA%\Beam\`、Linux `~/.config/beam/`），存檔後自動套用（可寫註解）：

```jsonc
{
  "hotkey": "Alt+Space",
  "windowSwitcherHotkey": "Ctrl+Alt+Space",   // 空字串 = 不使用
  "launchAtStartup": true,
  "includeWindowsInSearch": true,
  "theme": "auto",                            // auto | dark | light
  "searchEngineUrl": "https://www.google.com/search?q={query}",
  "maxResults": 40,
  // 把自己的工具資料夾加進索引，往下 3 層（Windows：.exe .lnk .bat .cmd .ps1 .msc；Linux：有執行權限的檔案）
  "extraFolders": ["D:\\Tools", "~\\scripts"],
  "quicklinks": [
    { "name": "GitHub 搜尋", "keyword": "gh", "target": "https://github.com/search?q={query}&type=repositories" },
    { "name": "Jira", "keyword": "j", "target": "https://example.atlassian.net/browse/{query}" },
    { "name": "在 VS Code 開專案", "keyword": "code", "target": "code", "arguments": "\"D:\\src\\{query}\"" },
    { "name": "編輯 hosts", "target": "notepad.exe", "arguments": "C:\\Windows\\System32\\drivers\\etc\\hosts", "runAsAdmin": true }
  ]
}
```

JSON 中的 Windows 路徑要寫成 `\\`；Linux 寫 `"~/scripts"`、`"/opt/tools"` 即可。`{query}` 在網址中會自動 URL-encode。`runAsAdmin` 只在 Windows 有效。

其他檔案：`favorites.json`（最愛）、`usage.json`（使用紀錄，可刪）、`plugins/`（外掛）、`plugin-data/`（外掛自己的資料）、`beam.log`（Windows 的錯誤紀錄；Linux 寫到 journal，`journalctl --user -g beam` 看得到）。

## Linux（Ubuntu 26.04 / GNOME）

### 安裝

```sh
sudo apt install ./beam-launcher_1.2.4_amd64.deb
```

然後從應用程式清單開啟「Beam」。第一次啟動時 Beam 會：

1. **設定全域快捷鍵**：Wayland 不讓一般程式攔截按鍵，所以 Beam 在 GNOME 的「設定 → 鍵盤 → 自訂快捷鍵」新增一組「Beam」（Alt+Space → `beam --toggle`）。GNOME 原本用 Alt+Space 開啟「視窗選單」，Beam 會把它讓出來並通知你。
2. **開機自動啟動**（`~/.config/autostart/local.beam.Launcher.desktop`），讓第一次按快捷鍵就很快。不想要的話在 `settings.json` 設 `"launchAtStartup": false`。就算沒有在執行，按快捷鍵也會把 Beam 叫起來。
3. **啟用「Beam Window Switcher」GNOME Shell 擴充功能**：Wayland 上只有 GNOME Shell 看得到其他程式的視窗，切換視窗靠它。GNOME 只在登入時載入新的擴充功能，所以**登出再登入一次**後才能切換視窗（其他功能馬上就能用）。

在 `settings.json` 改 `hotkey` / `windowSwitcherHotkey`，Beam 會自動更新 GNOME 的自訂快捷鍵；如果新的按鍵和 GNOME 其他快捷鍵衝突，會跳出通知。

### 和 Windows 版的差別

- 內建指令：鎖定畫面、睡眠、關機、重新啟動、登出、垃圾桶、清空垃圾桶、開啟終端機、設定視窗切換（GNOME 擴充功能）。
- `> 指令` 用 bash 在預設終端機（Ptyxis / GNOME 終端機…）執行，結束後留在 shell；Ctrl+Enter 在背景執行，Ctrl+Shift+Enter 加上 sudo。
- 應用程式的 Ctrl+K 面板會列出 desktop 檔裡的動作（例如瀏覽器的「開新視窗」「無痕視窗」）。
- 外掛範本是 Python / Node.js / Shell（bash）；`run` 的規則見 PLUGINS.md。
- 視窗列表、切換和關閉都經過 GNOME Shell 擴充功能。它在 GNOME Shell 裡提供一個 D-Bus 介面（`/local/beam/Windows`），只接受已安裝的 Beam（名叫 `beam`、只有 root 能修改的執行檔，例如 `/usr/bin/beam`）呼叫，其他程式來問會被拒絕（Wayland 本來就不讓程式看到別的程式的視窗）。所以直接從建置資料夾執行的 `beam` 不能切換視窗，要先安裝。
- Beam 打開時會蓋住整個工作區（透明，只看得到中間的面板），點面板外面就會關閉。切換輸入法（Super+Space）不會關閉 Beam。

### 解除安裝

```sh
beam --unregister            # 移除快捷鍵、開機啟動與擴充功能，Alt+Space 還給 GNOME 的視窗選單
sudo apt remove beam-launcher
rm -rf ~/.config/beam        # 要連設定、最愛與外掛一起刪的話
```

### Alt+Space 沒反應？

執行 `beam --doctor`。它會檢查 Beam 開過沒有、GNOME 裡 Beam 的快捷鍵還在不在、有沒有別的快捷鍵用了同一組按鍵（GNOME 本身、其他自訂快捷鍵、GNOME 擴充功能、App 透過 GNOME 登記的全域快捷鍵）、GNOME 的快捷鍵服務（gsd-media-keys）有沒有在執行，並順便把 Beam 自己的快捷鍵修好。

App 的全域快捷鍵：GNOME 50 讓 App 自己登記全域快捷鍵，第一次會跳出視窗請你允許；之後可以在「設定 → 應用程式 → 那個 App」移除。這類快捷鍵只在 App 執行時有效，所以 Alt+Space 有時能用、有時不能。如果 GNOME 一直拒絕、設定裡又找不到是誰，`beam --doctor` 會印出一行給 Looking Glass（Alt+F2 → `lg`）的程式碼，直接問 GNOME Shell 是誰占用了按鍵。

最常見的原因是**裝好之後還沒開過 Beam**：快捷鍵是 Beam 第一次啟動時才寫進 GNOME 的，只裝 `.deb` 還不會有。

另一個原因是 GNOME 拒絕了這組按鍵：GNOME 的快捷鍵服務（gsd-media-keys）只會向 GNOME Shell 要一次按鍵，那一刻按鍵如果還屬於別的快捷鍵（例如剛讓出 Alt+Space 的視窗選單），它只在系統記錄裡留一行 “Failed to grab accelerator”，之後不會再試。Beam 會在記錄裡找這一行，找到就請它重新註冊（最多 3 次，還是不行會跳通知）；Beam 啟動時和 `beam --doctor` 都會檢查。登出再登入也會讓它重新註冊所有快捷鍵。

### 其他指令

`beam --windows`（視窗清單）、`beam --show`、`beam --quit`、`beam --doctor`（檢查快捷鍵）、`beam --version`。除錯時用 `BEAM_DEBUG=1 beam` 啟動，會在終端機印出顯示／隱藏與焦點變化。

## 從原始碼建置

需要 CMake 3.20+ 與 C++20 編譯器。

**Windows（Visual Studio 2022）**

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
```

**Windows 版，在 Linux 上交叉編譯（MinGW-w64）**

```sh
sudo apt install g++-mingw-w64-x86-64-posix cmake ninja-build
cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
cmake --build build-win
```

會產生 `Beam.exe` 與 `beam-plugin.exe`。

**Linux 版（Ubuntu 26.04）**

```sh
sudo apt install build-essential cmake ninja-build pkg-config libgtk-4-dev
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build-linux
packaging/linux/build-deb.sh build-linux     # 產生 beam-launcher_<版本>_amd64.deb（也可以 sudo cmake --install build-linux）
```

需要 GTK 4.14 以上；Ubuntu 26.04 是 GTK 4.22、GNOME 50。

**測試**

```sh
cmake -S . -B build-tests -DBEAM_BUILD_TESTS=ON && cmake --build build-tests
./build-tests/beam_tests          # 核心邏輯：比對、計算機、JSON、熱鍵、最愛、外掛協定…（任何平台）
./build-tests/beam_plugin_tests   # Linux：用真的 bash / python3 / node 外掛測外掛執行器
```

`tests/gnome/` 是在無頭（headless）GNOME Shell 50 裡測整個流程的腳本：GNOME 快捷鍵、焦點、注音輸入、視窗切換、啟動程式。

## 結構

```
src/core/     可攜邏輯：fuzzy 排序、計算機、JSON、熱鍵、設定、使用紀錄、最愛、外掛協定、文字編輯
src/plugin/   外掛管理（兩個平台共用）：載入、執行排程、偵錯紀錄、範本、beam-plugin 工具
src/win/      Windows：啟動器視窗(launcher)、繪圖(renderer)、應用程式索引(apps)、視窗列舉(winlist)、
              圖示(icons)、搜尋組裝(search)、外掛行程(plugin_process)、設定視窗、系統匣、App 主控
src/linux/    Linux：GTK 4 啟動器(launcher)、應用程式索引(apps)、GNOME 快捷鍵(hotkey)、
              視窗切換(windows)、搜尋組裝(search)、外掛行程(plugin_process)、系統操作(system)、App 主控
data/linux/   desktop 檔、圖示、GNOME Shell 擴充功能（beam-windows@local.beam）
packaging/    Linux .deb 打包腳本
res/          Windows 圖示、manifest（PerMonitorV2 DPI）、對話框資源
tests/        單元測試、外掛執行器測試、GNOME 整合測試
PLUGINS.md    外掛協定說明（也會嵌入程式）
```

## 注意

- Windows：exe 沒有簽章，第一次執行 SmartScreen 可能攔下：「其他資訊 → 仍要執行」。從網路下載的 zip 先在內容中「解除封鎖」。
- Windows：Alt+Space 若已被其他工具（PowerToys Run 等）占用，Beam 會提示，到設定換一組即可。
- Windows：以一般權限執行時，無法切換到系統管理員權限的視窗以外的操作（例如對其送出關閉）會被 Windows 擋下；切換本身不受影響。
- Linux：只支援 GNOME（Ubuntu 26.04 預設桌面）的全域快捷鍵與視窗切換；其他桌面可以自己把 `beam --toggle` 設成快捷鍵，視窗切換則無法使用。
- 外掛就是你電腦上的程式，Beam 會直接執行它們；只安裝你信任的外掛。
