
// ============================================================
// SPECIES SETPOINTS  — synchronized with ESP32 firmware
// ============================================================
const SPECIES = {
  pangasius:  { label:'Pangasius',  targetTemp:28.0, tdsMin:100, tdsMax:400, maxTurbidity:100, hysteresis:0.5 },
  tilapia:    { label:'Tilapia',    targetTemp:30.0, tdsMin:200, tdsMax:500, maxTurbidity:80,hysteresis:0.5 },
  zebra_fish: { label:'Zebra Fish', targetTemp:26.0, tdsMin:50, tdsMax:150, maxTurbidity:30, hysteresis:0.5 }
};

const NODE_RED = 'http://192.168.1.17:1880';

let currentSpecies = 'tilapia';
let lastActuators  = null;
let manualMode     = false;
const logs = [];

// ============================================================
// THEME
// ============================================================
function toggleTheme() {
  const html = document.documentElement;
  const dark = html.getAttribute('data-theme') === 'dark';
  html.setAttribute('data-theme', dark ? 'light' : 'dark');
  document.querySelector('.theme-btn').textContent = dark ? '🌙 Dark Mode' : '☀ Light Mode';
}

// ============================================================
// MODE TOGGLE
// ============================================================
function toggleMode() {
  manualMode = !manualMode;
  const btn = document.getElementById('mode-btn');
  btn.textContent = manualMode ? 'MANUAL' : 'AUTO';
  btn.style.color = manualMode ? 'var(--red)' : 'var(--green)';
  btn.style.borderColor = manualMode ? 'var(--red)' : 'var(--green)';

  if (!manualMode) {
    fetch(`${NODE_RED}/set-actuators`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ active: false, heat: false, filter: false, air: false })
    }).catch(() => {});
    addLog('Mode → AUTO (ESP32 closed-loop)');
  } else {
    addLog('Mode → MANUAL (dashboard control)');
  }
}

// ============================================================
// SPECIES SELECTION
// ============================================================
function selectSpecies(sp, btn) {
  if (sp === currentSpecies) return;
  currentSpecies = sp;

  document.querySelectorAll('.spc-btn').forEach(b => b.classList.remove('active'));
  btn.classList.add('active');

  document.getElementById('active-profile-banner').textContent =
    SPECIES[sp].label.toUpperCase() + ' PROFILE ACTIVE';

  Object.keys(SPECIES).forEach(key => {
    const tag = document.getElementById('tag-' + key);
    if (!tag) return;
    if (key === sp) {
      tag.textContent = 'Active';
      tag.classList.remove('inactive');
    } else {
      tag.textContent = 'Profile ' + (Object.keys(SPECIES).indexOf(key) + 1);
      tag.classList.add('inactive');
    }
  });

  updateSetpointLabels();
  fetch(`${NODE_RED}/set-fish?type=${sp}`).catch(() => {});
  addLog(`Species changed → ${SPECIES[sp].label}`);
}

// ============================================================
// CLOSED-LOOP LOGIC (used in MANUAL mode only)
// ============================================================
function calculateActuators(sensors) {
  const sp = SPECIES[currentSpecies];
  let heat = lastActuators ? lastActuators.heat : false;
  if (sensors.temp < sp.targetTemp - sp.hysteresis) heat = true;
  else if (sensors.temp > sp.targetTemp + sp.hysteresis) heat = false;

  const filter = (sensors.tds > sp.tdsMax) || (sensors.turbidity > sp.maxTurbidity);
  const air    = filter || (sensors.temp > sp.targetTemp + 2.0);
  return { heat, filter, air };
}

// ============================================================
// UI HELPERS
// ============================================================
function updateSetpointLabels() {
  const sp = SPECIES[currentSpecies];
  document.getElementById('sp-temp').textContent = `Target: ${sp.targetTemp} °C`;
  document.getElementById('sp-tds').textContent  = `Range: ${sp.tdsMin} – ${sp.tdsMax} ppm`;
  document.getElementById('sp-turb').textContent = `Max: ${sp.maxTurbidity} NTU`;
}

function setRing(ringId, pctId, pct) {
  const offset = 238.76 - (pct / 100) * 238.76;
  const r = document.getElementById(ringId); if (r) r.style.strokeDashoffset = offset;
  const p = document.getElementById(pctId);  if (p) p.textContent = pct + '%';
}

function setActuator(rowId, badgeId, on) {
  const row   = document.getElementById(rowId);
  const badge = document.getElementById(badgeId);
  if (!row || !badge) return;
  row.classList.toggle('on', on);
  badge.classList.toggle('on', on);
  badge.textContent = on ? 'ON' : 'OFF';
}

