// Starter plugins created by "建立新外掛" / `beam-plugin new`.
#include <string>
#include <vector>

#include "core/json.hpp"
#include "core/text.hpp"
#include "plugin/plugin_manager.hpp"

namespace beam {
namespace {

std::string Replace(std::string text, const std::string& from, const std::string& to) {
    for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
        text.replace(at, from.size(), to);
    return text;
}

// The name goes into string literals of four languages; keep it inert.
std::string SafeName(const std::wstring& name) {
    std::wstring safe;
    for (const wchar_t c : name)
        if (c >= 0x20 && c != L'"' && c != L'\'' && c != L'\\' && c != L'`' && c != L'$' && c != L'{' && c != L'}')
            safe.push_back(c);
    return WideToUtf8(safe.empty() ? L"My Plugin" : safe);
}

const char* kPython = R"PY(# Beam 外掛：{{name}}
# Beam 會把一個 JSON 請求寫到 stdin，外掛把一個 JSON 回應印到 stdout。
# 除錯訊息請印到 stderr（Beam 的「外掛偵錯紀錄」看得到）。完整協定見 PLUGINS.md。
import json
import sys
import urllib.parse

request = json.load(sys.stdin)       # {"type": "query" | "action", "query": "...", ...}
query = request.get("query", "").strip()

if request.get("type") == "action":  # 使用者選了帶 "callback" 的動作
    action = request.get("action", {})
    print(json.dumps({"message": f"收到動作：{action.get('callback')}", "hide": False}, ensure_ascii=False))
    sys.exit(0)

items = []
if not query:
    items.append({"title": "輸入任何文字試試看", "subtitle": "這是「{{name}}」外掛的範例", "icon": "glyph:E946"})
else:
    items.append({
        "title": f"搜尋「{query}」",
        "subtitle": "Enter 在瀏覽器開啟，Ctrl+K 看更多動作",
        "icon": "glyph:E721",
        "actions": [
            {"title": "在瀏覽器開啟", "open": "https://www.google.com/search?q=" + urllib.parse.quote(query)},
            {"title": "複製文字", "copy": query, "shortcut": "Ctrl+Shift+C"},
        ],
    })
    items.append({"title": query.upper(), "subtitle": "轉成大寫，Enter 複製", "copy": query.upper()})
    items.append({"title": "示範 callback", "subtitle": "Beam 會帶著這個動作再呼叫一次外掛",
                  "callback": "demo", "data": {"query": query}})

print(json.dumps({"items": items}, ensure_ascii=False))
)PY";

const char* kNode = R"JS(// Beam 外掛：{{name}}
// Beam 會把一個 JSON 請求寫到 stdin，外掛把一個 JSON 回應印到 stdout。
// 除錯訊息請用 console.error（Beam 的「外掛偵錯紀錄」看得到）。完整協定見 PLUGINS.md。
let input = '';
process.stdin.setEncoding('utf8');
process.stdin.on('data', (chunk) => (input += chunk));
process.stdin.on('end', () => {
  const request = JSON.parse(input || '{}'); // {type: "query" | "action", query: "...", ...}
  const query = (request.query || '').trim();

  if (request.type === 'action') {
    return reply({ message: `收到動作：${request.action.callback}`, hide: false });
  }

  const items = [];
  if (!query) {
    items.push({ title: '輸入任何文字試試看', subtitle: '這是「{{name}}」外掛的範例', icon: 'glyph:E946' });
  } else {
    items.push({
      title: `搜尋「${query}」`,
      subtitle: 'Enter 在瀏覽器開啟，Ctrl+K 看更多動作',
      icon: 'glyph:E721',
      actions: [
        { title: '在瀏覽器開啟', open: 'https://www.google.com/search?q=' + encodeURIComponent(query) },
        { title: '複製文字', copy: query, shortcut: 'Ctrl+Shift+C' },
      ],
    });
    items.push({ title: query.toUpperCase(), subtitle: '轉成大寫，Enter 複製', copy: query.toUpperCase() });
    items.push({ title: '示範 callback', subtitle: 'Beam 會帶著這個動作再呼叫一次外掛', callback: 'demo', data: { query } });
  }
  reply({ items });
});

function reply(response) {
  process.stdout.write(JSON.stringify(response));
}
)JS";

const char* kPowerShell = R"PS(# Beam 外掛：{{name}}
# Beam 會把一個 JSON 請求寫到 stdin，外掛把一個 JSON 回應印到 stdout。
# 除錯訊息請用 [Console]::Error.WriteLine（Beam 的「外掛偵錯紀錄」看得到）。完整協定見 PLUGINS.md。
# 提示：Windows PowerShell 每次啟動約 0.3~1 秒；邊打邊查的外掛建議在 plugin.json 設 "filtering": "beam"。
[Console]::InputEncoding = [System.Text.Encoding]::UTF8
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$request = [Console]::In.ReadToEnd() | ConvertFrom-Json
$query = "$($request.query)".Trim()

function Reply($response) {
    [Console]::Out.Write(($response | ConvertTo-Json -Depth 10 -Compress))
}

if ($request.type -eq 'action') {
    Reply @{ message = "收到動作：$($request.action.callback)"; hide = $false }
    exit 0
}

