#include "RemapWebWindow.h"
#include "PaddleConfig.h"
#include "logging/Log.h"

#include <shlwapi.h>
#include <string>
#include <vector>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

RemapWebWindow* RemapWebWindow::s_instance = nullptr;

namespace {

struct ButtonMeta {
    int          index;
    const wchar_t* name;
    const wchar_t* group;
};

// Order defines display order. Indices match GetButtonAction() in
// VirtualController.h — the whole point of this editor over the old one is that
// all 22 are reachable, not just the five paddles.
constexpr ButtonMeta kButtons[] = {
    {  0, L"L4",          L"Back paddles" },
    {  1, L"L5",          L"Back paddles" },
    {  2, L"R4",          L"Back paddles" },
    {  3, L"R5",          L"Back paddles" },
    {  4, L"QAM",         L"Back paddles" },
    {  5, L"A",           L"Face buttons" },
    {  6, L"B",           L"Face buttons" },
    {  7, L"X",           L"Face buttons" },
    {  8, L"Y",           L"Face buttons" },
    {  9, L"LB",          L"Bumpers" },
    { 10, L"RB",          L"Bumpers" },
    { 20, L"L2",          L"Triggers" },
    { 21, L"R2",          L"Triggers" },
    { 16, L"D-Pad Up",    L"D-Pad" },
    { 17, L"D-Pad Down",  L"D-Pad" },
    { 18, L"D-Pad Left",  L"D-Pad" },
    { 19, L"D-Pad Right", L"D-Pad" },
    { 14, L"L3",          L"Sticks" },
    { 15, L"R3",          L"Sticks" },
    { 11, L"View",        L"System" },
    { 12, L"Menu",        L"System" },
    { 13, L"Guide",       L"System" },
};

// Menu-mapping fallback for the five paddles, used by Describe when an action is
// UseMenuMapping. Standard buttons pass through to themselves.
PaddleMapping FallbackFor(int index, const PaddleMappings& m) {
    switch (index) {
    case 0: return m.l4;
    case 1: return m.l5;
    case 2: return m.r4;
    case 3: return m.r5;
    case 4: return m.qam;
    default: return PaddleMapping::None;
    }
}

std::wstring JsonEscape(const std::wstring& in) {
    std::wstring out;
    out.reserve(in.size() + 8);
    for (wchar_t c : in) {
        switch (c) {
        case L'"':  out += L"\\\""; break;
        case L'\\': out += L"\\\\"; break;
        case L'\n': out += L"\\n";  break;
        case L'\r': out += L"\\r";  break;
        case L'\t': out += L"\\t";  break;
        default:
            if (c < 0x20) {
                wchar_t buf[8];
                swprintf_s(buf, L"\\u%04X", static_cast<unsigned>(c));
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out;
}

// Minimal field extraction. The only producer is our own page, so a full parser
// would be more machinery than the two message shapes justify.
bool JsonStringField(const std::wstring& json, const wchar_t* key, std::wstring& out) {
    const std::wstring needle = std::wstring(L"\"") + key + L"\"";
    size_t pos = json.find(needle);
    if (pos == std::wstring::npos) return false;
    pos = json.find(L':', pos + needle.size());
    if (pos == std::wstring::npos) return false;
    pos = json.find(L'"', pos);
    if (pos == std::wstring::npos) return false;
    ++pos;

    out.clear();
    for (; pos < json.size(); ++pos) {
        const wchar_t c = json[pos];
        if (c == L'\\' && pos + 1 < json.size()) {
            const wchar_t esc = json[++pos];
            switch (esc) {
            case L'n': out += L'\n'; break;
            case L'r': out += L'\r'; break;
            case L't': out += L'\t'; break;
            default:   out += esc;   break;
            }
            continue;
        }
        if (c == L'"') return true;
        out += c;
    }
    return false;
}

bool JsonIntField(const std::wstring& json, const wchar_t* key, int& out) {
    const std::wstring needle = std::wstring(L"\"") + key + L"\"";
    size_t pos = json.find(needle);
    if (pos == std::wstring::npos) return false;
    pos = json.find(L':', pos + needle.size());
    if (pos == std::wstring::npos) return false;
    ++pos;
    while (pos < json.size() && (json[pos] == L' ' || json[pos] == L'\t')) ++pos;

    bool any = false;
    int value = 0;
    bool negative = false;
    if (pos < json.size() && json[pos] == L'-') { negative = true; ++pos; }
    for (; pos < json.size() && json[pos] >= L'0' && json[pos] <= L'9'; ++pos) {
        value = value * 10 + (json[pos] - L'0');
        any = true;
    }
    if (!any) return false;
    out = negative ? -value : value;
    return true;
}

// Palette and metrics deliberately mirror SteamlessController's editor so the
// two read as the same tool: Steam's client blues, its text greys, its accent.
const char* kHtml = R"HTML(<!DOCTYPE html>
<html><head><meta charset="utf-8"><style>
:root{
  --bg-top:#1b2838; --bg-bot:#15202c; --panel:#171a21;
  --accent:#66c0f4; --accent-deep:#1a9fff;
  --text:#ffffff; --text-dim:#c7d5e0; --text-mute:#8f98a0; --text-faint:#5c6b78;
  --green:#5ba32b; --red:#c0392b;
}
*{box-sizing:border-box}
html,body{margin:0;height:100%}
body{
  font-family:'Barlow',system-ui,'Segoe UI',sans-serif;
  background:linear-gradient(180deg,var(--bg-top) 0%,var(--bg-bot) 100%);
  color:var(--text); font-size:14px; display:flex; flex-direction:column;
  overflow:hidden;
}
header{
  padding:14px 20px; background:var(--panel);
  display:flex; align-items:baseline; gap:12px; flex:0 0 auto;
}
header h1{margin:0; font-size:17px; font-weight:600; letter-spacing:.2px}
header .profile{color:var(--accent); font-size:13px}
main{flex:1 1 auto; overflow-y:auto; padding:16px 20px 8px}
.group{margin-bottom:18px}
.group>h2{
  margin:0 0 8px; font-size:11px; letter-spacing:1.4px; text-transform:uppercase;
  color:var(--text-faint); font-weight:600;
}
.rows{background:rgba(255,255,255,.025); border-radius:8px; overflow:hidden}
.row{
  display:flex; align-items:center; gap:12px; padding:9px 14px; cursor:pointer;
  border-left:3px solid transparent;
}
.row+.row{border-top:1px solid rgba(255,255,255,.04)}
.row:hover{background:rgba(255,255,255,.05)}
.row.sel{background:rgba(102,192,244,.10); border-left-color:var(--accent)}
.row .name{width:110px; flex:0 0 auto; font-weight:600; color:var(--text-dim)}
.row .desc{flex:1 1 auto; color:var(--text-mute); font-family:'JetBrains Mono',ui-monospace,monospace; font-size:12.5px}
.row.bound .desc{color:var(--accent)}
.row.unmapped .desc{color:var(--text-faint); font-style:italic}
footer{
  flex:0 0 auto; background:var(--panel); padding:12px 20px;
  display:flex; align-items:center; gap:10px;
}
.editor{
  background:#1b2a3a; border-radius:10px; padding:14px; margin-bottom:14px;
  display:grid; grid-template-columns:auto 1fr; gap:10px 12px; align-items:center;
}
.editor .full{grid-column:1 / -1}
label{color:var(--text-mute); font-size:12.5px}
select,input[type=text]{
  width:100%; padding:7px 9px; border-radius:6px; color:var(--text);
  background:rgba(255,255,255,.06); border:1px solid rgba(255,255,255,.10);
  font-family:inherit; font-size:13px;
}
select:focus,input:focus{outline:none; border-color:var(--accent)}
button{
  padding:8px 16px; border-radius:6px; border:1px solid rgba(255,255,255,.12);
  background:rgba(255,255,255,.06); color:var(--text-dim);
  font-family:inherit; font-size:13px; cursor:pointer;
}
button:hover{background:rgba(255,255,255,.11); color:var(--text)}
button.primary{
  background:linear-gradient(135deg,var(--accent-deep),#0a4a78);
  border-color:transparent; color:#fff; font-weight:600;
}
button.primary:hover{filter:brightness(1.12)}
.spacer{flex:1 1 auto}
.hint{color:var(--text-faint); font-size:12px}
.chk{display:flex; align-items:center; gap:7px; color:var(--text-mute); font-size:12.5px}
</style></head><body>
<header>
  <h1>Remap Buttons</h1>
  <span class="profile" id="prof"></span>
</header>
<main>
  <div id="ed" class="editor" style="display:none">
    <label for="type">Action</label>
    <select id="type">
      <option value="MENU">Passthrough (default)</option>
      <option value="NONE">Unmapped</option>
      <option value="GAMEPAD">Gamepad button</option>
      <option value="KEY">Keyboard shortcut</option>
    </select>
    <label for="pad" id="padL">Target</label>
    <select id="pad">
      <option>A</option><option>B</option><option>X</option><option>Y</option>
      <option>LB</option><option>RB</option><option>L3</option><option>R3</option>
      <option>View</option><option>Menu</option><option>Guide</option>
      <option>DPadUp</option><option>DPadDown</option><option>DPadLeft</option><option>DPadRight</option>
    </select>
    <label for="key" id="keyL">Keys</label>
    <input type="text" id="key" placeholder="CTRL+SHIFT+F1">
    <div class="full chk"><input type="checkbox" id="rapid"><label for="rapid">Rapid fire while held</label></div>
    <div class="full"><button class="primary" id="apply">Apply to <span id="applyName"></span></button></div>
  </div>
  <div id="groups"></div>
</main>
<footer>
  <span class="hint" id="hint">Select a button to rebind it.</span>
  <span class="spacer"></span>
  <button id="adv">Macros &amp; profiles…</button>
  <button id="close">Close</button>
</footer>
<script>
let state={buttons:[],profile:''}, sel=null;
const $=id=>document.getElementById(id);
const send=o=>window.chrome.webview.postMessage(JSON.stringify(o));

function classify(d){
  if(!d) return '';
  if(d==='Unmapped') return 'unmapped';
  return 'bound';
}

function render(){
  $('prof').textContent = state.profile ? 'profile: '+state.profile : '';
  const seen=[], host=$('groups');
  host.innerHTML='';
  for(const b of state.buttons){
    if(seen.indexOf(b.group)>=0) continue;
    seen.push(b.group);
    const g=document.createElement('div'); g.className='group';
    const h=document.createElement('h2'); h.textContent=b.group; g.appendChild(h);
    const rows=document.createElement('div'); rows.className='rows';
    for(const c of state.buttons.filter(x=>x.group===b.group)){
      const r=document.createElement('div');
      r.className='row '+classify(c.desc)+(sel===c.index?' sel':'');
      r.onclick=()=>{sel=c.index; syncEditor(); render();};
      const n=document.createElement('div'); n.className='name'; n.textContent=c.name;
      const d=document.createElement('div'); d.className='desc'; d.textContent=c.desc;
      r.appendChild(n); r.appendChild(d); rows.appendChild(r);
    }
    g.appendChild(rows); host.appendChild(g);
  }
}

function cur(){ return state.buttons.find(b=>b.index===sel); }

function syncEditor(){
  const b=cur();
  $('ed').style.display = b ? 'grid' : 'none';
  if(!b) return;
  $('applyName').textContent=b.name;
  $('hint').textContent='Editing '+b.name+'.';
  typeChanged();
}

function typeChanged(){
  const t=$('type').value;
  const padOn = t==='GAMEPAD', keyOn = t==='KEY';
  $('pad').style.display=padOn?'':'none';   $('padL').style.display=padOn?'':'none';
  $('key').style.display=keyOn?'':'none';   $('keyL').style.display=keyOn?'':'none';
  $('rapid').disabled = !(padOn||keyOn);
}

$('type').onchange=typeChanged;
$('close').onclick=()=>send({cmd:'close'});
$('adv').onclick=()=>send({cmd:'advanced'});
$('apply').onclick=()=>{
  const b=cur(); if(!b) return;
  const t=$('type').value;
  let a=t;
  if(t==='GAMEPAD') a='GAMEPAD:'+$('pad').value;
  else if(t==='KEY'){
    const k=$('key').value.trim();
    if(!k){ $('hint').textContent='Type a shortcut first, e.g. CTRL+C.'; return; }
    a='KEY:'+k;
  }
  if((t==='GAMEPAD'||t==='KEY') && $('rapid').checked) a+='|RAPID';
  send({cmd:'set', index:b.index, action:a});
};

window.chrome.webview.addEventListener('message', e=>{
  state=JSON.parse(e.data);
  if(sel!==null && !cur()) sel=null;
  render(); syncEditor();
});
send({cmd:'ready'});
</script></body></html>)HTML";

}  // namespace

RemapWebWindow::~RemapWebWindow() {
    Close();
    if (s_instance == this)
        s_instance = nullptr;
}

bool RemapWebWindow::IsRuntimeAvailable() {
    LPWSTR version = nullptr;
    const HRESULT hr = GetAvailableCoreWebView2BrowserVersionString(nullptr, &version);
    const bool ok = SUCCEEDED(hr) && version != nullptr;
    if (version)
        CoTaskMemFree(version);
    logging::Logf("[RemapWeb] WebView2 runtime %s", ok ? "available" : "missing");
    return ok;
}

bool RemapWebWindow::Open(HINSTANCE hInst, const std::wstring& profileId,
                          QueryFn query, ApplyFn apply, OpenAdvancedFn openAdvanced) {
    m_profileId    = profileId;
    m_query        = std::move(query);
    m_apply        = std::move(apply);
    m_openAdvanced = std::move(openAdvanced);

    if (m_hwnd) {
        BringToFront();
        PostState();
        return true;
    }

    m_hInst    = hInst;
    s_instance = this;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = CLASS_NAME;
    // Unsuffixed LoadCursor, matching the rest of the codebase: UNICODE is not
    // defined project-wide, so IDC_ARROW expands to an LPSTR.
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    // Matches the page background so the frame does not flash white before the
    // WebView2 controller finishes initialising asynchronously.
    wc.hbrBackground = CreateSolidBrush(RGB(0x15, 0x20, 0x2c));
    RegisterClassExW(&wc);  // harmless if already registered

    m_hwnd = CreateWindowExW(0, CLASS_NAME, L"Remap Buttons",
                             WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, WINDOW_W, WINDOW_H,
                             nullptr, nullptr, hInst, nullptr);
    if (!m_hwnd) {
        logging::Logf("[RemapWeb] CreateWindow failed error=%lu", GetLastError());
        return false;
    }

    ShowWindow(m_hwnd, SW_SHOW);
    CreateWebViewAsync(m_hwnd);
    return true;
}

void RemapWebWindow::Close() {
    if (m_controller) {
        m_controller->Close();
        m_controller.Reset();
    }
    m_webview.Reset();
    m_env.Reset();
    if (m_hwnd) {
        HWND h = m_hwnd;
        m_hwnd = nullptr;
        DestroyWindow(h);
    }
}

void RemapWebWindow::BringToFront() const {
    if (!m_hwnd) return;
    ShowWindow(m_hwnd, SW_SHOW);
    SetForegroundWindow(m_hwnd);
}

void RemapWebWindow::Refresh(const std::wstring& profileId) {
    m_profileId = profileId;
    PostState();
}

void RemapWebWindow::ResizeWebView() {
    if (!m_controller || !m_hwnd) return;
    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    m_controller->put_Bounds(rc);
}

void RemapWebWindow::CreateWebViewAsync(HWND hwnd) {
    // User-data folder under LOCALAPPDATA: the default is beside the exe, which
    // fails when installed to Program Files without write access.
    const std::wstring dataDir = logging::BuildAppDataDirectory(L"NonSteamController\\WebView2");

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, dataDir.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this, hwnd](HRESULT res, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(res) || !env) {
                    logging::Logf("[RemapWeb] environment creation failed hr=0x%08lX",
                                  static_cast<unsigned long>(res));
                    return res;
                }
                m_env = env;
                return env->CreateCoreWebView2Controller(
                    hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this](HRESULT res2, ICoreWebView2Controller* ctrl) -> HRESULT {
                            if (FAILED(res2) || !ctrl) {
                                logging::Logf("[RemapWeb] controller creation failed hr=0x%08lX",
                                              static_cast<unsigned long>(res2));
                                return res2;
                            }
                            m_controller = ctrl;
                            m_controller->get_CoreWebView2(&m_webview);
                            if (!m_webview) return E_FAIL;

                            ComPtr<ICoreWebView2Settings> settings;
                            if (SUCCEEDED(m_webview->get_Settings(&settings)) && settings) {
                                settings->put_AreDefaultContextMenusEnabled(FALSE);
                                settings->put_IsStatusBarEnabled(FALSE);
                                settings->put_AreDevToolsEnabled(FALSE);
                            }

                            m_webview->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [this](ICoreWebView2*,
                                           ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        LPWSTR raw = nullptr;
                                        if (SUCCEEDED(args->TryGetWebMessageAsString(&raw)) && raw) {
                                            OnWebMessage(raw);
                                            CoTaskMemFree(raw);
                                        }
                                        return S_OK;
                                    }).Get(),
                                nullptr);

                            ResizeWebView();
                            const std::wstring html(kHtml, kHtml + strlen(kHtml));
                            m_webview->NavigateToString(html.c_str());
                            return S_OK;
                        }).Get());
            }).Get());

    if (FAILED(hr))
        logging::Logf("[RemapWeb] CreateCoreWebView2EnvironmentWithOptions failed hr=0x%08lX",
                      static_cast<unsigned long>(hr));
}

