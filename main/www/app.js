'use strict';

// =====================================================
// GMS INTERNAL - FRONTEND APPLICATION
// =====================================================

const page = document.body.dataset.page;

let csrf = '';

const $ = id => document.getElementById(id);

const fmt = (v, d = 2) =>
  typeof v === 'number' && Number.isFinite(v)
    ? v.toFixed(d)
    : '—';

// =====================================================
// MESSAGE HELPER
// =====================================================

function msg(id, text, error = false) {
  const el = $(id);

  if (el) {
    el.textContent = text;
    el.style.color = error ? '#b45a31' : '#15775c';
  }
}

// =====================================================
// API HELPER
// =====================================================

async function api(path, opts = {}) {
  const init = {
    credentials: 'same-origin',
    cache: 'no-store',
    ...opts
  };

  if (opts.method === 'POST') {
    init.headers = {
      'Content-Type': 'application/x-www-form-urlencoded',
      'X-GMS-CSRF': csrf,
      ...(opts.headers || {})
    };
  }

  const r = await fetch(path, init);

  if (r.status === 401 && page !== 'login') {
    location.href = '/login';
    throw new Error('Sesi habis');
  }

  const data = await r.json();

  if (!r.ok) {
    throw new Error(data.error || 'Permintaan gagal');
  }

  return data;
}

function formBody(form) {
  return new URLSearchParams(
    new FormData(form)
  ).toString();
}

function toggleBusy(form, busy) {
  const btn = form.querySelector(
    'button[type="submit"]'
  );

  if (btn) {
    btn.disabled = busy;
  }
}

// =====================================================
// LOGIN
// =====================================================

async function setupLogin() {
  const form = $('login-form');

  form.addEventListener('submit', async ev => {
    ev.preventDefault();

    toggleBusy(form, true);
    msg('message', 'Memeriksa akses…');

    try {
      await api('/api/login', {
        method: 'POST',
        body: formBody(form)
      });

      location.href = '/dashboard';

    } catch (e) {
      msg('message', e.message, true);

    } finally {
      toggleBusy(form, false);
    }
  });
}

// =====================================================
// LOGOUT
// =====================================================

async function logout() {
  try {
    await api('/api/logout', {
      method: 'POST',
      body: ''
    });
  } catch (e) {
    console.warn(e.message);
  }

  location.href = '/login';
}

function nav() {
  const btn = $('logout');

  if (btn) {
    btn.addEventListener('click', logout);
  }
}

// =====================================================
// DASHBOARD TREND CHART
// =====================================================

const samples = [];

function drawTrend() {
  const canvas = $('trend');

  if (!canvas) return;

  const scale = window.devicePixelRatio || 1;
  const r = canvas.getBoundingClientRect();

  const width = Math.max(
    320,
    Math.round(r.width * scale)
  );

  const height = Math.round(240 * scale);

  if (
    canvas.width !== width ||
    canvas.height !== height
  ) {
    canvas.width = width;
    canvas.height = height;
  }

  const c = canvas.getContext('2d');

  c.clearRect(0, 0, width, height);

  const padL = 35 * scale;
  const padR = 8 * scale;
  const padT = 15 * scale;
  const padB = 22 * scale;

  const graphH = height - padT - padB;
  const graphW = width - padL - padR;

  const all = samples.flatMap(
    x => x.filter(v => Number.isFinite(v))
  );

  const lo = Math.max(
    0,
    Math.min(2, ...all) - 0.3
  );

  const hi = Math.max(
    6.5,
    Math.max(7, ...all) + 0.3
  );

  c.font = `${11 * scale}px sans-serif`;
  c.textBaseline = 'middle';

  for (let i = 0; i < 5; i++) {
    const y = padT + graphH * i / 4;

    const val = hi - (hi - lo) * i / 4;

    c.strokeStyle = '#dbe7f1';
    c.beginPath();
    c.moveTo(padL, y);
    c.lineTo(width - padR, y);
    c.stroke();

    c.fillStyle = '#7790a6';
    c.fillText(
      val.toFixed(1),
      4 * scale,
      y
    );
  }

  const colors = [
    '#346db0',
    '#2a9774',
    '#b87439'
  ];

  colors.forEach((col, k) => {
    c.strokeStyle = col;
    c.lineWidth = 2.1 * scale;
    c.beginPath();

    let begun = false;

    samples.forEach((sample, i) => {
      const v = sample[k];

      if (
        typeof v !== 'number' ||
        !Number.isFinite(v)
      ) {
        begun = false;
        return;
      }

      const x = padL +
        (samples.length < 2
          ? 0
          : i / (samples.length - 1)
        ) * graphW;

      const y = padT +
        (hi - v) / (hi - lo) * graphH;

      if (!begun) {
        c.moveTo(x, y);
        begun = true;
      } else {
        c.lineTo(x, y);
      }
    });

    c.stroke();
  });

  c.fillStyle = '#8296aa';

  c.fillText(
    'mA',
    4 * scale,
    10 * scale
  );
}

