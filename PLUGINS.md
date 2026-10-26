# Beam 外掛

外掛就是一個資料夾：`plugin.json` 描述指令，再加上任何語言寫的程式（Python、Node、PowerShell、bash、Go/Rust/C++ 編譯出來的程式、bat 都可以）。同一個外掛可以同時在 Windows 和 Linux 上用，差別只在 `run` 能用的直譯器（見〈run 的寫法〉）。

Beam 執行 `plugin.json` 裡的 `run`：
- 把**一個 JSON 請求**寫到它的 stdin；
- 從它的 stdout 讀**一個 JSON 回應**，畫成結果清單或執行動作。

除錯訊息請寫到 **stderr**，stdout 只放 JSON。

## 快速開始

1. 在 Beam 輸入「建立新外掛」（或 `newplugin 名稱`），選語言範本：Windows 有 Python / Node.js / PowerShell，Linux 有 Python / Node.js / Shell（bash）。也可以用命令列：`beam-plugin new 名稱 --lang python`（Windows 的 `beam-plugin.exe` 和 `Beam.exe` 放在同一個資料夾；Linux 安裝套件後就有 `beam-plugin` 指令）。
2. 外掛建立在外掛資料夾：Windows 是 `%APPDATA%\Beam\plugins\<名稱>\`，Linux 是 `~/.config/beam/plugins/<名稱>/`（`beam-plugin dir` 會印出來）。修改程式碼不用重新載入；改了 `plugin.json` 存檔後 Beam 也會自動重新載入。
3. 在 Beam 輸入外掛的 `keyword` 加空白（例如 `myplugin `），就會進入外掛，邊打邊呼叫它。
4. 在終端機測試：`beam-plugin run 名稱 "要查的字"`，會印出原始輸出、解析結果與錯誤；加 `--callback 1.2` 可以接著測試第 1 筆結果的第 2 個動作。Beam 裡的「外掛偵錯紀錄」也看得到最近 30 次執行的 stdin / stdout / stderr。

## plugin.json

```json
{
  "name": "GitHub Repos",
  "description": "搜尋我的 repo",
  "icon": "glyph:E8A7",
  "keyword": "repo",
  "placeholder": "搜尋 repo…",
  "mode": "list",
  "run": "main.py",
  "filtering": "plugin",
  "timeoutMs": 5000,
  "debounceMs": 120
}
```

| 欄位 | 說明 |
|---|---|
| `name` | 顯示名稱（預設為資料夾名稱） |
| `description` | 根搜尋裡的副標題 |
| `icon` | 見〈圖示〉 |
| `keyword` | 在 Beam 輸入 `keyword` + 空白就直接進入外掛；不可含空白。和快速連結的 keyword 相同時，外掛優先 |
| `placeholder` | 進入外掛後搜尋框的提示文字 |
| `mode` | `list`（預設）：邊打邊回傳清單。`command`：按 Enter 執行一次 |
| `run` | 要執行的命令列，工作目錄是外掛資料夾。見〈run 的寫法〉 |
| `filtering` | `plugin`（預設）：每次輸入都呼叫外掛，由外掛自己篩選。`beam`：進入時只呼叫一次（query 為空字串），之後由 Beam 模糊篩選（適合啟動慢的 PowerShell 或資料固定的清單） |
| `timeoutMs` | 逾時毫秒數；list 預設 5000，command 預設 60000 |
| `debounceMs` | list 模式停止打字多久後才呼叫，預設 120 |

一個外掛可以有多個指令：放一個 `commands` 陣列，每個元素寫上面的欄位再加 `id`；沒寫的欄位沿用最外層的值。

```json
{
  "name": "WSL",
  "run": "wsl.ps1",
  "mode": "command",
  "commands": [
    { "id": "shutdown", "name": "關閉 WSL", "keyword": "wslstop" },
    { "id": "list", "name": "WSL 發行版", "mode": "list", "filtering": "beam" }
  ]
}
```

command 模式的 keyword 後面可以接參數：輸入 `wslstop 參數` 再按 Enter，參數會放在請求的 `query`。

### run 的寫法

外掛資料夾裡的檔案會自動配上直譯器，其他程式照寫（要在 PATH 上或寫完整路徑）。工作目錄是外掛資料夾。

**Windows**

- `main.py`、`main.js`、`main.ps1`、`run.bat`、`tool.exe`：
  - `.py` 用 `py`，沒有就用 `python`；`.js` 用 `node`；`.ps1` 用 `powershell -File`；`.bat`/`.cmd` 用 `cmd /c`。
- `python main.py --verbose`、`node dist/index.js`、`C:\tools\x.exe`：照寫。

**Linux**

- `main.py` 用 `python3`；`.js`/`.mjs`/`.cjs` 用 `node`；`.sh`/`.bash` 用 `bash`；`.ps1` 用 `pwsh -File`；`.rb`、`.pl`、`.php` 用 `ruby`、`perl`、`php`。
- 其他檔案：有執行權限就直接執行；沒有的話看第一行的 `#!`（例如 `#!/usr/bin/env python3`）。兩者都沒有時，Beam 會提示 `chmod +x`。
- `~/bin/tool`、`/opt/x/run --flag`、`python3 main.py --verbose`：照寫。命令列的引號規則和 shell 一樣（`'…'`、`"…"`、`\`），但**不經過 shell**：`|`、`>`、`$VAR`、`*` 都只是普通字元；需要 shell 功能就寫成腳本檔。

**`{query}`**

- `{query}` 會換成輸入的文字，當成**一個參數**；不需要自己加引號。
- 參數會被當成程式碼的地方，`{query}` 會自動停用（換成空字串），因為查詢裡的 `&`、`|`、`$()` 之類的字元可能被執行：
  - Windows：經過 `cmd`（包括 bat/cmd 檔）或沒有 `-File` 的 PowerShell。
  - Linux：當成程式碼的那個參數，例如 `bash -c` 後面的指令字串、`python3 -c`、`node -e`、`perl -e` 的程式碼、awk / sed 的程式（前面有 `timeout`、`env` 之類的也算）。其他參數照常，所以 `bash -c 'grep -- "$1" notes.txt' _ {query}` 是可以的。
- 這種外掛請讀 stdin 的 `query` 或環境變數 `BEAM_QUERY`。最安全的做法永遠是讀 stdin。

## 請求（stdin）

```json
{
  "version": 1,
  "type": "query",
  "command": "gh-repos",
  "query": "beam",
  "pluginDir": "C:\\Users\\you\\AppData\\Roaming\\Beam\\plugins\\gh-repos",
  "dataDir": "C:\\Users\\you\\AppData\\Roaming\\Beam\\plugin-data\\gh-repos"
}
```

`type` 的三種值：

- `query`：list 模式的查詢。
- `run`：command 模式按下 Enter。
- `action`：使用者選了帶 `callback` 的動作，此時另外附上 `item`（原本那筆結果）和 `action`（那個動作，含你放的 `data`）。

`dataDir` 是給外掛存快取、token 的資料夾。同樣的資訊也放在環境變數裡：`BEAM_REQUEST_TYPE`、`BEAM_QUERY`、`BEAM_COMMAND`、`BEAM_PLUGIN_DIR`、`BEAM_DATA_DIR`、`BEAM_VERSION`（環境變數有長度限制，`BEAM_QUERY` 最多 32,000 字；完整的查詢一定在 stdin 裡）。

## 回應（stdout）

```json
{
  "items": [
    {
      "title": "user/beam",
      "subtitle": "★ 12 · C++",
      "accessory": "repo",
      "icon": "glyph:E8A7:#3B82F6",
      "section": "我的 repo",
      "autocomplete": "repo user/beam",
      "open": "https://github.com/user/beam",
      "actions": [
        { "title": "複製 clone 網址", "copy": "git@github.com:user/beam.git", "shortcut": "Ctrl+Shift+C" },
        { "title": "在終端機 clone", "run": "git clone git@github.com:user/beam.git", "cwd": "D:\\src", "terminal": true },
        { "title": "重新整理", "callback": "refresh", "data": { "force": true } }
      ]
    }
  ],
  "message": "找到 1 個 repo"
}
```

**結果項目**

| 欄位 | 說明 |
|---|---|
| `title` | 必填 |
| `subtitle`、`accessory` | 標題旁的灰字、右側標籤 |
| `icon` | 見〈圖示〉；沒寫就用指令的圖示 |
| `section` | 分組標題，相鄰且相同的項目放在同一組（預設「結果」） |
| `autocomplete` | 按 Tab 時填入搜尋框的文字 |
| `id` | 選填 |
| 動作 | 直接寫在項目上的 `open` / `copy` / `run` / `setQuery` / `callback` 是 Enter 的主要動作；`actions` 陣列是 Ctrl+K 面板裡的其他動作（沒有主要動作時，第一個就是 Enter） |

簡寫：`"items": ["a", "b"]` 也可以，Enter 會複製該文字。沒有結果請回 `{"items": []}`；什麼都沒印出來會被當成外掛有問題，Beam 會提示去看偵錯紀錄。

**動作**

| 動作 | 效果 | 預設 |
|---|---|---|
| `open` | 用預設程式開啟網址、檔案或資料夾（相對路徑以外掛資料夾為準） | 關閉 Beam |
| `copy` | 複製文字 | 關閉 Beam |
| `run` | 執行命令列（寫法同〈run 的寫法〉）；`terminal: true` 會開終端機視窗（Windows：主控台；Linux：預設終端機，指令經過 bash，執行完留在 shell），`cwd` 指定工作目錄（預設外掛資料夾） | 關閉 Beam |
| `setQuery` | 把文字填入搜尋框（可做多層選單） | 保持開啟 |
| `callback` | 用 `type: "action"` 再呼叫外掛一次；回應會取代目前清單，或回傳 message / open / copy… | 保持開啟 |

每個動作都可以加 `title`、`shortcut`（例如 `"Ctrl+Shift+C"`、`"Alt+Enter"`），以及 `keepOpen`（覆寫上表的預設）。第二個動作沒有指定 `shortcut` 時，自動是 `Ctrl+Enter`。

**回應最外層的其他欄位**

| 欄位 | 說明 |
|---|---|
| `message` | 顯示在 Beam 底部狀態列；Beam 已關閉時改用系統通知 |
| `error` | 同上，用警告色 |
| `placeholder` | 更新搜尋框的提示文字 |
| `output` | 長文字，用詳細檢視顯示（等寬字型，可捲動，Enter 複製全部）。只用於 `run` / `action` |
| `open` / `copy` / `run` / `setQuery` | 收到回應就直接執行。只用於 `run` / `action` |
| `hide` | `true` / `false`：執行完是否關閉 Beam。只用於 `run` / `action` |

`query` 的回應只看 `items`、`message`、`error`、`placeholder`（每打一個字就跑一次，不適合自動開網頁）。

command 模式執行時，Beam 底部會顯示「執行中…」。結束後：
- 只有 `message`：關閉 Beam，用系統通知顯示訊息。
- 有 `items`：進入外掛的清單，由 Beam 篩選。
- 有 `output`：開啟詳細檢視。

command 模式的輸出如果不是 JSON，單行文字會當成 `message`，多行文字會當成 `output`。所以一般腳本直接 `echo` 也能用。

## 圖示

- `"glyph:E8A7"`：Segoe MDL2 Assets 圖示碼；可加顏色 `"glyph:E8A7:#3B82F6"`。Linux 上會換成意思相近的系統圖示（常用的幾十個，其他顯示拼圖圖示），顏色照用。
- `"icon.png"`：外掛資料夾裡的圖片（png / jpg / ico / bmp / gif；Linux 也可以用 svg）。
- `"C:\\Program Files\\App\\app.exe"`：檔案或程式本身的圖示（Windows）。
- `"icon:utilities-terminal"`、`"icon:system-search-symbolic:#3B82F6"`：Linux 圖示主題裡的圖示名稱；`-symbolic` 結尾的單色圖示會放在彩色方塊上（Windows 不支援，顯示外掛的預設圖示）。

## 編碼（重要）

stdin 與 stdout 一律用 **UTF-8**，否則中文會變亂碼。

- **Python**：Beam 已設定 `PYTHONUTF8=1`，`json.load(sys.stdin)` 和 `print(json.dumps(..., ensure_ascii=False))` 可直接用。
- **Node / Go / Rust**：預設就是 UTF-8。
- **Linux 上的 bash / 其他程式**：系統本來就是 UTF-8，不用特別處理。bash 範本示範了怎麼把文字轉成 JSON 字串；要解析請求裡的 JSON（例如 callback 的 `data`），建議改用 Python 範本。
- **Windows PowerShell 5.1**：
  - 腳本開頭設定 `[Console]::InputEncoding` 與 `[Console]::OutputEncoding` 為 `[Text.Encoding]::UTF8`。
  - `.ps1` 檔要存成 **UTF-8 with BOM**。
  - 範本已經處理好這兩點。
- **bat**：開頭加 `chcp 65001 >nul` 與 `setlocal EnableDelayedExpansion`，再用 `!BEAM_QUERY!` 讀輸入（用 `%BEAM_QUERY%` 的話，查詢裡的 `&`、`|` 會被 cmd 當成指令）。

```bat
@echo off
chcp 65001 >nul
setlocal EnableDelayedExpansion
echo {"items":[{"title":"你輸入了 !BEAM_QUERY!"}]}
```

## 行為細節

- 一個外掛只要輸出一個 JSON（物件或陣列），印完就結束。
- list 模式：
  - 新的輸入會中止還在跑的舊查詢，連同它開出的子行程。
  - 結束代碼非 0 且沒有 JSON 輸出，視為失敗，stderr 最後一行會顯示在 Beam 裡。
- command 模式：外掛開出的程式（例如用 `run` 開 GUI 程式）不會被關閉，關閉 Beam 也不會。
- 逾時：外掛和它的所有子行程都會被結束（Windows 用 job object，Linux 用 process group）。
- 在外掛裡按 Backspace（搜尋框是空的時）回到根搜尋並保留 keyword；按 Esc 先清空搜尋框，再按一次回到根搜尋。
