#include "RemapWebWindow.h"
#include "PaddleConfig.h"
#include "logging/Log.h"
#include "resource.h"

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
    // Position on the controller artwork, as a percentage of the image box.
    // Derived from the Win32 editor's hotspot table, which placed the image at
    // Rect(20, 32, 560, 379) — these are those anchors normalised, so the two
    // editors mark the same physical buttons.
    double xPct;
    double yPct;
};

// Order defines display order. Indices match GetButtonAction() in
// VirtualController.h — the whole point of this editor over the old one is that
// all 22 are reachable, not just the five paddles.
constexpr ButtonMeta kButtons[] = {
    {  0, L"L4",          L"Back paddles", 30.7, 57.8 },
    {  1, L"L5",          L"Back paddles", 28.6, 72.3 },
    {  2, L"R4",          L"Back paddles", 69.6, 57.8 },
    {  3, L"R5",          L"Back paddles", 71.6, 72.3 },
    {  4, L"QAM",         L"Back paddles", 49.1, 70.7 },
    {  5, L"A",           L"Face buttons", 84.5, 43.5 },
    {  6, L"B",           L"Face buttons", 89.6, 36.4 },
    {  7, L"X",           L"Face buttons", 79.3, 36.4 },
    {  8, L"Y",           L"Face buttons", 84.5, 29.3 },
    {  9, L"LB",          L"Bumpers",      11.8, 10.5 },
    { 10, L"RB",          L"Bumpers",      84.6, 10.5 },
    { 20, L"L2",          L"Triggers",     11.6,  3.5 },
    { 21, L"R2",          L"Triggers",     84.6,  3.5 },
    { 16, L"D-Pad Up",    L"D-Pad",        21.1, 27.4 },
    { 17, L"D-Pad Down",  L"D-Pad",        21.1, 42.5 },
    { 18, L"D-Pad Left",  L"D-Pad",        15.7, 35.1 },
    { 19, L"D-Pad Right", L"D-Pad",        27.1, 35.1 },
    { 14, L"L3",          L"Sticks",       32.7, 45.1 },
    { 15, L"R3",          L"Sticks",       63.8, 45.1 },
    { 11, L"View",        L"System",       39.6, 40.6 },
    { 12, L"Menu",        L"System",       56.5, 40.6 },
    { 13, L"Guide",       L"System",       48.0, 46.7 },
};

// The controller artwork is already embedded as RCDATA for the Win32 editor.
// WebView2 has no access to module resources, so it is read out and inlined as a
// data: URI. Cached because it is ~240 KB of base64 and never changes.
std::string ControllerImageDataUri() {
    static std::string cached;
    static bool tried = false;
    if (tried) return cached;
    tried = true;

    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_CONTROLLER_IMAGE),
                              reinterpret_cast<LPCWSTR>(RT_RCDATA));
    if (!res) {
        logging::Logf("[RemapWeb] controller image resource not found");
        return cached;
    }
    HGLOBAL handle = LoadResource(nullptr, res);
    const DWORD size = SizeofResource(nullptr, res);
    const void* data = handle ? LockResource(handle) : nullptr;
    if (!data || size == 0) {
        logging::Logf("[RemapWeb] controller image resource empty");
        return cached;
    }

    static const char kB64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::string b64;
    b64.reserve(((static_cast<size_t>(size) + 2) / 3) * 4);
    for (DWORD i = 0; i < size; i += 3) {
        const unsigned c0 = bytes[i];
        const unsigned c1 = (i + 1 < size) ? bytes[i + 1] : 0u;
        const unsigned c2 = (i + 2 < size) ? bytes[i + 2] : 0u;
        const unsigned triple = (c0 << 16) | (c1 << 8) | c2;
        b64 += kB64[(triple >> 18) & 0x3F];
        b64 += kB64[(triple >> 12) & 0x3F];
        b64 += (i + 1 < size) ? kB64[(triple >> 6) & 0x3F] : '=';
        b64 += (i + 2 < size) ? kB64[triple & 0x3F]        : '=';
    }

    cached = "data:image/png;base64," + b64;
    logging::Logf("[RemapWeb] controller image inlined (%lu bytes -> %zu base64)",
                  size, b64.size());
    return cached;
}

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