// =====================================================
// SENSOR TABLE
// =====================================================

function makeCell(text, cls) {
  const td = document.createElement('td');

  td.textContent = text;

  if (cls) {
    td.className = cls;
  }

  return td;
}

function tableRow(
  name,
  adc,
  ma,
  v,
  raw,
  quality
) {
  const tr = document.createElement('tr');

  const parts = [
    name,
    adc ? 'OK' : 'FAULT',
    fmt(ma, 3) + ' mA',
    fmt(v, 4) + ' V',
    String(raw),
    quality ? 'NOMINAL' : 'CHECK LOOP'
  ];

  parts.forEach((x, i) => {
    tr.appendChild(
      makeCell(
        x,
        i === 5
          ? (quality ? 'good' : 'bad')
          : ''
      )
    );
  });

  return tr;
}

// =====================================================
// DASHBOARD STATUS
// =====================================================

// ================================================
// SENSOR THRESHOLD STATUS
// ================================================

function alarmInfo(sensor) {
  const alarm = sensor?.alarm;

  switch (alarm) {
    case 1:
      return { text: 'NORMAL', cls: 'normal' };
    case 2:
      return { text: 'WARNING', cls: 'warning' };
    case 3:
      return { text: 'CRITICAL', cls: 'critical' };
    case 4:
      return { text: 'OUT OF RANGE', cls: 'error' };
    default:
      return { text: 'SENSOR ERROR', cls: 'error' };
  }
}