function setSensor(valId, barId, display, pct, status) {
  const el = document.getElementById(valId);
  if (el) {
    el.innerHTML = display;
    el.className = 'sv' + (status === 'danger' ? ' danger' : status === 'warn' ? ' warn' : '');
  }
  const bar = document.getElementById(barId);
  if (bar) bar.style.width = Math.min(100, Math.max(0, pct)) + '%';
}

function addLog(msg) {
  const t = new Date().toTimeString().slice(0, 8);
  logs.unshift({ t, msg });
  if (logs.length > 30) logs.pop();
  const c = document.getElementById('ai-log');
  if (c) c.innerHTML = logs.map(e =>
    `<div class="log-e"><span class="log-t">${e.t}</span><span>${e.msg}</span></div>`
  ).join('');
}

// ============================================================
// MAIN POLLING LOOP
// ============================================================
async function updateDashboard() {
  try {
    const res = await fetch(`${NODE_RED}/get-data`, { method: 'GET' });
    if (!res.ok) throw new Error('HTTP ' + res.status);
    const data = await res.json();

    document.getElementById('conn-status').textContent = 'ONLINE';
    const s  = data.sensors;
    const sp = SPECIES[currentSpecies];

    // ── Actuators: use ESP32 auto states unless MANUAL mode is active ──
    let act;
    if (manualMode) {
      act = calculateActuators(s);
      fetch(`${NODE_RED}/set-actuators`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ active: true, ...act })
      }).catch(() => {});
    } else {
      act = data.actuators || { heat: false, filter: false, air: false };
    }

    setActuator('act-heat',   'badge-heat',   act.heat);
    setActuator('act-filter', 'badge-filter', act.filter);
    setActuator('act-air',    'badge-air',    act.air);

    // ── Sensors ──
    const tPct = ((s.temp - 15) / 25) * 100;
    const tSt  = (s.temp < sp.targetTemp - 2 || s.temp > sp.targetTemp + 4) ? 'warn' : '';
    setSensor('val-temp', 'bar-temp', `${s.temp.toFixed(1)}<span class="su">°C</span>`, tPct, tSt);

    const dPct = (s.tds / 1000) * 100;
    const dSt  = (s.tds < sp.tdsMin || s.tds > sp.tdsMax) ? 'warn' : '';
    setSensor('val-tds', 'bar-tds', `${Math.round(s.tds)}<span class="su">ppm</span>`, dPct, dSt);

    const turb = s.turbidity || 0;
    const uPct = (turb / 500) * 100;
    const uSt  = turb > sp.maxTurbidity ? 'danger' : '';
    setSensor('val-turb', 'bar-turb', `${turb.toFixed(1)}<span class="su">NTU</span>`, uPct, uSt);

    // ── AI / Vision ──
    if (data.ai) {
      document.getElementById('ai-species').textContent = data.ai.detected_species || '--';
      document.getElementById('ai-count').textContent   = data.ai.count    ?? 0;
      document.getElementById('ai-size').textContent    = data.ai.avg_size ?? '0 cm';
      document.getElementById('ai-conf').textContent    = (data.ai.confidence ?? '--') + '%';
    }

    // ── Timestamp ──
    document.getElementById('ts').textContent = new Date().toTimeString().slice(0, 8);

    // ── Health rings ──
    setRing('ring-wifi', 'pct-wifi', 90);
    setRing('ring-temp', 'pct-temp', s.temp > 0 ? 96 : 42);
    setRing('ring-tds',  'pct-tds',  s.tds  > 0 ? 93 : 42);
    setRing('ring-turb', 'pct-turb', turb  >= 0 ? 89 : 42);

    // ── Activity log ──
    if (lastActuators) {
      if (lastActuators.heat   !== act.heat)   addLog(`Heater → ${act.heat   ? 'ON' : 'OFF'}`);
      if (lastActuators.filter !== act.filter) addLog(`Filter → ${act.filter ? 'ON' : 'OFF'}`);
      if (lastActuators.air    !== act.air)    addLog(`Aerator → ${act.air   ? 'ON' : 'OFF'}`);
    }
    lastActuators = act;

  } catch (e) {
    document.getElementById('conn-status').textContent = 'OFFLINE';
    setRing('ring-wifi', 'pct-wifi', 0);
    addLog(`⚠ Fetch error: ${e.message} | Target: ${NODE_RED}`);
  }
}
// ── DATABASE: Charts ────────────────────────────────────
let currentRange = '1h';