// The page literal is UTF-8 (the target compiles with /utf-8) and
// NavigateToString wants UTF-16. Converting properly matters: widening
// byte-by-byte would split every multi-byte sequence into separate garbage code
// units, so any non-ASCII in the markup would render as mojibake.
std::wstring Utf8ToWide(const std::string& in) {
    if (in.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, in.data(),
                                           static_cast<int>(in.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, in.data(), static_cast<int>(in.size()),
                        out.data(), needed);
    return out;
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
.bar{
  flex:0 0 auto; padding:10px 20px; display:flex; align-items:center; gap:10px;
  background:rgba(255,255,255,.03); border-bottom:1px solid rgba(255,255,255,.05);
}
.bar select{width:auto; min-width:170px}
main{flex:1 1 auto; overflow-y:auto; padding:16px 20px 8px; display:flex; gap:20px; align-items:flex-start}
.left{flex:0 0 440px; position:sticky; top:0}
.right{flex:1 1 auto; min-width:0}
.pad{position:relative; width:100%; border-radius:10px; background:#0e141b; padding:6px}
.pad img{display:block; width:100%; height:auto; opacity:.92}
.dot{
  position:absolute; transform:translate(-50%,-50%);
  width:22px; height:22px; border-radius:50%; cursor:pointer;
  background:rgba(191,227,255,.22); border:2px solid var(--accent);
  display:flex; align-items:center; justify-content:center;
  font-size:9px; font-weight:700; color:var(--accent); line-height:1;
}
.dot:hover{background:rgba(102,192,244,.45); color:#fff}
.dot.bound{background:rgba(102,192,244,.30)}
.dot.sel{
  background:linear-gradient(135deg,var(--accent-deep),#0a4a78);
  border-color:#bfe3ff; color:#fff; width:26px; height:26px;
  box-shadow:0 0 0 4px rgba(102,192,244,.22);
}
.padhint{margin-top:8px; color:var(--text-faint); font-size:12px; text-align:center}
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
<div class="bar">
  <label for="prof2">Profile</label>
  <select id="prof2"></select>
  <span class="chk"><input type="checkbox" id="auto"><label for="auto">Auto-switch when a game launches</label></span>
</div>
<main>
  <div class="left">
    <div class="pad" id="pad"><img id="padimg" alt=""></div>
    <div class="padhint">Click a marker to rebind that button.</div>
  </div>
  <div class="right">
    <div id="ed" class="editor" style="display:none">
      <label for="type">Action</label>
      <select id="type">
        <option value="MENU">Passthrough (default)</option>
        <option value="NONE">Unmapped</option>
        <option value="GAMEPAD">Gamepad button</option>
        <option value="KEY">Keyboard shortcut</option>
      </select>
      <label for="pads" id="padL">Target</label>
      <select id="pads">
        <option>A</option><option>B</option><option>X</option><option>Y</option>
        <option>LB</option><option>RB</option><option>L3</option><option>R3</option>
        <option>View</option><option>Menu</option><option>Guide</option>
        <option>DPadUp</option><option>DPadDown</option><option>DPadLeft</option><option>DPadRight</option>
      </select>
      <label for="key" id="keyL">Keys</label>
      <input type="text" id="key" readonly placeholder="Click, then press the keys">
      <div class="full chk"><input type="checkbox" id="rapid"><label for="rapid">Rapid fire while held</label></div>
      <div class="full"><button class="primary" id="apply">Apply to <span id="applyName"></span></button></div>
    </div>
    <div id="groups"></div>
  </div>
</main>
<footer>
  <span class="hint" id="hint">Select a button to rebind it.</span>
  <span class="spacer"></span>
  <button id="adv">Macros &amp; profiles...</button>
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

// Arrows for the D-Pad; truncating the names gave "Dow" and "Lef". These are
// literal UTF-8 in the C++ source, which is safe because the page is converted
// with MultiByteToWideChar rather than widened byte-by-byte (see Utf8ToWide).
const SHORT={'D-Pad Up':'↑','D-Pad Down':'↓',
             'D-Pad Left':'←','D-Pad Right':'→'};

function renderPad(){
  const host=$('pad');
  host.querySelectorAll('.dot').forEach(d=>d.remove());
  for(const b of state.buttons){
    if(b.x===undefined) continue;
    const d=document.createElement('div');
    d.className='dot '+(classify(b.desc)==='bound'?'bound ':'')+(sel===b.index?'sel':'');
    d.style.left=b.x+'%'; d.style.top=b.y+'%';
    d.title=b.name+': '+b.desc;
    d.textContent=SHORT[b.name]||b.name;
    d.onclick=()=>{sel=b.index; syncEditor(); render();};
    host.appendChild(d);
  }
}

function renderProfiles(){
  const s=$('prof2');
  if(s.dataset.sig === (state.profiles||[]).join('|')+'#'+state.profile) return;
  s.dataset.sig=(state.profiles||[]).join('|')+'#'+state.profile;
  s.innerHTML='';
  for(const p of (state.profiles||[])){
    const o=document.createElement('option');
    o.value=p; o.textContent=p;
    if(p===state.profile) o.selected=true;
    s.appendChild(o);
  }
}

function render(){
  $('prof').textContent = state.profile ? 'profile: '+state.profile : '';
  $('auto').checked = !!state.autoSwitch;
  renderProfiles();
  renderPad();
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
  $('pads').style.display=padOn?'':'none';  $('padL').style.display=padOn?'':'none';
  $('key').style.display=keyOn?'':'none';   $('keyL').style.display=keyOn?'':'none';
  $('rapid').disabled = !(padOn||keyOn);
}

// Live key capture. Typing "CTRL+SHIFT+F1" by hand was the crudest part of the
// old editor; here the field records what is actually pressed. Tokens match what
// PaddleConfig's parser accepts.
const KEYMAP={
  ' ':'SPACE', 'Enter':'ENTER', 'Tab':'TAB', 'Escape':'ESC', 'Backspace':'BACKSPACE',
  'Delete':'DELETE', 'Insert':'INSERT', 'Home':'HOME', 'End':'END',
  'PageUp':'PAGEUP', 'PageDown':'PAGEDOWN',
  'ArrowUp':'UP', 'ArrowDown':'DOWN', 'ArrowLeft':'LEFT', 'ArrowRight':'RIGHT'
};
function tokenFor(e){
  const k=e.key;
  if(KEYMAP[k]) return KEYMAP[k];
  if(/^F\d{1,2}$/.test(k)) return k.toUpperCase();
  if(k.length===1){
    const u=k.toUpperCase();
    if((u>='A'&&u<='Z')||(u>='0'&&u<='9')) return u;
  }
  return null;
}
$('key').onkeydown=e=>{
  e.preventDefault();
  const mods=[];
  if(e.ctrlKey)  mods.push('CTRL');
  if(e.shiftKey) mods.push('SHIFT');
  if(e.altKey)   mods.push('ALT');
  if(e.metaKey)  mods.push('WIN');
  const t=tokenFor(e);
  // Modifier-only presses show progress but are not a complete chord.
  $('key').value = t ? mods.concat([t]).join('+') : mods.join('+');
  $('hint').textContent = t ? 'Captured '+$('key').value+'.' : 'Now press the main key.';
};
$('key').onfocus=()=>{ $('hint').textContent='Press the keys you want bound.'; };

$('type').onchange=typeChanged;
$('close').onclick=()=>send({cmd:'close'});
$('adv').onclick=()=>send({cmd:'advanced'});
$('prof2').onchange=()=>send({cmd:'profile', profile:$('prof2').value});
$('auto').onchange=()=>send({cmd:'autoswitch', on:$('auto').checked?1:0});
$('apply').onclick=()=>{
  const b=cur(); if(!b) return;
  const t=$('type').value;
  let a=t;
  if(t==='GAMEPAD') a='GAMEPAD:'+$('pads').value;
  else if(t==='KEY'){
    const k=$('key').value.trim();
    if(!k || /\+$/.test(k)){ $('hint').textContent='Click the Keys box and press a shortcut first.'; return; }
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

bool RemapWebWindow::Open(HINSTANCE hInst, const std::wstring& profileId, Callbacks cb) {
    m_profileId = profileId;
    m_cb        = std::move(cb);

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

                            // Inline the controller artwork before navigating.
                            // Substituted here rather than pushed as state so the
                            // ~240 KB data URI crosses the bridge once, not on
                            // every refresh.
                            std::string page = kHtml;
                            const std::string uri = ControllerImageDataUri();
                            const std::string token = "id=\"padimg\" alt=\"\"";
                            const size_t at = page.find(token);
                            if (at != std::string::npos && !uri.empty())
                                page.insert(at + token.size(), " src=\"" + uri + "\"");

                            const std::wstring html = Utf8ToWide(page);
                            m_webview->NavigateToString(html.c_str());
                            return S_OK;
                        }).Get());
            }).Get());

    if (FAILED(hr))
        logging::Logf("[RemapWeb] CreateCoreWebView2EnvironmentWithOptions failed hr=0x%08lX",
                      static_cast<unsigned long>(hr));
}

void RemapWebWindow::PostState() {
    if (!m_webview || !m_cb.query) return;

    const PaddleActionBindings bindings = m_cb.query();
    PaddleMappings mappings{};  // menu fallbacks are not edited here

    std::wstring json = L"{\"profile\":\"" + JsonEscape(m_profileId) + L"\"";

    json += L",\"autoSwitch\":";
    json += (m_cb.autoSwitchGet && m_cb.autoSwitchGet()) ? L"true" : L"false";

    json += L",\"profiles\":[";
    if (m_cb.profiles) {
        bool firstProfile = true;
        for (const std::wstring& id : m_cb.profiles()) {
            if (!firstProfile) json += L',';
            firstProfile = false;
            json += L'"' + JsonEscape(id) + L'"';
        }
    }
    json += L"]";

    json += L",\"buttons\":[";
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
                L",\"x\":" + std::to_wstring(meta.xPct) +
                L",\"y\":" + std::to_wstring(meta.yPct) +
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
        if (m_cb.openAdvanced) m_cb.openAdvanced();
        return;
    }
    if (cmd == L"set") {
        int index = -1;
        std::wstring action;
        if (!JsonIntField(json, L"index", index) ||
            !JsonStringField(json, L"action", action))
            return;
        if (m_cb.apply) m_cb.apply(index, action);
        PostState();  // echo back what actually took effect
        return;
    }
    if (cmd == L"profile") {
        std::wstring id;
        if (!JsonStringField(json, L"profile", id)) return;
        if (m_cb.switchProfile) {
            m_cb.switchProfile(id);
            m_profileId = id;
        }
        PostState();
        return;
    }
    if (cmd == L"autoswitch") {
        int on = 0;
        if (!JsonIntField(json, L"on", on)) return;
        if (m_cb.autoSwitchSet) m_cb.autoSwitchSet(on != 0);
        PostState();
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