function updateSensorAlarm(name, sensor, unit) {
  const info = alarmInfo(sensor);
  const value = sensor?.scaled;

  const valid =
    [1, 2, 3].includes(sensor?.alarm) &&
    typeof value === 'number' &&
    Number.isFinite(value);

  const badge = $(`${name}-alarm`);
  const summary = $(`${name}-status-summary`);
  const detail = $(`${name}-status-detail`);

  if (badge) {
    badge.textContent = info.text;
    badge.className = `sensor-alarm ${info.cls}`;
  }

  if (summary) {
    summary.textContent = info.text;
    summary.className = info.cls;
  }

  if (detail) {
    detail.textContent = valid
      ? `${fmt(value, 2)} ${unit} • ${fmt(sensor.ma, 3)} mA`
      : 'Pembacaan tidak valid';
  }
}
function presentStatus(s) {
csrf = s.csrf;

  // ================================================
  // DEVICE CONNECTION
  // ================================================

  $('device-status').textContent =
    s.mqtt_connected
      ? 'Online · MQTT aktif'
      : s.sta_online
        ? 'Wi-Fi aktif · MQTT offline'
        : 'AP mode · Wi-Fi STA offline';

  // ================================================
  // SCALED VALUES
  // ================================================

  const validValue = sensor =>
    [1, 2, 3].includes(sensor.alarm) &&
    typeof sensor.scaled === 'number' &&
    Number.isFinite(sensor.scaled);

  $('temp-val').textContent =
    validValue(s.temp)
      ? `${fmt(s.temp.scaled, 2)} °C`
      : 'CHECK SENSOR';

  $('level-val').textContent =
    validValue(s.level)
      ? `${fmt(s.level.scaled, 2)} %`
      : 'CHECK SENSOR';

  $('pressure-val').textContent =
    validValue(s.pressure)
      ? `${fmt(s.pressure.scaled, 3)} bar`
      : 'CHECK SENSOR';

  $('ewig-val').textContent =
    s.ewig.modbus_ok
      ? 'MODBUS OK'
      : 'OFFLINE';

  // ================================================
  // SENSOR QUALITY
  // ================================================

  $('temp-qual').textContent =
    validValue(s.temp)
      ? `${fmt(s.temp.ma, 3)} mA • scaling pengguna`
      : 'Periksa sensor suhu';

  $('level-qual').textContent =
    validValue(s.level)
      ? `${fmt(s.level.ma,3)} mA • ${fmt(s.tank?.level_mm,1)} mm • ${fmt(s.tank?.volume_l,3)} L • ${s.tank?.two_point_enabled ? 'referensi 2 titik' : 'skala nominal 0–5m (belum kalibrasi)'}`
      : 'Periksa sensor level';

  $('pressure-qual').textContent =
    validValue(s.pressure)
      ? `${fmt(s.pressure.ma, 3)} mA • belum kalibrasi referensi`
      : 'Periksa sensor tekanan';
  // Selected sensor uses virtual mA in dashboard, original ADC is still read.
  const test = s.sensor_test || {};
  const selected = test.active ? Number(test.channel) : -1;
  const cards = [
    ['temp-qual','temp-alarm','SUHU'],
    ['level-qual','level-alarm','LEVEL'],
    ['pressure-qual','pressure-alarm','PRESSURE']
  ];
  if (selected >= 0 && selected < cards.length) {
    const [qualId] = cards[selected];
    const qual = $(qualId);
    if (qual) qual.textContent = `BENCH SIMULASI ${fmt(test.ma,1)} mA; fisik ${fmt(test.real_ma,3)} mA • sisa ${test.remaining_s} dtk`;
  }

  // ================================================
  // THRESHOLD INDICATORS
  // ================================================

  updateSensorAlarm('temp', s.temp, '°C');
  updateSensorAlarm('level', s.level, '%');
  updateSensorAlarm('pressure', s.pressure, 'bar');
  if (selected >= 0 && selected < cards.length) {
    const badge = $(cards[selected][1]);
    if (badge) badge.textContent = 'SIMULASI · ' + badge.textContent;
  }

  // ================================================
  // NETWORK STATUS
  // ================================================

  $('wifi-state').textContent =
    s.sta_online ? 'Terhubung' : 'Belum terhubung';

  $('sta-ip').textContent = s.sta_ip || '—';

  $('mqtt-state').textContent =
    s.mqtt_connected
      ? 'CONNECTED (TLS)'
      : 'Belum terhubung';

  $('ap-ssid').textContent = s.ap_ssid || '—';

  // ================================================
  // SENSOR DIAGNOSTIC TABLE
  // ================================================

  const tbody = $('sensor-table');

  tbody.replaceChildren();

  const addSensorRow = (name, sensor, unit) => {
    const tr = document.createElement('tr');
    const info = alarmInfo(sensor);

    const measurementValid =
      [1, 2, 3].includes(sensor.alarm) &&
      typeof sensor.scaled === 'number' &&
      Number.isFinite(sensor.scaled);

    const parts = [
      name,
      sensor.adc_ok ? 'OK' : 'FAULT',
      `${fmt(sensor.ma, 3)} mA`,
      `${fmt(sensor.voltage, 4)} V`,
      String(sensor.raw),
      measurementValid
        ? `${fmt(sensor.scaled, 2)} ${unit}`
        : '—',
      info.text
    ];

    parts.forEach((value, i) => {
      const td = makeCell(
        value,
        i === 6 ? info.cls : ''
      );

      tr.appendChild(td);
    });

    tbody.appendChild(tr);
  };

  addSensorRow('Suhu A0', s.temp, '°C');
  addSensorRow('Level A2', s.level, '%');
  addSensorRow('Pressure A3', s.pressure, 'bar');

  // EWIG remains unvalidated.
  const ew = document.createElement('tr');

  [
    'EWIG RS485',
    s.ewig.modbus_ok ? 'CRC OK' : 'NO RESPONSE',
    '—',
    '—',
    `R09:${s.ewig.raw_r09} / R10:${s.ewig.raw_r10}`,
    '—',
    'BELUM VALIDASI'
  ].forEach(value => {
    ew.appendChild(makeCell(value));
  });

  tbody.appendChild(ew);

  // ================================================
  // RAW CURRENT TREND
  // ================================================

  samples.push([
    s.temp.loop_ok ? s.temp.ma : null,
    s.level.loop_ok ? s.level.ma : null,
    s.pressure.loop_ok ? s.pressure.ma : null
  ]);

  if (samples.length > 75) {
    samples.shift();
  }

  drawTrend();
}


