# restyle_web.py - swap the CSS / brand header / favicon of new_index.html
# without touching the body markup ids or the <script> logic. Re-run after
# editing the blocks below, then run replace_html.py to inject into firmware.
import re

root = r"D:\Project\Embedded\ESP32\ai-wireless-debugger-main"
html_path = root + r"\new_index.html"

# ---- favicon: cyan rounded tile + white wireless mark ----
favicon = (
    '<link rel="icon" href="data:image/svg+xml,'
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'>"
    "<rect width='24' height='24' rx='5' fill='%2322d3ee'/>"
    "<g fill='none' stroke='%2305303a' stroke-width='2' stroke-linecap='round'>"
    "<path d='M4.5 12a10.5 10.5 0 0 1 15 0'/>"
    "<path d='M7.8 15.2a6 6 0 0 1 8.4 0'/>"
    "<circle cx='12' cy='18.6' r='1.5' fill='%2305303a' stroke='none'/>"
    "</g></svg>\">"
)

# ---- hero brand header (keeps #topBadge) ----
hero = '''<div class="hero">
  <div class="hero-glow"></div>
  <div class="hero-row">
    <div class="logo">
      <svg viewBox="0 0 24 24" fill="none" stroke="#05303a" stroke-width="2.1" stroke-linecap="round" stroke-linejoin="round">
        <path d="M4.5 12a10.5 10.5 0 0 1 15 0"/>
        <path d="M7.8 15.2a6 6 0 0 1 8.4 0"/>
        <circle cx="12" cy="18.6" r="1.5" fill="#05303a" stroke="none"/>
      </svg>
    </div>
    <div class="hero-txt">
      <div class="brand-name">NexLink</div>
      <div class="brand-sub">无线硬件调试器</div>
    </div>
    <span class="badge err" id="topBadge">连接中…</span>
  </div>
</div>'''