const chartDefaults = {
  responsive: true, maintainAspectRatio: false,
  animation: { duration: 600 },
  plugins: { legend: { display: false }, tooltip: {
    backgroundColor: 'rgba(10,22,40,0.95)', titleColor: '#f5c400',
    bodyColor: '#e8edf5', borderColor: 'rgba(245,196,0,0.3)', borderWidth: 1,
    titleFont: { family:"'Share Tech Mono',monospace", size:11 },
    bodyFont: { family:"'Barlow',sans-serif", size:12 }
  }},
  scales: {
    x: { ticks:{ color:'#7a91b0', font:{ family:"'Share Tech Mono',monospace", size:10 }, maxTicksLimit:8 },
         grid:{ color:'rgba(245,196,0,0.05)' }, border:{ color:'rgba(245,196,0,0.1)' } },
    y: { ticks:{ color:'#7a91b0', font:{ family:"'Share Tech Mono',monospace", size:10 } },
         grid:{ color:'rgba(245,196,0,0.05)' }, border:{ color:'rgba(245,196,0,0.1)' } }
  }
};

function makeChart(id, color, fill) {
  const ctx = document.getElementById(id).getContext('2d');
  const gradient = ctx.createLinearGradient(0, 0, 0, 220);
  gradient.addColorStop(0, color.replace(')', ',0.25)').replace('rgb','rgba'));
  gradient.addColorStop(1, color.replace(')', ',0)').replace('rgb','rgba'));
  return new Chart(ctx, {
    type: 'line',
    data: { labels: [], datasets: [{ data: [], borderColor: color, borderWidth: 2,
      backgroundColor: fill ? gradient : 'transparent', fill: fill,
      pointRadius: 0, pointHoverRadius: 4, tension: 0.4 }] },
    options: JSON.parse(JSON.stringify(chartDefaults))
  });
}

const dbCharts = {
  temp: makeChart('chart-temp', 'rgb(245,196,0)', true),
  tds:  makeChart('chart-tds',  'rgb(0,212,255)', true),
  turb: makeChart('chart-turb', 'rgb(0,230,118)', false)
};

function formatLabel(ts) {
  const d = new Date(ts + 'Z');
  if (currentRange === '7d') return d.toLocaleDateString([], { month:'short', day:'numeric' });
  return d.toLocaleTimeString([], { hour:'2-digit', minute:'2-digit' });
}

// ── DATABASE: History fetch ──────────────────────────────
async function fetchHistory() {
  try {
    const res = await fetch(`${NODE_RED}/get-history?range=${currentRange}`);
    if (!res.ok) return;
    const rows = await res.json();
    if (!Array.isArray(rows) || !rows.length) return;

    const labels = rows.map(r => formatLabel(r.ts));
    dbCharts.temp.data.labels = labels;
    dbCharts.temp.data.datasets[0].data = rows.map(r => r.temp);
    dbCharts.temp.update('none');

    dbCharts.tds.data.labels = labels;
    dbCharts.tds.data.datasets[0].data = rows.map(r => r.tds);
    dbCharts.tds.update('none');

    dbCharts.turb.data.labels = labels;
    dbCharts.turb.data.datasets[0].data = rows.map(r => r.turbidity);
    dbCharts.turb.update('none');

    document.getElementById('stat-readings').textContent = rows.length.toLocaleString();
  } catch(e) {}
}

// ── DATABASE: Alerts fetch ───────────────────────────────
async function fetchAlerts() {
  try {
    const res = await fetch(`${NODE_RED}/get-alerts`);
    if (!res.ok) return;
    const alerts = await res.json();

    document.getElementById('stat-alerts').textContent = alerts.length;
    document.getElementById('alert-count-badge').textContent =
      `${alerts.length} ALERT${alerts.length !== 1 ? 'S' : ''}`;

    const list = document.getElementById('alert-list');
    if (!alerts.length) {
      list.innerHTML = '<div class="no-alerts">✓ No alerts — system nominal</div>';
      return;
    }
    list.innerHTML = alerts.map(a => {
      const ts = new Date(a.ts + 'Z').toLocaleTimeString([], { hour:'2-digit', minute:'2-digit', second:'2-digit' });
      return `<div class="alert-item ${a.level}">
        <span class="alert-level">${a.level}</span>
        <span class="alert-msg">${a.message}</span>
        <span class="alert-ts">${ts}</span>
      </div>`;
    }).join('');
  } catch(e) {}
}

// ── DATABASE: Range + Export ─────────────────────────────
function setRange(r, btn) {
  currentRange = r;
  document.querySelectorAll('.range-btn').forEach(b => b.classList.remove('active'));
  btn.classList.add('active');
  fetchHistory();
}

function exportCSV() {
  const a = document.createElement('a');
  a.href = `${NODE_RED}/export-csv?range=${currentRange}`;
  a.download = `aquarium_${currentRange}.csv`;
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  addLog(`CSV exported — range: ${currentRange.toUpperCase()}`);
}

// ── DATABASE: Auto-refresh ───────────────────────────────
fetchHistory();
fetchAlerts();
setInterval(fetchHistory, 15000);
setInterval(fetchAlerts,  10000);
// ============================================================
// INIT
// ============================================================
updateSetpointLabels();
addLog('System initialized — Tilapia profile loaded');
updateDashboard();
setInterval(updateDashboard, 1000);