// Tahap 2: API monitoring genset. Never substitute zero for unavailable data.
function presentGensetFields(s) {
  const g = s && s.genset ? s.genset : {};
  const synthetic = !!g.simulation_mode;
  const existingMarker = document.getElementById('genset-source-label');
  if (existingMarker) existingMarker.textContent = synthetic
    ? 'DATA GENSET SIMULASI (BUKAN PENGUKURAN AKTUAL)'
    : 'PARAMETER GENSET BELUM TERSEDIA — SENSOR FISIK TETAP REAL';
  const pairs = [
    ['engine_state', 'extra-engine-state', ''],
    ['rpm_calc', 'extra-rpm', 'RPM'],
    ['battery_v', 'extra-battery', 'V'],
    ['freq_hz', 'extra-frequency', 'Hz'],
    ['load_pct', 'extra-load', '%'],
    ['v_l1', 'extra-v-l1', 'V'],
    ['v_l2', 'extra-v-l2', 'V'],
    ['v_l3', 'extra-v-l3', 'V'],
    ['i_l1', 'extra-i-l1', 'A'],
    ['i_l2', 'extra-i-l2', 'A'],
    ['i_l3', 'extra-i-l3', 'A'],
    ['p_kw', 'extra-p-kw', 'kW'],
    ['s_kva', 'extra-s-kva', 'kVA'],
    ['pf', 'extra-pf', ''],
    ['run_hours', 'extra-run-hours', 'jam'],
    ['maint_due_h', 'extra-maint-due', 'jam'],
    ['service_status', 'extra-service', ''],
    ['fuel_est_l', 'extra-fuel-est', 'L']
  ];
  for (const [key, id, unit] of pairs) {
    const node = document.getElementById(id);
    if (!node) continue;
    const value = g[key];
    const valid = (typeof value === 'number' && Number.isFinite(value)) ||
                  (typeof value === 'string' && value.length > 0);
    node.textContent = valid ? `${value}${unit ? ' ' + unit : ''}`
                             : `—${unit ? ' ' + unit : ''}`;
  }
}

// =====================================================
// DASHBOARD POLLING
// =====================================================

async function polling() {
  try {
    const s = await api('/api/status');

    presentStatus(s);
    presentGensetFields(s);
    const sim = $('sim-status');
    if (sim) sim.textContent = s.genset && s.genset.simulation_mode
      ? `SIMULASI AKTIF — ${s.genset.engine_state} | ${Number(s.genset.run_hours || 0).toFixed(5)} jam (SIMULASI/NVS)`
      : `SIMULASI NONAKTIF — ${Number(s.genset?.run_hours || 0).toFixed(5)} jam simulasi tersimpan di NVS (bukan jam genset aktual).`;

  } catch (e) {
    $('device-status').textContent =
      'Koneksi HTTP terputus';

    console.warn(e.message);
  }
}

async function setupDashboard() {
  nav();

  window.addEventListener(
    'resize',
    drawTrend
  );

  document.querySelectorAll('[data-sim-state]').forEach(button => {
    button.addEventListener('click', async () => {
      try {
        await api('/api/sim/state', {
          method: 'POST',
          body: new URLSearchParams({state: button.dataset.simState}).toString()
        });
        await polling();
      } catch (err) { alert(err.message); }
    });
  });
  await polling();

  setInterval(polling, 2000);
}

