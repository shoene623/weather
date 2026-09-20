#pragma once

// Self-contained dashboard: static HTML/CSS/JS served as-is, all live data
// pulled client-side from the JSON endpoints in main.cpp (see /api/*).
// If those endpoints are unreachable (e.g. this file opened off-device) the
// page falls back to bundled demo data so the layout is never empty.
static const char PAGE_HTML[] PROGMEM = R"HTMLPAGE(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Station Dial</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Outfit:wght@500;600;700&family=Plus+Jakarta+Sans:wght@400;500;600;700;800&display=swap" rel="stylesheet">
<style>
  :root{
    --bg:#dff0fd;
    --surface:#ffffff;
    --surface-2:#eaf6ff;
    --border:rgba(21,74,120,.12);
    --border-strong:rgba(21,74,120,.20);
    --text:#0f2438;
    --text-dim:#4a6b85;
    --text-faint:#7d9cb2;
    --shadow: 0 1px 2px rgba(21,74,120,.05), 0 10px 26px -14px rgba(21,74,120,.28);
    --accent-h: 201;
    --accent-s: 82%;
    --accent-l: 47%;
    --accent: hsl(var(--accent-h) var(--accent-s) var(--accent-l));
    --accent-soft: hsl(var(--accent-h) 82% 47% / .12);
    --accent-ring: hsl(var(--accent-h) 82% 47% / .35);
    --good:#1f9d63;
    --warn:#c07a12;
    --sky-a: hsl(var(--accent-h) 88% 90%);
    --sky-b: hsl(calc(var(--accent-h) - 10) 75% 68%);
    --font-display:'Outfit', ui-sans-serif, system-ui, -apple-system, "Segoe UI", sans-serif;
    --font-body:'Plus Jakarta Sans', ui-sans-serif, system-ui, -apple-system, "Segoe UI", sans-serif;
    --font-mono:'Plus Jakarta Sans', ui-sans-serif, system-ui, -apple-system, "Segoe UI", sans-serif;
  }

  /* condition hue map — mirrors the categories drawn on the device's own round display */
  body[data-cond="clear"]{ --accent-h:40; }
  body[data-cond="pcloudy"]{ --accent-h:38; }
  body[data-cond="cloudy"]{ --accent-h:208; }
  body[data-cond="rain"]{ --accent-h:199; }
  body[data-cond="storm"]{ --accent-h:258; }
  body[data-cond="snow"]{ --accent-h:194; }
  body[data-cond="fog"]{ --accent-h:214; }
  body[data-cond="night"]{ --accent-h:232; }

  *{ box-sizing:border-box; }
  html{ color-scheme: light; }
  body{
    margin:0;
    background:
      radial-gradient(120% 70% at 15% -10%, hsl(var(--accent-h) 90% 88% / .8), transparent 60%),
      linear-gradient(180deg, #eaf7ff 0%, var(--bg) 55%, #eef8ff 100%);
    background-attachment:fixed;
    color:var(--text);
    font-family:var(--font-body);
    -webkit-font-smoothing:antialiased;
    padding-inline:20px;
    padding-block:20px 48px;
    transition: background 1s ease;
  }
  .wrap{ max-width:920px; margin-inline:auto; }
  a{ color:inherit; }
  button{ font-family:inherit; }

  ::selection{ background:var(--accent-soft); }

  /* ---------- header ---------- */
  header.top{
    display:flex; align-items:center; justify-content:space-between;
    gap:16px; flex-wrap:wrap;
    padding-block:4px 20px;
  }
  .brand{ display:flex; align-items:baseline; gap:10px; }
  .brand .mark{
    font-family:var(--font-display); font-weight:600; font-size:1.5rem;
    letter-spacing:.01em;
  }
  .brand .place{
    font-family:var(--font-mono); font-size:.78rem; color:var(--text-faint);
    letter-spacing:.02em;
  }
  .status-pill{
    display:inline-flex; align-items:center; gap:7px;
    font-family:var(--font-mono); font-size:.74rem; color:var(--text-dim);
    border:1px solid var(--border); background:var(--surface);
    padding:6px 12px 6px 10px; border-radius:999px; box-shadow:var(--shadow);
  }
  .status-pill .dot{
    width:7px; height:7px; border-radius:50%; background:var(--good);
    box-shadow:0 0 0 3px color-mix(in srgb, var(--good) 25%, transparent);
  }
  .status-pill.offline .dot{ background:#d64545; box-shadow:0 0 0 3px rgba(214,69,69,.25); }

  nav.tabs{
    display:flex; gap:4px; padding:4px;
    background:var(--surface-2); border:1px solid var(--border);
    border-radius:12px; width:fit-content; margin-bottom:22px;
  }
  nav.tabs button{
    appearance:none; border:none; background:transparent; color:var(--text-dim);
    font-size:.86rem; font-weight:600; padding:8px 16px; border-radius:9px;
    cursor:pointer; transition:background .15s ease, color .15s ease;
  }
  nav.tabs button.active{ background:var(--surface); color:var(--text); box-shadow:var(--shadow); }
  nav.tabs button:hover:not(.active){ color:var(--text); }

  section[hidden]{ display:none !important; }

  /* ---------- hero ---------- */
  .hero{
    position:relative; overflow:hidden;
    border-radius:28px; border:1px solid var(--border-strong);
    background: linear-gradient(160deg, var(--sky-a), var(--sky-b));
    box-shadow:var(--shadow);
    padding:32px clamp(18px,4vw,40px);
    display:grid; grid-template-columns: auto 1fr; gap:clamp(20px,4vw,44px);
    align-items:center;
    transition: background 1s ease;
  }
  .hero::before{
    content:""; position:absolute; inset:0; pointer-events:none;
    background: radial-gradient(120% 90% at 85% -10%, color-mix(in srgb, var(--accent) 20%, transparent), transparent 60%);
  }
  .dial{ position:relative; width:min(220px,40vw); height:min(220px,40vw); flex:none; display:grid; place-items:center; }
  .dial svg{ position:absolute; inset:0; width:100%; height:100%; transform:rotate(-90deg); }
  .dial .face{
    position:relative; width:78%; height:78%; border-radius:50%;
    background: color-mix(in srgb, var(--surface) 88%, transparent);
    backdrop-filter: blur(6px);
    border:1px solid color-mix(in srgb, var(--text) 16%, transparent);
    display:flex; flex-direction:column; align-items:center; justify-content:center;
    text-align:center;
  }
  .dial svg{ color:var(--text); }
  .dial svg .track{ stroke:currentColor; opacity:.22; }
  .dial svg .ring{ stroke:var(--accent); }
  .dial .temp{
    font-family:var(--font-display); font-weight:500; font-size:clamp(2.6rem,7vw,3.4rem);
    line-height:1; color:var(--text); font-variant-numeric:tabular-nums;
  }
  .dial .temp sup{ font-size:.42em; font-weight:500; top:-.55em; }
  .dial .feels{ font-family:var(--font-mono); font-size:.72rem; color:var(--text-dim); margin-top:4px; }

  .hero-info{ min-width:0; color:var(--text); }

  .hero-info .cond-row{ display:flex; align-items:center; gap:10px; margin-bottom:6px; }
  .hero-info .cond-icon{ width:30px; height:30px; flex:none; }
  .hero-info .cond{ font-family:var(--font-display); font-size:1.3rem; font-weight:500; text-wrap:balance; }
  .hero-info .sub{ font-family:var(--font-mono); font-size:.82rem; opacity:.82; display:flex; gap:14px; flex-wrap:wrap; margin-top:10px; }
  .hero-info .sub b{ font-weight:600; }
  .hero-info .updated{
    margin-top:18px; display:flex; align-items:center; gap:10px;
    font-size:.74rem; opacity:.72; font-family:var(--font-mono);
  }
  .refresh-btn{
    appearance:none; border:1px solid color-mix(in srgb, var(--text) 30%, transparent);
    background:color-mix(in srgb, var(--text) 10%, transparent);
    color:inherit; width:26px; height:26px; border-radius:50%; cursor:pointer;
    display:grid; place-items:center; transition:transform .5s ease, background .15s ease;
  }
  .refresh-btn:hover{ background:color-mix(in srgb, var(--text) 18%, transparent); }
  .refresh-btn.spin svg{ animation: spin .7s linear; }
  .refresh-btn svg{ width:13px; height:13px; }
  @keyframes spin{ to{ transform:rotate(360deg); } }

  /* ---------- stat grid ---------- */
  .grid{
    display:grid; grid-template-columns:repeat(auto-fill,minmax(150px,1fr));
    gap:12px; margin-top:16px;
  }
  .stat{
    background:var(--surface); border:1px solid var(--border); border-radius:16px;
    padding:16px; box-shadow:var(--shadow); position:relative;
    display:flex; flex-direction:column; gap:10px;
  }
  .stat .head{ display:flex; align-items:center; gap:8px; padding-right:52px; }
  .stat .label{ font-size:.72rem; font-weight:700; letter-spacing:.05em; text-transform:uppercase; color:var(--text-faint); }
  .stat .icon{ width:18px; height:18px; color:var(--accent); flex:none; }
  .stat .value{ font-family:var(--font-mono); font-size:1.5rem; font-weight:600; font-variant-numeric:tabular-nums; }
  .stat .value .unit{ font-size:.6em; color:var(--text-dim); font-weight:500; margin-left:2px; }
  .stat .chip{
    position:absolute; top:12px; right:12px; font-family:var(--font-mono); font-size:.6rem;
    letter-spacing:.03em; color:var(--accent); background:var(--accent-soft);
    padding:3px 7px; border-radius:999px; font-weight:600;
  }
  .stat.on-device{ border-color:var(--accent-ring); }

  /* ---------- cards / settings ---------- */
  .cards{ display:grid; gap:16px; grid-template-columns: repeat(auto-fit,minmax(280px,1fr)); align-items:start; }
  .card{
    background:var(--surface); border:1px solid var(--border); border-radius:18px;
    padding:20px 22px; box-shadow:var(--shadow);
  }
  .card h3{
    margin:0 0 4px; font-family:var(--font-body); font-size:.78rem; font-weight:800;
    text-transform:uppercase; letter-spacing:.07em; color:var(--text-faint);
  }
  .card p.hint{ margin:0 0 16px; font-size:.83rem; color:var(--text-dim); }
  .field{ margin-bottom:14px; }
  .field:last-child{ margin-bottom:0; }
  .field label{ display:block; font-size:.78rem; color:var(--text-dim); margin-bottom:6px; font-weight:600; }
  .field input[type=text], .field input[type=number], .field input[type=time]{
    width:100%; padding:10px 12px; font-size:.95rem; font-family:var(--font-mono);
    border:1px solid var(--border-strong); border-radius:10px; background:var(--surface-2); color:var(--text);
  }
  .field input:focus, .seg button:focus-visible, .switch input:focus-visible + .track, nav.tabs button:focus-visible, .icon-btn:focus-visible{
    outline:2px solid var(--accent); outline-offset:2px;
  }
  .row{ display:flex; gap:10px; align-items:end; flex-wrap:wrap; }
  .row .field{ flex:1; min-width:140px; margin-bottom:0; }

  .btn{
    appearance:none; border:none; cursor:pointer; font-weight:700; font-size:.85rem;
    padding:10px 18px; border-radius:10px; background:var(--text); color:var(--bg);
    transition:transform .1s ease, opacity .15s ease;
  }
  .btn:hover{ opacity:.88; }
  .btn:active{ transform:scale(.97); }
  .btn.ghost{ background:transparent; border:1px solid var(--border-strong); color:var(--text); }
  .btn.small{ padding:7px 12px; font-size:.78rem; }
  .btn.danger{ background:transparent; color:#d64545; }

  .seg{ display:inline-flex; border:1px solid var(--border-strong); border-radius:10px; overflow:hidden; }
  .seg button{
    appearance:none; border:none; background:var(--surface-2); color:var(--text-dim);
    padding:9px 18px; font-size:.85rem; font-weight:700; cursor:pointer; font-family:var(--font-mono);
  }
  .seg button.active{ background:var(--accent); color:#fff; }

  .slider-row{ display:flex; align-items:center; gap:14px; }
  input[type=range]{
    flex:1; -webkit-appearance:none; appearance:none; height:4px; border-radius:99px;
    background:var(--border-strong); outline-offset:4px;
  }
  input[type=range]::-webkit-slider-thumb{
    -webkit-appearance:none; width:18px; height:18px; border-radius:50%;
    background:var(--accent); border:3px solid var(--surface); box-shadow:0 1px 4px rgba(0,0,0,.3); cursor:pointer;
  }
  input[type=range]::-moz-range-thumb{
    width:18px; height:18px; border-radius:50%; background:var(--accent); border:3px solid var(--surface); cursor:pointer;
  }
  .slider-row .val{ font-family:var(--font-mono); font-size:.85rem; width:44px; text-align:right; color:var(--text-dim); }

  .toggle-list{ display:flex; flex-direction:column; gap:2px; }
  .toggle-row{
    display:flex; align-items:center; justify-content:space-between; gap:12px;
    padding:11px 2px; border-bottom:1px solid var(--border);
  }
  .toggle-row:last-child{ border-bottom:none; }
  .toggle-row .ti{ display:flex; align-items:center; gap:10px; }
  .toggle-row .icon{ width:16px; height:16px; color:var(--text-dim); flex:none; }
  .toggle-row .name{ font-size:.88rem; font-weight:600; }
  .toggle-row .desc{ font-size:.74rem; color:var(--text-faint); }

  .switch{ position:relative; width:38px; height:22px; flex:none; }
  .switch input{ position:absolute; inset:0; opacity:0; margin:0; cursor:pointer; }
  .switch .track{
    position:absolute; inset:0; background:var(--border-strong); border-radius:99px; transition:background .15s ease;
  }
  .switch .track::after{
    content:""; position:absolute; top:2px; left:2px; width:18px; height:18px; border-radius:50%;
    background:var(--surface); box-shadow:0 1px 3px rgba(0,0,0,.3); transition:transform .15s ease;
  }
  .switch input:checked + .track{ background:var(--accent); }
  .switch input:checked + .track::after{ transform:translateX(16px); }
  .switch input:disabled + .track{ opacity:.4; cursor:not-allowed; }

  .counter{ font-family:var(--font-mono); font-size:.78rem; color:var(--text-dim); }
  .counter b{ color:var(--text); }

  /* ---------- notes ---------- */
  .note-form{ display:flex; gap:10px; margin-bottom:18px; }
  .note-form input{
    flex:1; padding:11px 14px; border-radius:10px; border:1px solid var(--border-strong);
    background:var(--surface-2); color:var(--text); font-size:.9rem; font-family:var(--font-body);
  }
  .note-list{ display:flex; flex-direction:column; gap:8px; }
  .note{
    display:flex; align-items:center; gap:10px; padding:12px 14px;
    background:var(--surface); border:1px solid var(--border); border-radius:12px; box-shadow:var(--shadow);
  }
  .note .txt{ flex:1; font-size:.9rem; }
  .note .idx{ font-family:var(--font-mono); font-size:.7rem; color:var(--text-faint); width:20px; }
  .icon-btn{
    appearance:none; border:1px solid var(--border-strong); background:var(--surface-2); color:var(--text-dim);
    width:30px; height:30px; border-radius:9px; cursor:pointer; display:grid; place-items:center; flex:none;
  }
  .icon-btn:hover{ color:var(--text); }
  .icon-btn svg{ width:14px; height:14px; }
  .empty{ color:var(--text-faint); font-size:.86rem; padding:8px 2px; }

  /* ---------- toast ---------- */
  .toast{
    position:fixed; left:50%; bottom:24px; transform:translate(-50%,20px);
    background:var(--text); color:var(--bg); font-size:.82rem; font-weight:600;
    padding:10px 18px; border-radius:99px; box-shadow:0 8px 24px rgba(0,0,0,.3);
    opacity:0; pointer-events:none; transition:opacity .2s ease, transform .2s ease; z-index:50;
  }
  .toast.show{ opacity:1; transform:translate(-50%,0); }

  .demo-banner{
    display:none; align-items:center; gap:8px; font-family:var(--font-mono); font-size:.72rem;
    color:var(--warn); background:color-mix(in srgb, var(--warn) 12%, transparent);
    border:1px solid color-mix(in srgb, var(--warn) 35%, transparent);
    padding:8px 14px; border-radius:10px; margin-bottom:18px; width:fit-content;
  }
  body.demo .demo-banner{ display:flex; }

  footer{ margin-top:36px; text-align:center; color:var(--text-faint); font-size:.76rem; font-family:var(--font-mono); }
  footer a{ color:var(--text-dim); text-decoration:none; border-bottom:1px dotted var(--text-faint); }

  @media (max-width:640px){
    .hero{ grid-template-columns:1fr; text-align:center; justify-items:center; }
    .hero-info .cond-row{ justify-content:center; }
    .hero-info .sub{ justify-content:center; }
    .hero-info .updated{ justify-content:center; }
    .row{ flex-direction:column; align-items:stretch; }
    .row .field{ min-width:0; }
  }

  @media (prefers-reduced-motion: reduce){
    *{ animation-duration:0.001ms !important; transition-duration:0.001ms !important; }
  }
</style>
</head>
<body class="demo">

<div class="wrap">

  <header class="top">
    <div class="brand">
      <span class="mark">Station Dial</span>
      <span class="place" id="hdr-place">32082 · PONTE VEDRA BEACH</span>
    </div>
    <div class="status-pill" id="status-pill"><span class="dot"></span><span id="status-text">Online</span></div>
  </header>

  <div class="demo-banner">◆ Preview data — connect on your network to see the live station</div>

  <nav class="tabs" role="tablist">
    <button class="active" data-tab="overview" role="tab" aria-selected="true">Overview</button>
    <button data-tab="settings" role="tab" aria-selected="false">Settings</button>
    <button data-tab="notes" role="tab" aria-selected="false">Notes</button>
    <button data-tab="power" role="tab" aria-selected="false">Power</button>
  </nav>

  <!-- ===================== OVERVIEW ===================== -->
  <section id="tab-overview">

    <div class="hero">
      <div class="dial">
        <svg viewBox="0 0 100 100">
          <circle class="track" cx="50" cy="50" r="46" fill="none" stroke-width="3"/>
          <circle id="dial-ring" class="ring" cx="50" cy="50" r="46" fill="none" stroke-width="3"
                  stroke-linecap="round" stroke-dasharray="289" stroke-dashoffset="140"/>
        </svg>
        <div class="face">
          <div class="temp" id="temp">26<sup>°</sup></div>
          <div class="feels" id="feels">FEELS 30°</div>
        </div>
      </div>
      <div class="hero-info">
        <div class="cond-row">
          <svg class="cond-icon" id="cond-icon" viewBox="0 0 24 24" fill="none"></svg>
          <div class="cond" id="cond-text">Rain showers</div>
        </div>
        <div class="sub">
          <span>H <b id="sub-hi">31°</b></span>
          <span>L <b id="sub-lo">24°</b></span>
          <span>Rain <b id="sub-rain">43%</b></span>
        </div>
        <div class="updated">
          <button class="refresh-btn" id="refresh-btn" title="Refresh now" aria-label="Refresh now">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round"><path d="M21 12a9 9 0 1 1-3-6.7M21 3v6h-6"/></svg>
          </button>
          <span id="updated-text">Updated 2 min ago</span>
        </div>
      </div>
    </div>

    <div class="grid" id="stat-grid"></div>

    <div class="card" id="local-card" style="margin-top:16px;">
      <h3>Local sensor</h3>
      <div class="grid" id="local-grid" style="margin-top:0;"></div>
      <p class="hint" id="local-caption" style="margin:10px 0 0;">—</p>
    </div>

    <div class="card" id="air-card" style="margin-top:16px;">
      <h3>Air quality</h3>
      <div class="grid" id="air-grid" style="margin-top:0;"></div>
      <p class="hint" id="air-caption" style="margin:10px 0 0;">—</p>
    </div>

  </section>

  <!-- ===================== SETTINGS ===================== -->
  <section id="tab-settings" hidden>
    <div class="cards">

      <div class="card">
        <h3>Location</h3>
        <p class="hint">US ZIP code. We resolve it to coordinates once and cache them.</p>
        <div class="row">
          <div class="field">
            <label for="zip-input">ZIP code</label>
            <input type="text" id="zip-input" inputmode="numeric" pattern="[0-9]{5}" maxlength="5" value="32082">
          </div>
          <button class="btn" id="zip-save">Update</button>
        </div>
        <p class="hint" style="margin:10px 0 0" id="latlon-readout">30.2419, -81.3901</p>
      </div>

      <div class="card">
        <h3>Local sensor</h3>
        <p class="hint">Hostname or IP of the AHT20+BMP280 node reading conditions right outside.</p>
        <div class="row">
          <div class="field">
            <label for="localhost-input">Sensor address</label>
            <input type="text" id="localhost-input" placeholder="localsensor.local" value="localsensor.local">
          </div>
          <button class="btn" id="localhost-save">Update</button>
        </div>
      </div>

      <div class="card">
        <h3>Air quality</h3>
        <p class="hint">Hostname or IP of the PMS5003 particulate sensor node.</p>
        <div class="row">
          <div class="field">
            <label for="airhost-input">Sensor address</label>
            <input type="text" id="airhost-input" placeholder="airquality.local" value="airquality.local">
          </div>
          <button class="btn" id="airhost-save">Update</button>
        </div>
      </div>

      <div class="card">
        <h3>Units</h3>
        <p class="hint">Applies to the dial, this page, and the round display.</p>
        <div class="seg" id="unit-seg">
          <button data-unit="c" class="active">°C</button>
          <button data-unit="f">°F</button>
        </div>
      </div>

      <div class="card">
        <h3>Screen timing</h3>
        <p class="hint">How long each screen holds before the station advances to the next.</p>
        <div class="slider-row">
          <input type="range" id="speed-slider" min="1" max="60" value="5">
          <span class="val" id="speed-val">5s</span>
        </div>
      </div>

      <div class="card">
        <h3>Now showing</h3>
        <p class="hint">Jump the physical station to a specific screen right now.</p>
        <div class="row">
          <div class="field" style="flex:1">
            <label for="screen-select">Screen</label>
            <select id="screen-select" style="width:100%;padding:10px 12px;border-radius:10px;border:1px solid var(--border-strong);background:var(--surface-2);color:var(--text);font-family:var(--font-mono);font-size:.9rem;">
              <option>Weather</option>
            </select>
          </div>
          <button class="btn ghost" id="goto-btn">Show now</button>
        </div>
      </div>

      <div class="card" style="grid-column:1 / -1;">
        <h3>Display metrics</h3>
        <p class="hint">The round display has room for four readouts beneath the temperature. Choose which ones — everything still shows here on the web dashboard.</p>
        <div class="toggle-list" id="toggle-list"></div>
        <p class="counter" style="margin-top:10px;"><b id="toggle-count">4</b>/4 selected</p>
      </div>

    </div>
  </section>

  <!-- ===================== NOTES ===================== -->
  <section id="tab-notes" hidden>
    <div class="card">
      <h3>Notes</h3>
      <p class="hint">Short reminders that cycle on the station alongside the weather.</p>
      <form class="note-form" id="note-form">
        <input type="text" id="note-input" maxlength="80" placeholder="e.g. bring in the packages" autocomplete="off">
        <button class="btn" type="submit">Add</button>
      </form>
      <div class="note-list" id="note-list"></div>
    </div>
  </section>

  <!-- ===================== POWER ===================== -->
  <section id="tab-power" hidden>
    <div class="cards">

      <div class="card">
        <h3>Display</h3>
        <p class="hint">Turn the round display off without powering down the station.</p>
        <div class="toggle-row" style="border-bottom:none; padding:11px 2px 0;">
          <div class="ti"><span class="name">Display on</span></div>
          <label class="switch"><input type="checkbox" id="power-on-toggle" checked><span class="track"></span></label>
        </div>
      </div>

      <div class="card">
        <h3>Brightness</h3>
        <p class="hint">The backlight has no dimmer wired up, so this dims the rendered colors instead.</p>
        <div class="slider-row">
          <input type="range" id="brightness-slider" min="5" max="100" value="100">
          <span class="val" id="brightness-val">100%</span>
        </div>
      </div>

      <div class="card" style="grid-column:1 / -1;">
        <h3>Schedule</h3>
        <p class="hint">Automatically turn the display on and off at set times, with a separate schedule for weekends.</p>
        <div class="toggle-row" style="padding-top:0;">
          <div class="ti"><span class="name">Enable schedule</span></div>
          <label class="switch"><input type="checkbox" id="schedule-enabled-toggle"><span class="track"></span></label>
        </div>
        <div class="row" style="margin-top:14px;">
          <div class="field">
            <label for="weekday-on">Weekdays &middot; on</label>
            <input type="time" id="weekday-on" value="07:00">
          </div>
          <div class="field">
            <label for="weekday-off">Weekdays &middot; off</label>
            <input type="time" id="weekday-off" value="22:00">
          </div>
        </div>
        <div class="row" style="margin-top:14px;">
          <div class="field">
            <label for="weekend-on">Weekends &middot; on</label>
            <input type="time" id="weekend-on" value="08:00">
          </div>
          <div class="field">
            <label for="weekend-off">Weekends &middot; off</label>
            <input type="time" id="weekend-off" value="23:00">
          </div>
        </div>
      </div>

    </div>
  </section>

  <footer>
    esp32-c3 · <a href="http://weather.local/" id="mdns-link">weather.local</a> · updates every 10 min
  </footer>

</div>

<div class="toast" id="toast"></div>

<script>
(function(){
  "use strict";

  // ---------- mock fallback (used only when /api/status is unreachable) ----------
  var MOCK = {
    online: true,
    demo: true,
    zip: "32082",
    latlon: "30.2419,-81.3901",
    place: "Ponte Vedra Beach",
    fahrenheit: false,
    cycleSeconds: 5,
    currentScreen: 0,
    localHost: "localsensor.local",
    airHost: "airquality.local",
    screens: ["Weather", "Hourly", "5-Day", "Local", "Air Quality", "bring in the packages", "water the ferns thursday"],
    updatedSecondsAgo: 132,
    weather: {
      code: 80, isDay: false, condition: "Rain showers",
      tempC: 25.7, feelsLikeC: 30.3, humidity: 91, windKph: 11.1, windGustKph: 16.2,
      pressureHpa: 1014.6, rainChancePct: 43, uvIndex: 7.45, highC: 30.6, lowC: 23.6
    },
    local: { online: true, tempC: 24.2, humidityPct: 55, pressureHpa: 1015.8, updatedSecondsAgo: 38,
      forecast: { state: "Fair", rainProbabilityPct: 20, pressureTrend: "Steady" } },
    air: { online: true, pm1_0: 4, pm2_5: 8, pm10: 11, aqi: 33, aqiCategory: "Good", updatedSecondsAgo: 45 },
    show: { humidity:true, wind:true, rainChance:true, feelsLike:false, highLow:true, pressure:false, uv:false },
    power: {
      on: true, brightness: 100,
      schedule: { enabled: false, weekdayOn:"07:00", weekdayOff:"22:00", weekendOn:"08:00", weekendOff:"23:00" }
    }
  };

  var METRICS = [
    { key:"humidity",   name:"Humidity",    unit:"%",    icon:"drop",   fmt:function(w){ return isNum(w.humidity)? Math.round(w.humidity): null; } },
    { key:"wind",       name:"Wind",        unit:"km/h", icon:"wind",   fmt:function(w){ return isNum(w.windKph)? Math.round(w.windKph): null; }, sub:function(w){ return isNum(w.windGustKph)? "gust "+Math.round(w.windGustKph): ""; } },
    { key:"rainChance", name:"Chance of rain", unit:"%", icon:"umbrella", fmt:function(w){ return isNum(w.rainChancePct)? Math.round(w.rainChancePct): null; } },
    { key:"feelsLike",  name:"Feels like",  unit:"°",    icon:"therm",  fmt:function(w,u){ return isNum(w.feelsLikeC)? Math.round(u(w.feelsLikeC)): null; } },
    { key:"highLow",    name:"High / Low",  unit:"",     icon:"hilo",   fmt:function(w,u){ return (isNum(w.highC)&&isNum(w.lowC))? Math.round(u(w.highC))+"°/"+Math.round(u(w.lowC))+"°": null; } },
    { key:"pressure",   name:"Pressure",    unit:"hPa",  icon:"gauge",  fmt:function(w){ return isNum(w.pressureHpa)? Math.round(w.pressureHpa): null; } },
    { key:"uv",         name:"UV index",    unit:"",     icon:"sun",    fmt:function(w){ return isNum(w.uvIndex)? w.uvIndex.toFixed(1): null; } }
  ];
  var MAX_ON_DEVICE = 4;

  function isNum(v){ return typeof v === "number" && !isNaN(v); }

  var ICONS = {
    drop:'<path d="M12 3s6 6.5 6 11a6 6 0 0 1-12 0c0-4.5 6-11 6-11Z" stroke="currentColor" stroke-width="1.8" stroke-linejoin="round"/>',
    wind:'<path d="M3 8h10.5a2.5 2.5 0 1 0-2.3-3.5M3 12h14a2.5 2.5 0 1 1-2.3 3.5M3 16h8" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"/>',
    umbrella:'<path d="M12 3C7 3 3 7 3 11.5h18C21 7 17 3 12 3Zm0 0v15.5a2.2 2.2 0 0 1-4 1.3" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/>',
    therm:'<path d="M12 14.5V5a2 2 0 1 0-4 0v9.5a4 4 0 1 0 4 0Z" stroke="currentColor" stroke-width="1.8" stroke-linejoin="round"/>',
    hilo:'<path d="M12 3v7m0 0 3-3m-3 3-3-3" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/><path d="M12 21v-7m0 0 3 3m-3-3-3 3" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/>',
    gauge:'<path d="M4 15a8 8 0 1 1 16 0M12 15l4.2-4.2" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"/>',
    sun:'<circle cx="12" cy="12" r="4" stroke="currentColor" stroke-width="1.8"/><path d="M12 2v2.5M12 19.5V22M22 12h-2.5M4.5 12H2M19 5l-1.8 1.8M6.8 17.2 5 19M19 19l-1.8-1.8M6.8 6.8 5 5" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"/>'
  };

  // WMO weather_code -> {cat, label}; mirrors the category logic drawn on the round display
  function codeInfo(code){
    if (code === 0) return { cat:"clear", label:"Clear sky" };
    if (code === 1) return { cat:"clear", label:"Mostly clear" };
    if (code === 2) return { cat:"pcloudy", label:"Partly cloudy" };
    if (code === 3) return { cat:"cloudy", label:"Overcast" };
    if (code === 45 || code === 48) return { cat:"fog", label:"Fog" };
    if (code === 51 || code === 53 || code === 55) return { cat:"rain", label:"Drizzle" };
    if (code === 56 || code === 57) return { cat:"rain", label:"Freezing drizzle" };
    if (code === 61 || code === 63 || code === 65) return { cat:"rain", label:"Rain" };
    if (code === 66 || code === 67) return { cat:"rain", label:"Freezing rain" };
    if (code === 71 || code === 73 || code === 75 || code === 77) return { cat:"snow", label:"Snow" };
    if (code === 80 || code === 81 || code === 82) return { cat:"rain", label:"Rain showers" };
    if (code === 85 || code === 86) return { cat:"snow", label:"Snow showers" };
    if (code === 95) return { cat:"storm", label:"Thunderstorm" };
    if (code === 96 || code === 99) return { cat:"storm", label:"Thunderstorm, hail" };
    return { cat:"cloudy", label:"—" };
  }

  function condIcon(cat, isDay){
    var s = 'stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round" fill="none"';
    if (cat === "clear" && isDay) return '<circle cx="12" cy="12" r="4.5" '+s+'/><path d="M12 2.5v2.2M12 19.3v2.2M21.5 12h-2.2M4.7 12H2.5M18.4 5.6l-1.6 1.6M7.2 16.8l-1.6 1.6M18.4 18.4l-1.6-1.6M7.2 7.2 5.6 5.6" '+s+'/>';
    if (cat === "clear" && !isDay) return '<path d="M20 14.5A8.5 8.5 0 1 1 9.5 4a6.8 6.8 0 0 0 10.5 10.5Z" '+s+'/>';
    if (cat === "pcloudy") return (isDay? '<circle cx="9" cy="9" r="3.4" '+s+'/><path d="M9 3.2v1.6M9 14v1.6M15.8 9h-1.6M3.8 9H2.2M13.8 4.2l-1.1 1.1M5.3 12.7l-1.1 1.1" '+s+'/>' : '<path d="M14.5 8.2A5.6 5.6 0 0 1 8 3.4a4.4 4.4 0 0 0 5.6 5.9Z" '+s+'/>') +
        '<path d="M6 21a4 4 0 0 1-.5-8 5 5 0 0 1 9.6-1.6A3.8 3.8 0 0 1 17.5 19c0 .3 0 .7-.1 1Z" '+s+'/>';
    if (cat === "cloudy") return '<path d="M7 20a4.2 4.2 0 0 1-.6-8.3 5.4 5.4 0 0 1 10.4-1.7A4 4 0 0 1 16.4 18M7 20h9.4M7 20a4 4 0 0 1 0-8" '+s+'/>';
    if (cat === "fog") return '<path d="M4 8h16M3 12h18M4 16h16M6 20h12" '+s+'/>';
    if (cat === "rain") return '<path d="M6.5 15a4 4 0 0 1-.4-8 5.2 5.2 0 0 1 10 -1.6A3.8 3.8 0 0 1 15.7 13" '+s+'/><path d="M8 17.5 7 20M12 17.5 11 20M16 17.5 15 20" '+s+'/>';
    if (cat === "storm") return '<path d="M6.5 13a4 4 0 0 1-.4-8 5.2 5.2 0 0 1 10 -1.6A3.8 3.8 0 0 1 15.7 11" '+s+'/><path d="M13 12.5 10 17h3l-2 4.5" '+s+'/>';
    if (cat === "snow") return '<path d="M6.5 13a4 4 0 0 1-.4-8 5.2 5.2 0 0 1 10 -1.6A3.8 3.8 0 0 1 15.7 11" '+s+'/><path d="M8 17v4M6 18l4 2M12 17v4M14 18l-4 2M16 17v4M14 18l4 2" '+s+'/>';
    return '<circle cx="12" cy="12" r="8" '+s+'/>';
  }

  var toCF = function(c){ return c*9/5+32; };

  var state = null;
  var editingSettings = false;

  function u(c){ return state.fahrenheit ? toCF(c) : c; }

  function toast(msg){
    var t = document.getElementById("toast");
    t.textContent = msg;
    t.classList.add("show");
    clearTimeout(toast._h);
    toast._h = setTimeout(function(){ t.classList.remove("show"); }, 2200);
  }

  function render(){
    var w = state.weather, info = codeInfo(w.code);

    document.body.dataset.cond = (info.cat === "clear" && !w.isDay) ? "night" : info.cat;
    document.body.classList.toggle("demo", !!state.demo);

    var pill = document.getElementById("status-pill");
    pill.classList.toggle("offline", !state.online);
    document.getElementById("status-text").textContent = state.online ? "Online" : "Offline";
    document.getElementById("hdr-place").textContent = state.zip + (state.place ? " · " + state.place.toUpperCase() : "");

    document.getElementById("temp").innerHTML = (isNum(w.tempC) ? Math.round(u(w.tempC)) : "--") + "<sup>°</sup>";
    document.getElementById("feels").textContent = isNum(w.feelsLikeC) ? "FEELS " + Math.round(u(w.feelsLikeC)) + "°" : "";
    document.getElementById("cond-text").textContent = info.label;
    document.getElementById("cond-icon").innerHTML = condIcon((info.cat === "clear" && !w.isDay) ? "clear" : info.cat, w.isDay);
    document.getElementById("sub-hi").textContent = isNum(w.highC) ? Math.round(u(w.highC)) + "°" : "--";
    document.getElementById("sub-lo").textContent = isNum(w.lowC) ? Math.round(u(w.lowC)) + "°" : "--";
    document.getElementById("sub-rain").textContent = isNum(w.rainChancePct) ? Math.round(w.rainChancePct) + "%" : "--";

    var mins = Math.round((state.updatedSecondsAgo||0)/60);
    document.getElementById("updated-text").textContent =
      state.online ? "Updated " + (mins < 1 ? "just now" : mins + " min ago") : "Last seen " + mins + " min ago";

    var pct = isNum(w.humidity) ? w.humidity : 0;
    var ring = document.getElementById("dial-ring");
    var C = 2 * Math.PI * 46;
    ring.setAttribute("stroke-dasharray", C.toFixed(1));
    ring.setAttribute("stroke-dashoffset", (C * (1 - pct/100)).toFixed(1));

    renderStatGrid(w);
    renderLocalCard();
    renderAirCard();
    if (!editingSettings) { renderSettings(); renderPower(); }
    renderNotes();
  }

  function renderLocalCard(){
    var l = state.local || {};
    var online = !!l.online;
    var grid = document.getElementById("local-grid");
    grid.innerHTML = "";
    var fc = l.forecast;
    var items = [
      { name:"Temp", val: isNum(l.tempC) ? Math.round(u(l.tempC)) + "°" : "--" },
      { name:"Humidity", val: isNum(l.humidityPct) ? Math.round(l.humidityPct) + "%" : "--" },
      { name:"Pressure", val: isNum(l.pressureHpa) ? Math.round(l.pressureHpa) + " hPa" : "--" },
      { name:"Rain chance", val: (fc && isNum(fc.rainProbabilityPct)) ? fc.rainProbabilityPct + "%" : "--" }
    ];
    items.forEach(function(it){
      var el = document.createElement("div");
      el.className = "stat";
      el.innerHTML = '<div class="head"><span class="label">'+it.name+'</span></div><div class="value">'+it.val+'</div>';
      grid.appendChild(el);
    });
    var cap = document.getElementById("local-caption");
    var mins = Math.round((l.updatedSecondsAgo||0)/60);
    var freshness = online
      ? "Live via " + (state.localHost || "sensor") + " · updated " + (mins < 1 ? "just now" : mins + " min ago")
      : "Sensor offline (" + (state.localHost || "not configured") + ")";
    cap.textContent = (fc && fc.state) ? freshness + " · " + fc.state + " (" + fc.pressureTrend + ")" : freshness;
    cap.style.color = online ? "" : "#d64545";
  }

  function renderAirCard(){
    var a = state.air || {};
    var online = !!a.online;
    var grid = document.getElementById("air-grid");
    grid.innerHTML = "";
    var items = [
      { name:"PM2.5", val: isNum(a.pm2_5) ? a.pm2_5 + " µg/m³" : "--" },
      { name:"PM10", val: isNum(a.pm10) ? a.pm10 + " µg/m³" : "--" },
      { name:"AQI", val: isNum(a.aqi) ? a.aqi : "--" }
    ];
    items.forEach(function(it){
      var el = document.createElement("div");
      el.className = "stat";
      el.innerHTML = '<div class="head"><span class="label">'+it.name+'</span></div><div class="value">'+it.val+'</div>';
      grid.appendChild(el);
    });
    var cap = document.getElementById("air-caption");
    var mins = Math.round((a.updatedSecondsAgo||0)/60);
    var freshness = online
      ? "Live via " + (state.airHost || "sensor") + " · updated " + (mins < 1 ? "just now" : mins + " min ago")
      : "Sensor offline (" + (state.airHost || "not configured") + ")";
    cap.textContent = (online && a.aqiCategory) ? freshness + " · " + a.aqiCategory : freshness;
    cap.style.color = online ? "" : "#d64545";
  }

  function renderStatGrid(w){
    var grid = document.getElementById("stat-grid");
    grid.innerHTML = "";
    METRICS.forEach(function(m){
      var val = m.fmt(w, u);
      var el = document.createElement("div");
      el.className = "stat" + (state.show[m.key] ? " on-device" : "");
      var sub = m.sub ? m.sub(w) : "";
      el.innerHTML =
        '<div class="head"><svg class="icon" viewBox="0 0 24 24">'+ICONS[m.icon]+'</svg><span class="label">'+m.name+'</span></div>' +
        '<div class="value">'+ (val === null ? "--" : val) + (val !== null && m.unit ? '<span class="unit">'+m.unit+'</span>' : '') + '</div>' +
        (sub ? '<div class="label" style="text-transform:none;letter-spacing:0;font-weight:500;">'+sub+'</div>' : '') +
        (state.show[m.key] ? '<span class="chip">ON DIAL</span>' : '');
      grid.appendChild(el);
    });
  }

  function renderSettings(){
    document.getElementById("zip-input").value = state.zip;
    document.getElementById("latlon-readout").textContent = state.latlon;
    document.getElementById("localhost-input").value = state.localHost || "";
    document.getElementById("airhost-input").value = state.airHost || "";
    document.getElementById("speed-slider").value = state.cycleSeconds;
    document.getElementById("speed-val").textContent = state.cycleSeconds + "s";
    document.querySelectorAll("#unit-seg button").forEach(function(b){
      b.classList.toggle("active", (b.dataset.unit === "f") === !!state.fahrenheit);
    });

    var sel = document.getElementById("screen-select");
    sel.innerHTML = "";
    state.screens.forEach(function(label, i){
      var o = document.createElement("option");
      o.value = i; o.textContent = label; if (i === state.currentScreen) o.selected = true;
      sel.appendChild(o);
    });

    var list = document.getElementById("toggle-list");
    list.innerHTML = "";
    var enabledCount = METRICS.filter(function(m){ return state.show[m.key]; }).length;
    document.getElementById("toggle-count").textContent = enabledCount;

    METRICS.forEach(function(m){
      var checked = !!state.show[m.key];
      var row = document.createElement("div");
      row.className = "toggle-row";
      row.innerHTML =
        '<div class="ti"><svg class="icon" viewBox="0 0 24 24">'+ICONS[m.icon]+'</svg><span class="name">'+m.name+'</span></div>' +
        '<label class="switch"><input type="checkbox" data-key="'+m.key+'" '+(checked?"checked":"")+' '+((!checked && enabledCount>=MAX_ON_DEVICE)?"disabled":"")+'><span class="track"></span></label>';
      list.appendChild(row);
    });
  }

  function renderPower(){
    var p = state.power;
    document.getElementById("power-on-toggle").checked = !!p.on;
    document.getElementById("brightness-slider").value = p.brightness;
    document.getElementById("brightness-val").textContent = p.brightness + "%";
    document.getElementById("schedule-enabled-toggle").checked = !!p.schedule.enabled;
    document.getElementById("weekday-on").value = p.schedule.weekdayOn;
    document.getElementById("weekday-off").value = p.schedule.weekdayOff;
    document.getElementById("weekend-on").value = p.schedule.weekendOn;
    document.getElementById("weekend-off").value = p.schedule.weekendOff;
  }

  function renderNotes(){
    var list = document.getElementById("note-list");
    list.innerHTML = "";
    var notes = state.screens.slice(4);
    if (!notes.length){
      list.innerHTML = '<p class="empty">No notes yet — add one below and it\'ll cycle on the station.</p>';
      return;
    }
    notes.forEach(function(text, i){
      var row = document.createElement("div");
      row.className = "note";
      row.innerHTML =
        '<span class="idx">'+String(i+1).padStart(2,"0")+'</span>' +
        '<span class="txt"></span>' +
        '<button class="icon-btn" data-goto="'+(i+4)+'" title="Show on station" aria-label="Show on station"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M5 12h14M13 6l6 6-6 6"/></svg></button>' +
        '<button class="icon-btn" data-del="'+i+'" title="Delete" aria-label="Delete note"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M6 6l12 12M18 6 6 18"/></svg></button>';
      row.querySelector(".txt").textContent = text;
      list.appendChild(row);
    });
  }

  // ---------- networking ----------
  function apiGet(){
    return fetch("/api/status").then(function(r){ if(!r.ok) throw 0; return r.json(); });
  }
  function apiPost(url, body){
    return fetch(url, { method:"POST", headers:{"Content-Type":"application/json"}, body: JSON.stringify(body) })
      .then(function(r){ if(!r.ok) throw 0; return r.json(); });
  }

  function load(){
    apiGet().then(function(data){
      state = data;
      render();
    }).catch(function(){
      state = MOCK;
      render();
    });
  }

  function refreshInPlace(){
    if (state.demo){ toast("Preview mode — nothing to refresh"); return; }
    var btn = document.getElementById("refresh-btn");
    btn.classList.add("spin");
    apiPost("/api/refresh", {}).then(function(data){
      state = data; render(); toast("Weather refreshed");
    }).catch(function(){ toast("Refresh failed — station unreachable"); })
      .finally(function(){ setTimeout(function(){ btn.classList.remove("spin"); }, 400); });
  }

  // ---------- wiring ----------
  document.querySelectorAll("nav.tabs button").forEach(function(btn){
    btn.addEventListener("click", function(){
      document.querySelectorAll("nav.tabs button").forEach(function(b){ b.classList.remove("active"); b.setAttribute("aria-selected","false"); });
      btn.classList.add("active"); btn.setAttribute("aria-selected","true");
      ["overview","settings","notes","power"].forEach(function(id){
        document.getElementById("tab-"+id).hidden = (id !== btn.dataset.tab);
      });
    });
  });

  document.getElementById("refresh-btn").addEventListener("click", refreshInPlace);

  document.getElementById("zip-save").addEventListener("click", function(){
    var zip = document.getElementById("zip-input").value.trim();
    if (!/^\d{5}$/.test(zip)){ toast("Enter a 5-digit ZIP code"); return; }
    if (state.demo){ state.zip = zip; toast("Preview mode — saved locally only"); render(); return; }
    apiPost("/api/settings", { zip: zip }).then(function(data){ state = data; render(); toast("Location updated"); })
      .catch(function(){ toast("Couldn't resolve that ZIP"); });
  });

  document.getElementById("localhost-save").addEventListener("click", function(){
    var host = document.getElementById("localhost-input").value.trim();
    if (!host){ toast("Enter a hostname or IP"); return; }
    if (state.demo){ state.localHost = host; toast("Preview mode — saved locally only"); render(); return; }
    apiPost("/api/settings", { localHost: host }).then(function(data){ state = data; render(); toast("Sensor address updated"); })
      .catch(function(){ toast("Couldn't save sensor address"); });
  });

  document.getElementById("airhost-save").addEventListener("click", function(){
    var host = document.getElementById("airhost-input").value.trim();
    if (!host){ toast("Enter a hostname or IP"); return; }
    if (state.demo){ state.airHost = host; toast("Preview mode — saved locally only"); render(); return; }
    apiPost("/api/settings", { airHost: host }).then(function(data){ state = data; render(); toast("Sensor address updated"); })
      .catch(function(){ toast("Couldn't save sensor address"); });
  });

  document.getElementById("unit-seg").addEventListener("click", function(e){
    var b = e.target.closest("button"); if (!b) return;
    var f = b.dataset.unit === "f";
    state.fahrenheit = f; render();
    if (!state.demo) apiPost("/api/settings", { fahrenheit: f }).catch(function(){ toast("Couldn't save units"); });
  });

  var speedSlider = document.getElementById("speed-slider");
  speedSlider.addEventListener("input", function(){
    document.getElementById("speed-val").textContent = speedSlider.value + "s";
  });
  speedSlider.addEventListener("change", function(){
    var secs = parseInt(speedSlider.value, 10);
    state.cycleSeconds = secs;
    if (!state.demo) apiPost("/api/settings", { cycleSeconds: secs }).then(function(d){ state=d; }).catch(function(){ toast("Couldn't save timing"); });
    else toast("Preview mode — saved locally only");
  });

  document.getElementById("goto-btn").addEventListener("click", function(){
    var page = parseInt(document.getElementById("screen-select").value, 10);
    if (state.demo){ state.currentScreen = page; toast("Preview mode only"); renderSettings(); return; }
    apiPost("/api/goto", { page: page }).then(function(d){ state = d; render(); toast("Station updated"); });
  });

  document.getElementById("toggle-list").addEventListener("change", function(e){
    var input = e.target.closest("input[type=checkbox]"); if (!input) return;
    var key = input.dataset.key;
    var show = Object.assign({}, state.show); show[key] = input.checked;
    var count = METRICS.filter(function(m){ return show[m.key]; }).length;
    if (count > MAX_ON_DEVICE){ input.checked = false; toast("Up to 4 on the display — turn one off first"); return; }
    state.show = show;
    render();
    renderSettings(); // toggling a checkbox keeps it focused, which makes render() skip this via editingSettings — force it so sibling disabled states/counter update immediately
    if (!state.demo) apiPost("/api/settings", { show: show }).then(function(d){ state=d; }).catch(function(){ toast("Couldn't save"); });
    else toast("Preview mode — saved locally only");
  });

  document.getElementById("note-form").addEventListener("submit", function(e){
    e.preventDefault();
    var input = document.getElementById("note-input");
    var text = input.value.trim();
    if (!text) return;
    if (state.demo){ state.screens.push(text); input.value=""; render(); toast("Preview mode — saved locally only"); return; }
    apiPost("/api/notes/add", { text: text }).then(function(d){ state = d; input.value=""; render(); toast("Note added"); });
  });

  document.getElementById("note-list").addEventListener("click", function(e){
    var del = e.target.closest("[data-del]");
    var go = e.target.closest("[data-goto]");
    if (del){
      var i = parseInt(del.dataset.del, 10);
      if (state.demo){ state.screens.splice(i+4,1); render(); return; }
      apiPost("/api/notes/delete", { index:i }).then(function(d){ state = d; render(); toast("Note deleted"); });
    } else if (go){
      var page = parseInt(go.dataset.goto, 10);
      if (state.demo){ toast("Preview mode only"); return; }
      apiPost("/api/goto", { page: page }).then(function(d){ state = d; render(); toast("Station updated"); });
    }
  });

  document.getElementById("power-on-toggle").addEventListener("change", function(e){
    var on = e.target.checked;
    state.power.on = on;
    if (state.demo){ toast("Preview mode — saved locally only"); return; }
    apiPost("/api/settings", { power: { on: on } }).then(function(d){ state=d; render(); }).catch(function(){ toast("Couldn't save"); });
  });

  var brightnessSlider = document.getElementById("brightness-slider");
  brightnessSlider.addEventListener("input", function(){
    document.getElementById("brightness-val").textContent = brightnessSlider.value + "%";
  });
  brightnessSlider.addEventListener("change", function(){
    var pct = parseInt(brightnessSlider.value, 10);
    state.power.brightness = pct;
    if (state.demo){ toast("Preview mode — saved locally only"); return; }
    apiPost("/api/settings", { power: { brightness: pct } }).then(function(d){ state=d; }).catch(function(){ toast("Couldn't save brightness"); });
  });

  document.getElementById("schedule-enabled-toggle").addEventListener("change", function(e){
    var enabled = e.target.checked;
    state.power.schedule.enabled = enabled;
    if (state.demo){ toast("Preview mode — saved locally only"); return; }
    apiPost("/api/settings", { power: { schedule: { enabled: enabled } } }).then(function(d){ state=d; }).catch(function(){ toast("Couldn't save schedule"); });
  });

  [["weekday-on","weekdayOn"],["weekday-off","weekdayOff"],["weekend-on","weekendOn"],["weekend-off","weekendOff"]].forEach(function(pair){
    document.getElementById(pair[0]).addEventListener("change", function(e){
      var val = e.target.value; if (!val) return;
      state.power.schedule[pair[1]] = val;
      if (state.demo){ toast("Preview mode — saved locally only"); return; }
      var sched = {}; sched[pair[1]] = val;
      apiPost("/api/settings", { power: { schedule: sched } }).then(function(d){ state=d; }).catch(function(){ toast("Couldn't save schedule"); });
    });
  });

  document.addEventListener("focusin", function(e){ editingSettings = !!e.target.closest("#tab-settings input, #tab-settings select, #tab-power input"); });
  document.addEventListener("focusout", function(){ editingSettings = false; });

  load();
  setInterval(function(){
    if (document.visibilityState === "visible") load();
  }, 20000);

})();
</script>
</body>
</html>
)HTMLPAGE";