# ---- full stylesheet ----
css = r'''*{box-sizing:border-box;margin:0;padding:0;-webkit-tap-highlight-color:transparent}
:root{
  --bg:#080c13;--card:#121b27;--card2:#0e1621;--inset:#0a1019;--border:#202e42;
  --fg:#e9eef6;--muted:#8b99ad;--faint:#5c697d;
  --accent:#2dd4ee;--accent2:#38bdf8;--ok:#34d399;--warn:#fbbf24;--err:#fb7185;
  --rx:#34d399;--tx:#fbbf24;
}
html,body{height:100%}
body{
  font-family:-apple-system,BlinkMacSystemFont,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif;
  background:var(--bg);color:var(--fg);font-size:14px;line-height:1.5;
  padding:14px 12px calc(72px + env(safe-area-inset-bottom));
  background-image:radial-gradient(900px 380px at 50% -120px,rgba(34,211,238,.14),transparent 70%);
  background-repeat:no-repeat;
}
.mono{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}
.wrap{max-width:860px;margin:0 auto}
::-webkit-scrollbar{width:8px;height:8px}
::-webkit-scrollbar-thumb{background:#243448;border-radius:8px}
::-webkit-scrollbar-track{background:transparent}

/* ---------- brand hero ---------- */
.hero{
  position:relative;overflow:hidden;border-radius:18px;margin-bottom:14px;
  padding:16px 18px;border:1px solid #1c3a47;
  background:linear-gradient(135deg,#0d2a34 0%,#101c29 52%,#0d151f 100%);
  box-shadow:0 12px 32px rgba(0,0,0,.4),inset 0 1px 0 rgba(255,255,255,.05);
}
.hero-glow{position:absolute;top:-60px;right:-30px;width:200px;height:200px;border-radius:50%;
  background:radial-gradient(circle,rgba(45,212,238,.35),transparent 65%);pointer-events:none}
.hero-row{position:relative;z-index:1;display:flex;align-items:center;gap:13px}
.logo{
  width:44px;height:44px;flex:0 0 auto;border-radius:13px;display:flex;align-items:center;justify-content:center;
  background:linear-gradient(135deg,#46e6f5,#0ea5e9);
  box-shadow:0 8px 18px rgba(34,211,238,.4),inset 0 1px 0 rgba(255,255,255,.45);
}
.logo svg{width:27px;height:27px}
.hero-txt{min-width:0}
.brand-name{
  font-size:23px;font-weight:800;letter-spacing:2px;line-height:1.05;
  background:linear-gradient(90deg,#cffafe,#67e8f9 45%,#38bdf8);
  -webkit-background-clip:text;background-clip:text;color:transparent;
}
.brand-sub{font-size:11.5px;color:#8aa0b6;margin-top:5px;letter-spacing:2px}
.badge{
  margin-left:auto;flex:0 0 auto;
  display:inline-flex;align-items:center;gap:6px;
  font-size:12px;font-weight:600;padding:5px 11px;border-radius:999px;
  background:#1a2533;color:var(--muted);border:1px solid var(--border);
}
.badge::before{content:"";width:7px;height:7px;border-radius:50%;background:currentColor}
.badge.ok{color:var(--ok);background:rgba(52,211,153,.12);border-color:rgba(52,211,153,.35)}
.badge.warn{color:var(--warn);background:rgba(251,191,36,.12);border-color:rgba(251,191,36,.35)}
.badge.err{color:var(--err);background:rgba(251,113,133,.12);border-color:rgba(251,113,133,.35)}

/* ---------- cards ---------- */
.card{
  background:linear-gradient(180deg,#131d29,#0f1823);
  border:1px solid var(--border);border-radius:15px;
  padding:15px 17px;margin-bottom:12px;
  box-shadow:0 8px 22px rgba(0,0,0,.28),inset 0 1px 0 rgba(255,255,255,.03);
}
.card h2{display:flex;align-items:center;gap:9px;font-size:13.5px;font-weight:700;margin-bottom:3px}
.card h2::before{content:"";width:3px;height:15px;border-radius:2px;
  background:linear-gradient(var(--accent),var(--accent2));box-shadow:0 0 8px rgba(45,212,238,.6)}
.card .sub{font-size:11.5px;color:var(--faint);margin-bottom:12px}
.kv{display:flex;justify-content:space-between;align-items:center;gap:12px;padding:8px 0;border-bottom:1px dashed rgba(32,46,66,.8);font-size:13px}
.kv:last-child{border-bottom:none}
.kv .k{color:var(--muted);flex-shrink:0}
.kv .v{text-align:right;word-break:break-all}
.kv .v.good{color:var(--ok)} .kv .v.bad{color:var(--err)} .kv .v.hot{color:var(--warn)}

/* ---------- controls ---------- */
input,select{
  background:var(--inset);color:var(--fg);border:1px solid var(--border);
  border-radius:10px;padding:10px 11px;font-size:13px;font-family:inherit;width:100%;
  transition:border-color .15s,box-shadow .15s;
}
input:focus,select:focus{outline:none;border-color:var(--accent);box-shadow:0 0 0 3px rgba(45,212,238,.15)}
input[type=file]{font-size:12px;color:var(--muted);padding:8px}
select{appearance:none;background-image:url("data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg' width='10' height='6'><path d='M1 1l4 4 4-4' stroke='%238b99ad' fill='none' stroke-width='1.5'/></svg>");background-repeat:no-repeat;background-position:right 11px center;padding-right:28px}
.btn{
  display:inline-flex;align-items:center;justify-content:center;gap:6px;
  border:1px solid var(--border);background:linear-gradient(180deg,#16222f,#101925);color:var(--fg);
  border-radius:10px;padding:9px 16px;font-size:13px;font-weight:600;
  font-family:inherit;cursor:pointer;flex-shrink:0;
  box-shadow:0 3px 10px rgba(0,0,0,.25),inset 0 1px 0 rgba(255,255,255,.05);
  transition:transform .1s,box-shadow .15s,filter .15s;
}
.btn:hover{filter:brightness(1.12)}
.btn:active{transform:scale(.97)}
.btn.primary{background:linear-gradient(180deg,#39e0f3,#14b8d8);border-color:#2dd4ee;color:#042730;font-weight:700;
  box-shadow:0 6px 16px rgba(34,211,238,.32),inset 0 1px 0 rgba(255,255,255,.4)}
.btn.ok{background:linear-gradient(180deg,#41dca5,#1fae82);border-color:#34d399;color:#05302a;font-weight:700;
  box-shadow:0 6px 16px rgba(52,211,153,.28),inset 0 1px 0 rgba(255,255,255,.4)}
.btn.danger{background:transparent;border-color:rgba(251,113,133,.5);color:var(--err);box-shadow:none}
.btn.block{width:100%;margin-top:8px}
.btn:disabled{opacity:.5;cursor:not-allowed}
.field{margin-bottom:10px}
.field label{display:block;font-size:12px;color:var(--muted);margin-bottom:5px}
.row{display:flex;gap:8px;align-items:center}
.row>*{flex:1}
.row .keep{flex:0 0 auto}
.switch{display:inline-flex;align-items:center;gap:6px;font-size:12px;color:var(--muted);cursor:pointer;user-select:none;flex-shrink:0}
.switch input{width:auto;accent-color:var(--accent)}
.hint{font-size:11.5px;color:var(--faint);margin-top:8px;line-height:1.6}

/* segmented control */
.seg{display:flex;gap:6px;background:var(--inset);border:1px solid var(--border);border-radius:12px;padding:5px}
.seg button{
  flex:1;border:none;background:transparent;color:var(--muted);
  border-radius:9px;padding:9px 4px;font-size:12.5px;font-weight:600;
  font-family:inherit;cursor:pointer;line-height:1.3;transition:background .15s,color .15s;
}
.seg button small{display:block;font-size:10px;font-weight:400;opacity:.8;margin-top:2px}
.seg button.on{background:linear-gradient(180deg,#39e0f3,#14b8d8);color:#042730;box-shadow:0 4px 12px rgba(34,211,238,.3)}
.seg button.on small{opacity:.85}

/* sub tabs */
.subtabs{display:flex;gap:6px;margin-bottom:12px;background:var(--inset);border:1px solid var(--border);border-radius:12px;padding:5px}
.subtabs button{flex:1;border:none;background:transparent;color:var(--muted);border-radius:9px;padding:9px;font-size:13px;font-weight:600;font-family:inherit;cursor:pointer;transition:background .15s,color .15s}
.subtabs button.on{background:#153040;color:var(--accent);box-shadow:inset 0 0 0 1px rgba(45,212,238,.25)}

/* ---------- serial terminal ---------- */
.term-toolbar{display:flex;gap:8px;align-items:center;margin-bottom:8px;flex-wrap:wrap}
.term-toolbar select{width:auto;padding:7px 26px 7px 9px;font-size:12px}
#term{
  background:#060a10;border:1px solid var(--border);border-radius:11px;
  padding:11px 13px;height:50vh;max-height:560px;overflow-y:auto;
  font-size:12.5px;line-height:1.65;white-space:pre-wrap;word-break:break-all;
  box-shadow:inset 0 2px 10px rgba(0,0,0,.45);
}
#term .rx{color:var(--rx)}
#term .tx{color:var(--tx)}
#term .sy{color:var(--faint)}
#term .ts{color:#46536a}
.send-row{display:flex;gap:8px;margin-top:8px}
.send-row select{width:84px;flex:0 0 auto;padding:9px 24px 9px 8px}
.send-row input{flex:1;min-width:0}

/* ---------- OLED ---------- */
.lcd-wrap{background:#000;border-radius:11px;padding:12px;display:flex;justify-content:center;margin-bottom:10px;
  box-shadow:inset 0 0 0 1px #101826,0 6px 16px rgba(0,0,0,.35)}
.lcd-wrap canvas{width:100%;max-width:320px;height:auto;image-rendering:pixelated}
.lcd-btns{display:flex;gap:8px}
.lcd-btns button{flex:1;padding:12px 0;font-weight:700}

/* ---------- waves / logs ---------- */
canvas.wave{width:100%;height:90px;background:#060a10;border:1px solid var(--border);border-radius:10px;margin:8px 0;box-shadow:inset 0 2px 8px rgba(0,0,0,.4)}
.txn-log{background:#060a10;border:1px solid var(--border);border-radius:10px;padding:8px 11px;max-height:170px;overflow-y:auto;font-size:11.5px;margin:8px 0;box-shadow:inset 0 2px 8px rgba(0,0,0,.4)}
.txn-log div{white-space:pre-wrap;word-break:break-all;line-height:1.7}
.txn-log .mosi{color:var(--ok)} .txn-log .miso{color:var(--warn)} .txn-log .i2cw{color:var(--ok)} .txn-log .i2cr{color:var(--accent)}
.result{font-size:12px;margin-top:8px;color:var(--muted);word-break:break-all}
.result.err{color:var(--err)} .result.ok{color:var(--ok)}

/* ---------- wifi list / OTA ---------- */
.wifi-list{display:flex;flex-direction:column;gap:6px;margin:8px 0}
.wifi-list button{text-align:left;padding:9px 12px;font-size:12.5px;display:flex;justify-content:space-between;width:100%}
.wifi-list button .sig{color:var(--muted);font-size:11px;flex-shrink:0;margin-left:8px}
.progress{height:8px;background:var(--inset);border-radius:99px;overflow:hidden;margin-top:10px;border:1px solid var(--border)}
.progress i{display:block;height:100%;width:0;background:linear-gradient(90deg,var(--accent),var(--accent2));box-shadow:0 0 10px rgba(45,212,238,.6);transition:width .2s}

/* ---------- pages / bottom nav ---------- */
.page{display:none;animation:fade .18s ease}
.page.on{display:block}
@keyframes fade{from{opacity:0;transform:translateY(5px)}to{opacity:1;transform:none}}
.tabbar{
  position:fixed;left:0;right:0;bottom:0;z-index:50;
  display:flex;background:rgba(12,17,25,.92);backdrop-filter:blur(14px);-webkit-backdrop-filter:blur(14px);
  border-top:1px solid var(--border);padding-bottom:env(safe-area-inset-bottom);
}
.tabbar button{
  position:relative;flex:1;border:none;background:transparent;color:var(--faint);
  font-family:inherit;font-size:11.5px;font-weight:600;
  padding:9px 0 10px;cursor:pointer;display:flex;flex-direction:column;align-items:center;gap:3px;
  transition:color .15s;
}
.tabbar button svg{width:21px;height:21px;fill:none;stroke:currentColor;stroke-width:1.8;stroke-linecap:round;stroke-linejoin:round;transition:transform .15s}
.tabbar button.on{color:var(--accent)}
.tabbar button.on svg{transform:translateY(-1px) scale(1.08)}
.tabbar button.on::before{content:"";position:absolute;top:3px;width:46px;height:30px;border-radius:999px;
  background:rgba(45,212,238,.14);border:1px solid rgba(45,212,238,.22);z-index:-1}

/* ---------- toast ---------- */
#toasts{position:fixed;top:14px;left:50%;transform:translateX(-50%);z-index:100;display:flex;flex-direction:column;gap:8px;width:min(90vw,420px)}
.toast{
  background:rgba(24,36,52,.96);backdrop-filter:blur(8px);border:1px solid var(--border);color:var(--fg);
  border-radius:12px;padding:10px 16px;font-size:13px;text-align:center;
  box-shadow:0 10px 28px rgba(0,0,0,.5);animation:fade .15s ease;
}
.toast.ok{border-color:rgba(52,211,153,.55)} .toast.err{border-color:rgba(251,113,133,.55)}'''

src = open(html_path, "r", encoding="utf-8").read()

# 1) favicon after <title>
src, n1 = re.subn(r"(<title>[^<]*</title>)", r"\1\n" + favicon, src, count=1)
assert n1 == 1, "title anchor not found"

# 2) replace stylesheet
src, n2 = re.subn(r"<style>[\s\S]*?</style>", "<style>\n" + css + "\n</style>", src, count=1)
assert n2 == 1, "style block not found"

# 3) replace old topbar with hero
old_top = re.compile(r'<div class="topbar">[\s\S]*?</div>\s*<!--', re.MULTILINE)
m = old_top.search(src)
assert m, "topbar block not found"
src = src[:m.start()] + hero + "\n\n  <!--" + src[m.end():]

with open(html_path, "w", encoding="utf-8", newline="\n") as f:
    f.write(src)
print("restyled OK:", len(src.encode("utf-8")), "bytes")