// =====================================================
// SENSOR CONFIGURATION - CONSTANTS
// =====================================================

const SENSOR_NAMES = [
  'temperature',
  'level',
  'pressure'
];

const SENSOR_FIELDS = [
  'input_min_ma',
  'input_max_ma',
  'output_min',
  'output_max',
  'warning_low',
  'critical_low',
  'warning_high',
  'critical_high'
];

// =====================================================
// LOAD SENSOR CONFIGURATION INTO FORM
// =====================================================

async function loadSensorConfig() {
  const form = $('sensor-config-form');

  if (!form) return;

  msg(
    'sensor-config-msg',
    'Memuat konfigurasi sensor…'
  );

  const result = await api(
    '/api/sensors/config'
  );

  if (result.csrf) {
    csrf = result.csrf;
  }

  for (const sensor of SENSOR_NAMES) {
    const config = result[sensor];

    if (!config) {
      throw new Error(
        'Konfigurasi ' + sensor +
        ' tidak tersedia'
      );
    }

    for (const field of SENSOR_FIELDS) {
      const name = `${sensor}_${field}`;

      const input = form.elements.namedItem(
        name
      );

      if (!input) {
        throw new Error(
          'Input form tidak ditemukan: ' + name
        );
      }

      const value = config[field];

      if (
        typeof value !== 'number' ||
        !Number.isFinite(value)
      ) {
        throw new Error(
          'Nilai konfigurasi tidak valid: ' + name
        );
      }

      input.value = String(value);
    }
  }

  msg(
    'sensor-config-msg',
    'Konfigurasi sensor berhasil dimuat.'
  );
}

// =====================================================
// SENSOR CONFIGURATION - VALIDATION
// =====================================================

function validateSensorConfig(form) {
  for (const sensor of SENSOR_NAMES) {
    const c = {};

    for (const field of SENSOR_FIELDS) {
      const name = `${sensor}_${field}`;

      const input = form.elements.namedItem(
        name
      );

      if (
        !input ||
        input.value.trim() === ''
      ) {
        throw new Error(
          `Parameter ${name} wajib diisi.`
        );
      }

      const value = Number(input.value);

      if (!Number.isFinite(value)) {
        throw new Error(
          `Nilai ${name} harus berupa angka.`
        );
      }

      c[field] = value;
    }

    // Validate input current range.
    if (
      c.input_min_ma < 0 ||
      c.input_max_ma > 25 ||
      c.input_min_ma >= c.input_max_ma
    ) {
      throw new Error(
        `${sensor}: rentang input mA tidak valid.`
      );
    }

    // Validate physical measurement range.
    if (
      c.output_min >= c.output_max
    ) {
      throw new Error(
        `${sensor}: output minimum harus lebih kecil dari maksimum.`
      );
    }

    // Validate warning and critical thresholds.
    const orderValid =
      c.output_min <= c.critical_low &&
      c.critical_low < c.warning_low &&
      c.warning_low < c.warning_high &&
      c.warning_high < c.critical_high &&
      c.critical_high <= c.output_max;

    if (!orderValid) {
      throw new Error(
        `${sensor}: urutan threshold tidak valid. ` +
        `Gunakan Critical Low < Warning Low < ` +
        `Warning High < Critical High.`
      );
    }
  }

  return true;
}

// =====================================================
// SENSOR CONFIGURATION - SAVE
// =====================================================

function setupSensorConfigForm() {
  const form = $('sensor-config-form');

  if (!form) return;

  form.addEventListener(
    'submit',
    async ev => {
      ev.preventDefault();

      toggleBusy(form, true);

      msg(
        'sensor-config-msg',
        'Memvalidasi konfigurasi sensor…'
      );

      try {
        // Browser-side validation.
        validateSensorConfig(form);

        msg(
          'sensor-config-msg',
          'Menyimpan kalibrasi dan threshold ke ESP32-S3…'
        );

        // Submit all three sensors at once.
        const result = await api(
          '/api/sensors/config',
          {
            method: 'POST',
            body: formBody(form)
          }
        );

        msg(
          'sensor-config-msg',
          result.message ||
          'Konfigurasi sensor berhasil disimpan.'
        );

        // Read back the actual stored configuration.
        await loadSensorConfig();

        msg(
          'sensor-config-msg',
          'Berhasil! Kalibrasi dan threshold tersimpan di NVS.'
        );

      } catch (e) {
        msg(
          'sensor-config-msg',
          e.message,
          true
        );

      } finally {
        toggleBusy(form, false);
      }
    }
  );
}