$items = @()
if ($query -eq '') {
    $items += @{ title = '輸入任何文字試試看'; subtitle = '這是「{{name}}」外掛的範例'; icon = 'glyph:E946' }
} else {
    $items += @{
        title    = "搜尋「$query」"
        subtitle = 'Enter 在瀏覽器開啟，Ctrl+K 看更多動作'
        icon     = 'glyph:E721'
        actions  = @(
            @{ title = '在瀏覽器開啟'; open = "https://www.google.com/search?q=$([uri]::EscapeDataString($query))" },
            @{ title = '複製文字'; copy = $query; shortcut = 'Ctrl+Shift+C' }
        )
    }
    $items += @{ title = $query.ToUpper(); subtitle = '轉成大寫，Enter 複製'; copy = $query.ToUpper() }
    $items += @{ title = '示範 callback'; subtitle = 'Beam 會帶著這個動作再呼叫一次外掛'; callback = 'demo' }
}
Reply @{ items = $items }
)PS";

const char* kShell = R"SH(#!/usr/bin/env bash
# Beam 外掛：{{name}}
# Beam 會把一個 JSON 請求寫到 stdin，同樣的資訊也放在環境變數 BEAM_QUERY、BEAM_REQUEST_TYPE…；
# 外掛把一個 JSON 回應印到 stdout。除錯訊息請寫到 stderr（Beam 的「外掛偵錯紀錄」看得到）。
# 完整協定見 PLUGINS.md。需要解析請求裡的 JSON（例如 callback 的 data）時，建議改用 Python 範本。
set -euo pipefail

json() {  # 文字 -> JSON 字串
  local s=$1
  s=${s//\\/\\\\}; s=${s//\"/\\\"}
  s=${s//$'\n'/\\n}; s=${s//$'\r'/\\r}; s=${s//$'\t'/\\t}
  printf '"%s"' "$s"
}

urlencode() {
  local h out=
  for h in $(printf '%s' "$1" | od -An -v -tx1); do
    case $h in
      3[0-9] | 4[1-9a-f] | 5[0-9a] | 6[1-9a-f] | 7[0-9a] | 2d | 2e | 5f | 7e) out+=$(printf "\\x$h") ;;
      *) out+="%${h^^}" ;;
    esac
  done
  printf '%s' "$out"
}

query=${BEAM_QUERY:-}
type=${BEAM_REQUEST_TYPE:-query}

if [[ $type == action ]]; then  # 使用者選了帶 "callback" 的動作
  printf '{"message": %s, "hide": false}\n' "$(json "收到動作")"
  exit 0
fi

if [[ -z $query ]]; then
  printf '{"items": [{"title": %s, "subtitle": %s, "icon": "glyph:E946"}]}\n' \
    "$(json "輸入任何文字試試看")" "$(json "這是「{{name}}」外掛的範例")"
  exit 0
fi

upper=${query^^}
cat <<JSON
{"items": [
  {"title": $(json "搜尋「$query」"), "subtitle": "Enter 在瀏覽器開啟，Ctrl+K 看更多動作", "icon": "glyph:E721",
   "actions": [
     {"title": "在瀏覽器開啟", "open": "https://www.google.com/search?q=$(urlencode "$query")"},
     {"title": "複製文字", "copy": $(json "$query"), "shortcut": "Ctrl+Shift+C"}]},
  {"title": $(json "$upper"), "subtitle": "轉成大寫，Enter 複製", "copy": $(json "$upper")},
  {"title": "示範 callback", "subtitle": "Beam 會帶著這個動作再呼叫一次外掛", "callback": "demo"}
]}
JSON
)SH";

}  // namespace

const wchar_t* PluginScriptFileName(PluginTemplate language) {
    switch (language) {
        case PluginTemplate::Python: return L"main.py";
        case PluginTemplate::Node: return L"main.js";
        case PluginTemplate::PowerShell: return L"main.ps1";
        case PluginTemplate::Shell: return L"main.sh";
    }
    return L"main.py";
}

const wchar_t* PluginTemplateLabel(PluginTemplate language) {
    switch (language) {
        case PluginTemplate::Python: return L"Python";
        case PluginTemplate::Node: return L"Node.js";
        case PluginTemplate::PowerShell: return L"PowerShell";
        case PluginTemplate::Shell: return L"Shell";
    }
    return L"Python";
}

std::vector<PluginTemplate> PlatformPluginTemplates() {
#ifdef _WIN32
    return {PluginTemplate::Python, PluginTemplate::Node, PluginTemplate::PowerShell};
#else
    return {PluginTemplate::Python, PluginTemplate::Node, PluginTemplate::Shell};
#endif
}

std::string PluginScriptTemplate(PluginTemplate language, const std::wstring& name) {
    const char* text = language == PluginTemplate::Node         ? kNode
                       : language == PluginTemplate::PowerShell ? kPowerShell
                       : language == PluginTemplate::Shell      ? kShell
                                                                : kPython;
    std::string out = Replace(text, "{{name}}", SafeName(name));
    if (language == PluginTemplate::PowerShell) out = Replace(out, "\n", "\r\n");
    return out;
}

std::string PluginManifestTemplate(PluginTemplate language, const std::wstring& name, const std::wstring& keyword) {
    Json m = Json::MakeObject();
    m.Set("name", Json(WideToUtf8(name)));
    m.Set("description", Json(std::string("我的 Beam 外掛（範本）")));
    m.Set("icon", Json(std::string("glyph:EA86")));
    m.Set("keyword", Json(WideToUtf8(keyword)));
    m.Set("placeholder", Json(std::string("輸入要查詢的內容…")));
    m.Set("mode", Json(std::string("list")));
    m.Set("run", Json(WideToUtf8(PluginScriptFileName(language))));
    if (language == PluginTemplate::PowerShell) m.Set("debounceMs", 300);  // slow to start; wait for a pause
    return m.Dump();
}

}  // namespace beam
