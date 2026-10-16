/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 GrakovNe
 */
const fs = require("fs");
const vm = require("vm");
const path = require("path");
const WEB = path.join(__dirname, "..", "..", "web");

function mockClassList(el) {
  const has = c => el.className.split(/\s+/).includes(c);
  return {
    add: c => { if (!has(c)) el.className = (el.className + " " + c).trim(); },
    remove: c => { el.className = el.className.split(/\s+/).filter(x => x && x !== c).join(" "); },
    toggle: (c, on) => { const want = on === undefined ? !has(c) : on; if (want) this.add(c); else this.remove(c); },
    contains: has,
  };
}

function mockEl() {
  const el = {
    textContent: "", className: "", value: "", checked: false, disabled: false,
    innerHTML: "", style: {}, dataset: {}, type: "text", href: "",
    children: [],
    appendChild(c) { this.children.push(c); },
    addEventListener(name, fn) { (this.listeners ||= {})[name] = fn; },
    setAttribute(k, v) { (this.attrs ||= {})[k] = v; },
    getBoundingClientRect() { return { width: 600, height: 190, left: 0, top: 0 }; },
    querySelectorAll() { return []; },
    querySelector() { return null; },
  };
  const cl = mockClassList(el);
  cl.toggle = (c, on) => { const want = on === undefined ? !cl.contains(c) : on; if (want) cl.add(c); else cl.remove(c); };
  el.classList = cl;
  Object.defineProperty(el, "firstChild", {
    get() { return this._first ||= mockEl(); },
  });
  return el;
}

function runPage(file, apiData) {
  const html = fs.readFileSync(file, "utf8");
  const js = html.match(/<script>([\s\S]*?)<\/script>/)[1];
  const els = {};
  const fetchCalls = [];
  const timers = [];
  const doc = {
    getElementById: (id) => (els[id] ||= mockEl()),
    createElement: () => mockEl(),
    querySelectorAll: () => [],
    body: mockEl(),
    title: "",
  };
  const ctx = {
    document: doc,
    window: { addEventListener() {} },
    location: { origin: "http://airmonitor-dd94.local", href: "", reload() {} },
    fetch: (url, opts) => {
      fetchCalls.push(url + (opts && opts.method ? " " + opts.method : ""));
      const key = url.split("?")[0];
      if (apiData[key] === undefined) {
        return Promise.reject(new Error("network down"));
      }
      return Promise.resolve({ ok: true, json: async () => apiData[key] });
    },
    setInterval: (fn, ms) => { timers.push(fn); return 0; },
    clearInterval: () => {},
    setTimeout: (fn, ms) => { return 0; },
    console, Date, Math, JSON, String, Number, Array, Promise, Set,
  };
  vm.createContext(ctx);
  vm.runInContext(js, ctx, { filename: file });
  return new Promise((resolve) => setImmediate(() => setImmediate(() => setImmediate(() =>
    resolve({ els, fetchCalls, timers, ctx, doc })))));
}

let fails = 0;
function check(cond, name) {
  if (cond) console.log("PASS " + name);
  else { fails++; console.log("FAIL " + name); }
}