// =====================================================
// SETTINGS PAGE
// =====================================================

async function setupSettings() {
  nav();

  const [status, config] = await Promise.all([
    api('/api/status'),
    api('/api/config')
  ]);

  csrf = status.csrf;

  // =================================================
  // AP INFORMATION
  // =================================================

  $('ap-info').textContent = status.ap_ssid;

  // =================================================
  // WIFI CONFIGURATION
  // =================================================

  $('ssid').value = config.ssid;

  $('ip-mode').value =
    config.static_ip
      ? 'static'
      : 'dhcp';

  [
    'ip',
    'gateway',
    'mask',
    'dns'
  ].forEach(id => {
    $(id).value = config[id] || '';
  });

  // =================================================
  // MQTT CONFIGURATION
  // =================================================

  const hostSelect = $('mqtt-host');

  if (
    config.mqtt_host &&
    !Array.from(hostSelect.options).some(
      o => o.value === config.mqtt_host
    )
  ) {
    const saved = new Option(
      'Server tersimpan — ' +
      config.mqtt_host,
      config.mqtt_host
    );

    hostSelect.add(saved);
  }

  hostSelect.value = config.mqtt_host;

  $('mqtt-port').value = config.mqtt_port;

  $('mqtt-interval').value =
    config.interval_s;

  $('mqtt2-enabled').value = config.mqtt2_enabled ? 'true' : 'false';
  $('mqtt2-host').value = config.mqtt2_host || '178.128.107.238';
  $('mqtt2-port').value = config.mqtt2_port || 1886;
  $('mqtt2-status').textContent = config.mqtt2_connected
    ? 'CONNECTED' : (config.mqtt2_enabled ? 'DISCONNECTED' : 'OFF');

  // =================================================
  // WIFI STATIC/DHCP UX
  // =================================================

  function syncIpFields() {
    $('manual-ip-fields').hidden =
      $('ip-mode').value !== 'static';
  }

  $('ip-mode').addEventListener(
    'change',
    syncIpFields
  );

  syncIpFields();

  // =================================================
  // WIFI CONNECTION RESULT
  // =================================================

  const resultPanel = $('wifi-result');

  if (
    status.sta_online &&
    status.sta_ip
  ) {
    $('wifi-result-ssid').textContent =
      config.ssid || '—';

    $('wifi-result-mode').textContent =
      config.static_ip
        ? 'Static IP'
        : 'DHCP / Otomatis';

    $('wifi-result-ap').textContent =
      status.ap_ip || '192.168.4.1';

    $('wifi-result-sta').textContent =
      status.sta_ip;

    resultPanel.hidden = false;

  } else {
    resultPanel.hidden = true;
  }

  // =================================================
  // EXISTING WIFI / MQTT / ADMIN FORMS
  // =================================================

  const wifi = $('wifi-form');
  const mqtt = $('mqtt-form');
  const admin = $('admin-form');

  function bindForm(
    form,
    endpoint,
    message
  ) {
    form.addEventListener(
      'submit',
      async ev => {
        ev.preventDefault();

        toggleBusy(form, true);

        msg(message, 'Menyimpan…');

        try {
          const result = await api(
            endpoint,
            {
              method: 'POST',
              body: formBody(form)
            }
          );

          msg(
            message,
            result.note || 'Berhasil.'
          );

          if (result.restart) {
            msg(
              message,
              'Tersimpan. ESP32 restart; ' +
              'sambungkan ulang bila alamat IP berubah. ' +
              'AP: 192.168.4.1'
            );

          } else if (
            endpoint.endsWith('/admin')
          ) {
            setTimeout(
              () => location.href = '/login',
              900
            );
          }

        } catch (e) {
          msg(message, e.message, true);

        } finally {
          toggleBusy(form, false);
        }
      }
    );
  }

  bindForm(
    wifi,
    '/api/config/wifi',
    'wifi-msg'
  );

  bindForm(
    mqtt,
    '/api/config/mqtt',
    'mqtt-msg'
  );

  bindForm(
    $('mqtt2-form'),
    '/api/config/mqtt2',
    'mqtt2-msg'
  );

  bindForm(
    admin,
    '/api/config/admin',
    'admin-msg'
  );

  // =================================================
  // HYBRID TANK DIMENSIONS — independent API + NVS, no restart.
  const tankForm = $('tank-config-form');
  if (tankForm) {
    try {
      const tank = await api('/api/tank/config');
      for (const name of ['length_cm','width_cm','height_cm',
                          'zero_ma','reference_ma','reference_depth_mm']) {
        tankForm.elements.namedItem('tank_' + name).value = tank[name];
      }
      tankForm.elements.namedItem('tank_calibration_enabled').value =
        tank.calibration_enabled ? '1' : '0';
      msg('tank-config-msg','Dimensi tersimpan dimuat. Kalibrasi referensi tidak wajib diaktifkan.');
    } catch(e) { msg('tank-config-msg',e.message,true); }
    tankForm.addEventListener('submit',async ev => {
      ev.preventDefault();
      toggleBusy(tankForm,true);
      try {
        const result = await api('/api/tank/config',{
          method:'POST',body:formBody(tankForm)
        });
        msg('tank-config-msg', result.note || 'Tersimpan di NVS.');
      } catch(e) { msg('tank-config-msg',e.message,true); }
      finally {toggleBusy(tankForm,false);}
    });
  }

  // BENCH ONLY: software current override; one of A0/A2/A3, timeout 120s.
  const testStart = $('sensor-test-start');
  const testStop = $('sensor-test-stop');
  const sensorEl = $('sensor-test-channel');
  const currentEl = $('sensor-test-ma');
  const testAction = async action => {
    try {
      if (testStart) testStart.disabled = true;
      if (testStop) testStop.disabled = true;
      const val = Number(currentEl?.value);
      if (action === 'START' && (!Number.isFinite(val) || val < 0 || val > 24 || !currentEl?.value.trim()))
        throw new Error('Masukkan nilai arus simulasi 0 sampai 24 mA.');
      const result = await api('/api/bench/sensor-test', {
        method:'POST',
        body:new URLSearchParams({action, sensor:sensorEl?.value || 'pressure', ma:String(val)}).toString()
      });
      msg('sensor-test-msg',result.active
        ? `SIMULASI AKTIF: ${sensorEl.options[sensorEl.selectedIndex].text} = ${val} mA. Maksimum 120 detik. Periksa dashboard dan Mastify.`
        : 'STOP diterima: pembacaan real kembali pada siklus telemetri selanjutnya.');
    } catch(e) { msg('sensor-test-msg',e.message,true); }
    finally {if(testStart)testStart.disabled=false; if(testStop)testStop.disabled=false;}
  };
  if(testStart) testStart.addEventListener('click',()=>testAction('START'));
  if(testStop) testStop.addEventListener('click',()=>testAction('STOP'));

  // NEW SENSOR CALIBRATION & THRESHOLD
  // =================================================

  setupSensorConfigForm();

  try {
    await loadSensorConfig();

  } catch (e) {
    console.error(
      'Sensor configuration load failed:',
      e
    );

    msg(
      'sensor-config-msg',
      'Gagal memuat konfigurasi sensor: ' +
      e.message,
      true
    );
  }
}

// =====================================================
// APPLICATION INITIALIZATION
// =====================================================

if (page === 'login') {
  setupLogin();
}

if (page === 'dashboard') {
  setupDashboard();
}

if (page === 'settings') {
  setupSettings().catch(e => {
    console.error(e);

    msg(
      'wifi-msg',
      e.message,
      true
    );
  });
}