void RemapWebWindow::PostState() {
    if (!m_webview || !m_query) return;

    const PaddleActionBindings bindings = m_query();
    PaddleMappings mappings{};  // menu fallbacks are not edited here

    std::wstring json = L"{\"profile\":\"" + JsonEscape(m_profileId) + L"\",\"buttons\":[";
    bool first = true;
    for (const ButtonMeta& meta : kButtons) {
        const PaddleAction* action = GetButtonAction(bindings, meta.index);
        const std::wstring desc =
            action ? PaddleConfig::Describe(*action, FallbackFor(meta.index, mappings))
                   : L"Unmapped";
        if (!first) json += L',';
        first = false;
        json += L"{\"index\":" + std::to_wstring(meta.index) +
                L",\"name\":\"" + JsonEscape(meta.name) + L"\"" +
                L",\"group\":\"" + JsonEscape(meta.group) + L"\"" +
                L",\"desc\":\"" + JsonEscape(desc) + L"\"}";
    }
    json += L"]}";

    m_webview->PostWebMessageAsString(json.c_str());
}

void RemapWebWindow::OnWebMessage(const std::wstring& json) {
    std::wstring cmd;
    if (!JsonStringField(json, L"cmd", cmd))
        return;

    if (cmd == L"ready") {
        PostState();
        return;
    }
    if (cmd == L"close") {
        Close();
        return;
    }
    if (cmd == L"advanced") {
        if (m_openAdvanced) m_openAdvanced();
        return;
    }
    if (cmd == L"set") {
        int index = -1;
        std::wstring action;
        if (!JsonIntField(json, L"index", index) ||
            !JsonStringField(json, L"action", action))
            return;
        if (m_apply) m_apply(index, action);
        PostState();  // echo back what actually took effect
        return;
    }
}

LRESULT CALLBACK RemapWebWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (s_instance)
        return s_instance->HandleMessage(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT RemapWebWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        ResizeWebView();
        return 0;
    case WM_CLOSE:
        Close();
        return 0;
    case WM_DESTROY:
        m_hwnd = nullptr;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