(async () => {
  const now = Math.floor(Date.now() / 1000);
  const status = {
    time: { timestamp: now, synced: true, uptime_s: 5231 },
    device: {
      name: "Salon", id: "dd94", hostname: "salon", chip: "ESP32-S3", revision: "v0.2", cores: 2, cpu_mhz: 160,
      firmware: "abc1234", idf: "v5.5.5", build_date: "Oct 16 2026", mac: "7c:4f:ad:bb:dd:94",
      flash_bytes: 16777216, psram_bytes: 8388608, heap_free: 8123456, heap_min_free: 8000000,
      reset_reason: "power_on", chip_temperature_c: 41.5,
    },
    mode: "normal",
    sensor: { connected: true, warming_up: false, co2_ppm: 612, co2_age_s: 2, temperature_c: 23.98,
              temperature_age_s: 1, frames_valid: 18423, frames_bad: 0 },
    wifi: { connected: true, ssid: "Home", bssid: "aa:bb:cc:dd:ee:ff", rssi: -58, channel: 6,
            ip: "192.168.1.42", gateway: "192.168.1.1", last_disconnect: "other" },
    mqtt: { enabled: true, connected: true, last_publish: now - 10, errors: 0 },
  };

  const cur = {
    timestamp: now, time_synced: true, uptime_s: 5231,
    sensor: { connected: true, warming_up: false },
    co2: { ppm: 612, age_s: 2 },
    temperature: { celsius: 23.98, age_s: 1 },
  };
  const hist = {
    step_s: 600,
    records: [
      { t: now - 300, co2: 598, temperature: 23.91 },
      { t: now - 3600, co2: 1200, temperature: 23.95 },
      { t: now - 10000, co2: null, temperature: 17.5 },
    ],
  };
  const m = await runPage(path.join(WEB, "main.html"), {
    "/api/v1/current": cur, "/api/v1/history": hist, "/api/v1/status": status,
  });
  check(m.els.co2.textContent === "612", "main: co2 number");
  check(m.els.co2fill.className === "fill good", "main: co2 zone green");
  check(m.els.co2d.innerHTML.includes("OK"), "main: co2 word OK");
  check(m.els.temp.textContent === "24.0", "main: temp number one decimal");
  check(m.els.tempfill.className === "fill good", "main: temp zone green");
  check(m.els.upd.textContent.startsWith("Updated "), "main: updated line");
  check(m.els.fw.textContent.includes("abc1234"), "main: firmware footer");
  check((m.els["chart-co2"].innerHTML.match(/<rect/g) || []).length === 2, "main: 2 co2 bars");
  check(m.els["chart-co2"].innerHTML.includes("bar good"), "main: good bar colored");
  check(m.els["chart-co2"].innerHTML.includes("bar warn"), "main: warn bar colored");
  check((m.els["chart-temp"].innerHTML.match(/<rect/g) || []).length === 3, "main: 3 temp bars");
  check(m.els["chart-temp"].innerHTML.includes("bar cold"), "main: cold bar colored");
  check(m.els["stats-co2"].innerHTML.includes("max <b>1200"), "main: co2 max stat");
  check(m.doc.title === "Salon", "main: title is the device name");

  const m2 = await runPage(path.join(WEB, "main.html"), {});
  check(m2.els.upd.textContent === "Device not reachable", "main: unreachable footer");
  check(m2.els.upd.className === "err", "main: unreachable red");

  const cur2 = { ...cur, time_synced: false, timestamp: null,
                 sensor: { connected: true, warming_up: true }, co2: null };
  const m3 = await runPage(path.join(WEB, "main.html"), {
    "/api/v1/current": cur2, "/api/v1/history": hist, "/api/v1/status": status,
  });
  check(m3.els.co2d.innerHTML.includes("Warming up"), "main: warming up line");
  check(m3.els.co2.textContent === "—", "main: co2 null dash");
  check(m3.els.co2fill.style.width === "0", "main: empty track");
  check(m3.els.upd.textContent.includes("clock not synced"), "main: clock line");

  const cur3 = { ...cur, sensor: { connected: false, warming_up: false }, co2: null, temperature: null };
  const m4 = await runPage(path.join(WEB, "main.html"), {
    "/api/v1/current": cur3, "/api/v1/history": hist, "/api/v1/status": status,
  });
  check(m4.els.co2d.className === "d err", "main: unplugged is red");
  check(m4.els.tempd.innerHTML.includes("sensor not connected"), "main: unplugged text");

  const cfg = {
    device: { name: "Salon" },
    wifi: { ssid: "Home", password_set: true },
    time: { ntp_server: "pool.ntp.org" },
    mqtt: {
      enabled: true, host: "mqtt.test", port: 1884, username: "u",
      password_set: true, tls: false, ca_cert_set: true, skip_verify: false,
      co2_topic: "airmonitor/dd94/co2",
      temperature_topic: "airmonitor/dd94/temperature",
      interval_s: 45, discovery: true,
    },
  };
  const i = await runPage(path.join(WEB, "info.html"), { "/api/v1/status": status, "/api/v1/config": cfg });
  check(i.els["s-sensor"].textContent === "Operating", "info: sensor state");
  check(i.els["s-sensor"].className === "state ok", "info: sensor green");
  check(i.els["v-co2"].textContent === "612 ppm · 2 s ago", "info: co2 row");
  check(i.els["v-rssi"].textContent === "-58 dBm", "info: rssi row");
  check(i.els["v-ip"].textContent === "192.168.1.42", "info: ip row");
  check(i.els["v-host"].textContent === "salon.local", "info: hostname row");
  check(i.els["v-broker"].textContent === "mqtt.test : 1884", "info: broker from config");
  check(i.els["v-pub"].textContent === "10 s ago", "info: last publish");
  check(i.els["v-ntp"].textContent === "pool.ntp.org", "info: ntp from config");
  check(i.els["v-up"].textContent === "1 h 27 min", "info: uptime");
  check(i.els["v-flash"].textContent === "16 MB", "info: flash size");
  check(i.els["v-heap"].textContent === "7.7 MB · lowest 7.6 MB", "info: heap");
  check(i.els["v-reset"].textContent === "Power on", "info: reset reason");
  check(i.els["v-fw"].textContent === "abc1234", "info: firmware");

  const st2 = { ...status, sensor: { ...status.sensor, connected: false, co2_ppm: null, co2_age_s: null,
                temperature_c: null, temperature_age_s: null },
                mqtt: { enabled: false, connected: false, last_publish: null, errors: 0 },
                time: { ...status.time, synced: false, timestamp: null } };
  const i2 = await runPage(path.join(WEB, "info.html"), { "/api/v1/status": st2, "/api/v1/config": cfg });
  check(i2.els["s-sensor"].className === "state err", "info: unplugged red");
  check(i2.els["v-co2"].textContent === "—", "info: co2 dash");
  check(i2.els["s-mqtt"].textContent === "Off", "info: mqtt off");
  check(i2.els["s-mqtt"].className === "state", "info: mqtt off is neutral");
  check(i2.els["s-time"].textContent === "Not synced", "info: time not synced");
  check(i2.els["v-now"].textContent === "—", "info: no device time");

  const i3 = await runPage(path.join(WEB, "info.html"), {});
  check(i3.els.upd.textContent === "Device not reachable", "info: unreachable footer");

  const s = await runPage(path.join(WEB, "setup.html"), {
    "/api/v1/status": status,
    "/api/v1/config": cfg,
    "/api/v1/wifi/scan": { scanning: false, networks: [{ ssid: "Home", rssi: -50, secure: true },
                                                       { ssid: "Guest", rssi: -70, secure: false }] },
  });
  check(s.els.name.value === "Salon", "setup: device name");
  check(s.els.hostprev.textContent === "http://salon.local/", "setup: hostname preview");
  check(s.els.passfor.textContent === "Home", "setup: password for the saved ssid");
  check(s.els.passhint.textContent.includes("leave empty"), "setup: pass hint");
  check(s.els.ntp.value === "pool.ntp.org", "setup: ntp");
  check(s.els.mq_en.checked === true, "setup: mqtt enabled");
  check(s.els.mq_host.value === "mqtt.test", "setup: mqtt host");
  check(s.els.mq_port.value === 1884, "setup: mqtt port");
  check(s.els.mq_int.value === 45, "setup: interval");
  check(s.els.mq_disc.checked === true, "setup: discovery");
  check(s.els.mq_skip.disabled === true, "setup: skip disabled without TLS");
  check(s.els.cawrap.style.display === "none", "setup: CA hidden without TLS");
  check(s.els.mqcahint.textContent.includes("saved"), "setup: ca hint");
  check(s.els.nets.children.length === 2, "setup: scan list rendered");
  check(s.els.nets.children[0].className === "net sel", "setup: saved network selected");
  check(s.els.h1 === undefined, "setup: title untouched on LAN");
  check(s.els.nav === undefined, "setup: nav untouched on LAN");
  check(s.els.fw.textContent.includes("abc1234"), "setup: firmware footer");

  s.els.nets.children[1].onclick();
  check(s.els.passwrap.style.display === "none", "setup: open network hides password");
  check(s.els.savebar.className.includes("show"), "setup: save bar appears");
  check(s.els.nchanges.textContent === "1 change", "setup: one change counted");
  await s.els.save.onclick();
  const post = s.fetchCalls.find(c => c.startsWith("/api/v1/config POST"));
  check(!!post, "setup: config POSTed");
  check(s.els.rtitle.textContent === "Restarting", "setup: restart screen after save");

  const s2 = await runPage(path.join(WEB, "setup.html"), {
    "/api/v1/status": status, "/api/v1/config": { ...cfg, mqtt: { ...cfg.mqtt, port: 1883 } },
    "/api/v1/wifi/scan": { scanning: false, networks: [] },
  });
  s2.els.mq_port.value = String(s2.els.mq_port.value); /* a real input.value is a string */
  s2.els.mq_tls.checked = true;
  s2.els.mq_tls.listeners.change();
  check(s2.els.mq_port.value === "8883", "setup: TLS flips the port");
  check(s2.els.cawrap.style.display === "", "setup: TLS reveals CA");
  check(s2.els.mq_skip.disabled === false, "setup: TLS enables skip");
  check(s2.els.nets.children.length === 1, "setup: saved ssid listed when not scanned");

  const s3 = await runPage(path.join(WEB, "setup.html"), {
    "/api/v1/status": { ...status, mode: "provisioning" },
    "/api/v1/config": { ...cfg, wifi: { ssid: "", password_set: false }, mqtt: { ...cfg.mqtt, enabled: false } },
    "/api/v1/wifi/scan": { scanning: false, networks: [] },
  });
  check(s3.els.h1.textContent === "Set up Air Monitor", "setup: first-run title");
  check(s3.els.nav.style.display === "none", "setup: first-run hides nav");
  check(s3.els.mq_disc.disabled === true, "setup: MQTT off disables discovery");
  check(s3.els.mqbody.className.includes("dim"), "setup: MQTT off dims fields");

  console.log(fails === 0 ? "ALL OK" : fails + " FAILURES");
  process.exit(fails ? 1 : 0);
})();
