// ==========================================================================
// Aurora — interface de voz (celular) para o assistente no ESP32
//   • Reconhecimento de fala e síntese de voz: Web Speech API (no celular)
//   • Transporte: Web Bluetooth (BLE) com Nordic UART Service
//   • Protocolo: JSON, uma mensagem por linha ('\n')
// ==========================================================================

const NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
const NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"; // celular -> ESP32 (write)
const NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"; // ESP32 -> celular (notify)
const DEVICE_PREFIX = "ESP32-Assist";
const WRITE_CHUNK = 180;
const REPLY_TIMEOUT_MS = 75_000;

const $ = (id) => document.getElementById(id);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// --------------------------------------------------------------------------
// Preferências locais
// --------------------------------------------------------------------------
const prefs = Object.assign(
  { voiceURI: "", rate: 1.05, speak: true, continuous: false },
  JSON.parse(localStorage.getItem("aurora.prefs") || "{}"),
);
const savePrefs = () => localStorage.setItem("aurora.prefs", JSON.stringify(prefs));

// --------------------------------------------------------------------------
// Link BLE
// --------------------------------------------------------------------------
class BleLink extends EventTarget {
  device = null;
  rx = null;
  buffer = new Uint8Array(0);
  writeChain = Promise.resolve();
  manualDisconnect = false;
  encoder = new TextEncoder();
  decoder = new TextDecoder();

  get connected() {
    return !!(this.device?.gatt?.connected && this.rx);
  }

  async connect() {
    this.device = await navigator.bluetooth.requestDevice({
      filters: [{ namePrefix: DEVICE_PREFIX }, { services: [NUS_SERVICE] }],
      optionalServices: [NUS_SERVICE],
    });
    this.device.addEventListener("gattserverdisconnected", () => this.#onDisconnected());
    this.manualDisconnect = false;
    await this.#setup();
  }

  async #setup() {
    const server = await this.device.gatt.connect();
    const service = await server.getPrimaryService(NUS_SERVICE);
    const rx = await service.getCharacteristic(NUS_RX);
    const tx = await service.getCharacteristic(NUS_TX);
    tx.addEventListener("characteristicvaluechanged", (e) => this.#onData(e.target.value));
    await tx.startNotifications();
    this.buffer = new Uint8Array(0);
    this.writeChain = Promise.resolve();
    this.rx = rx;
    this.dispatchEvent(new Event("connected"));
  }

  #onData(view) {
    const chunk = new Uint8Array(view.buffer, view.byteOffset, view.byteLength);
    const merged = new Uint8Array(this.buffer.length + chunk.length);
    merged.set(this.buffer);
    merged.set(chunk, this.buffer.length);
    this.buffer = merged;

    let nl;
    while ((nl = this.buffer.indexOf(10)) >= 0) {
      const line = this.decoder.decode(this.buffer.slice(0, nl)).trim();
      this.buffer = this.buffer.slice(nl + 1);
      if (!line) continue;
      try {
        this.dispatchEvent(new CustomEvent("message", { detail: JSON.parse(line) }));
      } catch {
        console.warn("Linha inválida do ESP32:", line);
      }
    }
  }

  send(obj) {
    const bytes = this.encoder.encode(JSON.stringify(obj) + "\n");
    this.writeChain = this.writeChain
      .catch(() => {})
      .then(async () => {
        if (!this.connected) throw new Error("Não conectado");
        for (let i = 0; i < bytes.length; i += WRITE_CHUNK) {
          const part = bytes.slice(i, i + WRITE_CHUNK);
          if (this.rx.writeValueWithResponse) await this.rx.writeValueWithResponse(part);
          else await this.rx.writeValue(part);
        }
      });
    return this.writeChain;
  }

  async #onDisconnected() {
    this.rx = null;
    this.dispatchEvent(new Event("disconnected"));
    if (this.manualDisconnect || !this.device) return;
    // Tenta reconectar automaticamente (não precisa de novo toque do usuário)
    for (let attempt = 1; attempt <= 6; attempt++) {
      this.dispatchEvent(new CustomEvent("reconnecting", { detail: attempt }));
      await sleep(Math.min(1000 * 2 ** (attempt - 1), 10_000));
      if (this.manualDisconnect) return;
      try {
        await this.#setup();
        return;
      } catch (e) {
        console.warn("Reconexão falhou:", e);
      }
    }
    this.dispatchEvent(new Event("gaveup"));
  }

  disconnect() {
    this.manualDisconnect = true;
    this.device?.gatt?.disconnect();
  }
}

// --------------------------------------------------------------------------
// Voz (reconhecimento + síntese)
// --------------------------------------------------------------------------
function cleanForSpeech(text) {
  return text
    .replace(/https?:\/\/\S+/g, "link")
    .replace(/[*_#`>~|]/g, "")
    .replace(/\p{Extended_Pictographic}/gu, "")
    .replace(/\s+/g, " ")
    .trim();
}

function splitSentences(text, max = 220) {
  const pieces = text.match(/[^.!?…;:]+[.!?…;:]*\s*/g) || [text];
  const out = [];
  let cur = "";
  for (const p of pieces) {
    if ((cur + p).length > max && cur) {
      out.push(cur.trim());
      cur = "";
    }
    cur += p;
  }
  if (cur.trim()) out.push(cur.trim());
  return out;
}

class Voice {
  SR = window.SpeechRecognition || window.webkitSpeechRecognition;
  synth = window.speechSynthesis;
  rec = null;
  token = 0;
  voices = [];

  constructor() {
    if (!this.synth) return;
    const load = () => {
      this.voices = this.synth.getVoices();
      document.dispatchEvent(new Event("voices"));
    };
    load();
    this.synth.addEventListener?.("voiceschanged", load);
  }

  get canListen() {
    return !!this.SR;
  }

  ptVoices() {
    const pt = this.voices.filter((v) => /^pt(-|_|$)/i.test(v.lang));
    const score = (v) => (/pt[-_]BR/i.test(v.lang) ? 2 : 0) + (/google|natural|online|neural/i.test(v.name) ? 1 : 0);
    return pt.sort((a, b) => score(b) - score(a));
  }

  pickVoice(uri) {
    return this.voices.find((v) => v.voiceURI === uri) || this.ptVoices()[0] || null;
  }

  listen(onInterim) {
    return new Promise((resolve, reject) => {
      const rec = new this.SR();
      rec.lang = "pt-BR";
      rec.interimResults = true;
      rec.continuous = false;
      rec.maxAlternatives = 1;
      let finalText = "";
      let failed = false;
      rec.onresult = (e) => {
        let interim = "";
        for (let i = e.resultIndex; i < e.results.length; i++) {
          const r = e.results[i];
          if (r.isFinal) finalText += r[0].transcript;
          else interim += r[0].transcript;
        }
        onInterim?.((finalText + " " + interim).trim());
      };
      rec.onerror = (e) => {
        if (e.error === "no-speech" || e.error === "aborted") return;
        failed = true;
        reject(new Error(e.error));
      };
      rec.onend = () => {
        this.rec = null;
        if (!failed) resolve(finalText.trim() || null);
      };
      this.rec = rec;
      rec.start();
    });
  }

  stopListening() {
    this.rec?.stop();
  }

  abortListening() {
    this.rec?.abort();
  }

  /** Fala o texto. Resolve true se terminou normalmente, false se interrompido. */
  speak(text, { voiceURI, rate } = {}) {
    this.stopSpeaking();
    if (!this.synth) return Promise.resolve(true);
    const parts = splitSentences(cleanForSpeech(text));
    const voice = this.pickVoice(voiceURI);
    const token = ++this.token;

    return new Promise((resolve) => {
      let i = 0;
      const next = () => {
        if (token !== this.token) return resolve(false);
        if (i >= parts.length) return resolve(true);
        const u = new SpeechSynthesisUtterance(parts[i++]);
        u.lang = voice?.lang || "pt-BR";
        if (voice) u.voice = voice;
        u.rate = rate || 1;
        // Rede de segurança: alguns Androids não disparam 'onend'
        const guard = setTimeout(next, 4000 + (u.text.length * 95) / u.rate);
        const done = () => {
          clearTimeout(guard);
          next();
        };
        u.onend = done;
        u.onerror = done;
        this.synth.speak(u);
      };
      next();
    });
  }

  stopSpeaking() {
    this.token++;
    this.synth?.cancel();
  }
}

// --------------------------------------------------------------------------
// UI
// --------------------------------------------------------------------------
const ble = new BleLink();
const voice = new Voice();

const CAPTIONS = {
  disconnected: "Toque para conectar ao ESP32",
  idle: "Toque para falar",
  listening: "Ouvindo…",
  thinking: "Pensando…",
  speaking: "Falando… toque para interromper",
};
const ORB_LABELS = {
  disconnected: "Conectar ao ESP32",
  idle: "Falar",
  listening: "Parar de ouvir",
  thinking: "Aguardando resposta",
  speaking: "Interromper fala",
};
const TOOL_LABELS = {
  set_led: "💡 Controlando o LED",
  blink_led: "✨ Piscando o LED",
  set_timer: "⏰ Lembrete criado",
  list_timers: "📋 Consultando lembretes",
  cancel_timers: "🗑️ Cancelando lembretes",
  get_device_status: "📟 Lendo status do ESP32",
};

let state = "disconnected";
let replyTimer = null;
let lastStatus = null;
let wakeLock = null;

function setState(s, caption) {
  state = s;
  document.body.dataset.state = s;
  $("caption").textContent = caption ?? CAPTIONS[s];
  $("orb").setAttribute("aria-label", ORB_LABELS[s]);
}

function toast(text, { error = false, ms = 2600 } = {}) {
  const el = $("toast");
  el.textContent = text;
  el.classList.toggle("error", error);
  el.hidden = false;
  el.style.animation = "none";
  void el.offsetWidth;
  el.style.animation = "";
  clearTimeout(toast.t);
  toast.t = setTimeout(() => (el.hidden = true), ms);
}

function showBanner(text, actionLabel = "Configurar") {
  $("banner-text").textContent = text;
  $("banner-action").textContent = actionLabel;
  $("banner").hidden = false;
}
const hideBanner = () => ($("banner").hidden = true);

// ---- Chat ----
const chatEl = $("chat");
let chatLog = JSON.parse(localStorage.getItem("aurora.chat") || "[]");

function renderMessage({ kind, text, meta, error }) {
  const div = document.createElement("div");
  div.className = `msg ${kind}${error ? " err" : ""}`;
  div.textContent = text;
  if (meta) {
    const m = document.createElement("span");
    m.className = "meta";
    m.textContent = meta;
    div.append(m);
  }
  chatEl.append(div);
  document.body.classList.add("has-chat");
  requestAnimationFrame(() => (chatEl.scrollTop = chatEl.scrollHeight));
}

function addMessage(kind, text, opts = {}) {
  const entry = { kind, text, ...opts };
  renderMessage(entry);
  chatLog.push(entry);
  chatLog = chatLog.slice(-40);
  localStorage.setItem("aurora.chat", JSON.stringify(chatLog));
}

function clearChat() {
  chatLog = [];
  localStorage.removeItem("aurora.chat");
  chatEl.replaceChildren();
  document.body.classList.remove("has-chat");
}

function addToolChip(name) {
  const chip = document.createElement("span");
  chip.className = "tool-chip";
  chip.textContent = TOOL_LABELS[name] || `🔧 ${name}`;
  $("tool-chips").append(chip);
}
const clearToolChips = () => $("tool-chips").replaceChildren();

// ---- Status do ESP32 ----
function fmtUptime(s) {
  const d = Math.floor(s / 86400), h = Math.floor((s % 86400) / 3600), m = Math.floor((s % 3600) / 60);
  return d ? `${d}d ${h}h` : h ? `${h}h ${m}min` : `${m} min`;
}

function updateStatus(m) {
  lastStatus = m;
  if (m.name) {
    $("assistant-name").textContent = m.name;
    document.title = `${m.name} — Assistente de voz para ESP32`;
  }
  $("st-wifi").textContent = m.wifi ? m.ssid || "conectado" : "desconectado";
  $("st-ip").textContent = m.ip || "—";
  $("st-rssi").textContent = m.rssi != null ? `${m.rssi} dBm` : "—";
  $("st-heap").textContent = m.heap ? `${Math.round(m.heap / 1024)} KB` : "—";
  $("st-uptime").textContent = m.uptime != null ? fmtUptime(m.uptime) : "—";
  $("st-model").textContent = m.model || "—";
  $("st-timers").textContent = m.timers ?? "—";
  $("st-fw").textContent = m.fw ? `v${m.fw}` : "—";

  if (m.ssid && !$("cfg-ssid").value) $("cfg-ssid").value = m.ssid;
  if (m.model) {
    const sel = $("cfg-model");
    if (![...sel.options].some((o) => o.value === m.model)) sel.add(new Option(m.model, m.model));
    sel.value = m.model;
  }

  document.body.classList.toggle("no-wifi", !m.wifi);
  $("conn-text").textContent = m.wifi ? "Conectado" : "Sem Wi-Fi";
  if (!m.wifi) showBanner(m.ssid ? `ESP32 sem Wi-Fi (tentando “${m.ssid}”)` : "ESP32 sem Wi-Fi configurado");
  else if (!m.hasKey) showBanner("Falta a chave da API do Gemini no ESP32");
  else hideBanner();
}

// ---- Fluxo de conversa ----
async function connect() {
  if (!("bluetooth" in navigator)) {
    toast(window.isSecureContext ? "Use o Google Chrome no Android." : "Abra a página via HTTPS.", { error: true });
    return;
  }
  document.body.classList.add("connecting");
  setState("disconnected", "Conectando…");
  try {
    await ble.connect();
  } catch (e) {
    setState("disconnected");
    if (e.name !== "NotFoundError") toast(`Falha ao conectar: ${e.message}`, { error: true, ms: 4000 });
  } finally {
    document.body.classList.remove("connecting");
  }
}

async function startListening() {
  if (!voice.canListen) {
    toast("Reconhecimento de voz indisponível neste navegador — use o campo de texto.", { error: true, ms: 4000 });
    return;
  }
  voice.stopSpeaking();
  setState("listening");
  $("interim").textContent = "";
  try {
    const text = await voice.listen((t) => ($("interim").textContent = t));
    if (state !== "listening") return;
    if (text) {
      ask(text);
    } else {
      $("interim").textContent = "";
      setState("idle", "Não ouvi nada — toque para tentar de novo");
    }
  } catch (err) {
    const msg = {
      "not-allowed": "Permita o acesso ao microfone para falar com o assistente.",
      "service-not-allowed": "Permita o acesso ao microfone para falar com o assistente.",
      network: "O reconhecimento de voz precisa de internet no celular.",
      "audio-capture": "Microfone indisponível.",
    }[err.message] || `Erro no reconhecimento de voz (${err.message}).`;
    setState("idle");
    toast(msg, { error: true, ms: 4000 });
  }
}

async function ask(text) {
  if (!ble.connected) {
    toast("Conecte ao ESP32 primeiro.", { error: true });
    return;
  }
  voice.stopSpeaking();
  addMessage("user", text);
  $("interim").textContent = "";
  clearToolChips();
  setState("thinking");
  clearTimeout(replyTimer);
  replyTimer = setTimeout(() => {
    if (state !== "thinking") return;
    addMessage("bot", "O ESP32 não respondeu a tempo. Tente novamente.", { error: true });
    setState("idle");
  }, REPLY_TIMEOUT_MS);
  try {
    await ble.send({ t: "ask", text });
  } catch (e) {
    clearTimeout(replyTimer);
    setState(ble.connected ? "idle" : "disconnected");
    toast(`Falha ao enviar: ${e.message}`, { error: true });
  }
}

async function respond(text, { allowContinue = true } = {}) {
  if (!prefs.speak || !voice.synth) return finishTurn(allowContinue);
  setState("speaking");
  const completed = await voice.speak(text, prefs);
  if (state === "speaking") finishTurn(allowContinue && completed);
}

function finishTurn(allowContinue) {
  setState(ble.connected ? "idle" : "disconnected");
  if (allowContinue && prefs.continuous && ble.connected) setTimeout(() => state === "idle" && startListening(), 400);
}

function onMessage(m) {
  switch (m.t) {
    case "status":
      updateStatus(m);
      break;
    case "thinking":
      if (state !== "thinking") setState("thinking");
      break;
    case "tool":
      addToolChip(m.name);
      break;
    case "reply":
    case "error": {
      clearTimeout(replyTimer);
      const meta = [m.model, m.ms ? `${(m.ms / 1000).toFixed(1)} s` : ""].filter(Boolean).join(" · ");
      addMessage("bot", m.text, { meta, error: m.t === "error" });
      respond(m.text, { allowContinue: m.t === "reply" });
      break;
    }
    case "alert":
      addMessage("alert", `⏰ ${m.text}`);
      navigator.vibrate?.([300, 150, 300, 150, 300]);
      if (state === "idle") respond(m.text, { allowContinue: false });
      else toast(m.text, { ms: 5000 });
      break;
    case "info":
      toast(m.text);
      break;
  }
}

// ---- Wake lock: mantém a tela ligada enquanto conectado ----
async function keepAwake(on) {
  try {
    if (on && "wakeLock" in navigator && !wakeLock) {
      wakeLock = await navigator.wakeLock.request("screen");
      wakeLock.addEventListener("release", () => (wakeLock = null));
    } else if (!on && wakeLock) {
      await wakeLock.release();
    }
  } catch {
    /* sem suporte ou bateria fraca */
  }
}
document.addEventListener("visibilitychange", () => {
  if (document.visibilityState === "visible" && ble.connected) keepAwake(true);
});

// --------------------------------------------------------------------------
// Eventos BLE
// --------------------------------------------------------------------------
function setComposerEnabled(on) {
  $("text-input").disabled = !on;
  $("btn-send").disabled = !on;
}

ble.addEventListener("connected", () => {
  setState("idle");
  $("conn-text").textContent = "Conectado";
  setComposerEnabled(true);
  toast(`Conectado a ${ble.device?.name || "ESP32"}`);
  ble.send({ t: "status" }).catch(() => {});
  keepAwake(true);
});

ble.addEventListener("disconnected", () => {
  clearTimeout(replyTimer);
  voice.abortListening();
  voice.stopSpeaking();
  clearToolChips();
  setState("disconnected");
  $("conn-text").textContent = "Desconectado";
  setComposerEnabled(false);
  hideBanner();
  keepAwake(false);
});

ble.addEventListener("reconnecting", (e) => {
  document.body.classList.add("connecting");
  setState("disconnected", `Reconectando… (tentativa ${e.detail})`);
  $("conn-text").textContent = "Reconectando";
});
ble.addEventListener("connected", () => document.body.classList.remove("connecting"));
ble.addEventListener("gaveup", () => {
  document.body.classList.remove("connecting");
  setState("disconnected");
  $("conn-text").textContent = "Desconectado";
  toast("Não foi possível reconectar. Toque no orbe para tentar de novo.", { error: true, ms: 4500 });
});

ble.addEventListener("message", (e) => onMessage(e.detail));

// --------------------------------------------------------------------------
// Controles
// --------------------------------------------------------------------------
$("orb").addEventListener("click", () => {
  switch (state) {
    case "disconnected":
      if (!document.body.classList.contains("connecting")) connect();
      break;
    case "idle":
      startListening();
      break;
    case "listening":
      voice.stopListening();
      break;
    case "speaking":
      voice.stopSpeaking();
      setState("idle");
      break;
    case "thinking":
      toast("Aguardando a resposta do ESP32…");
      break;
  }
});

$("conn-chip").addEventListener("click", () => (ble.connected ? openSettings() : connect()));

$("composer-form").addEventListener("submit", (e) => {
  e.preventDefault();
  const text = $("text-input").value.trim();
  if (!text) return;
  $("text-input").value = "";
  ask(text);
});

const contBtn = $("btn-continuous");
const syncContinuous = () => contBtn.setAttribute("aria-pressed", String(prefs.continuous));
contBtn.addEventListener("click", () => {
  prefs.continuous = !prefs.continuous;
  savePrefs();
  syncContinuous();
  toast(prefs.continuous ? "Conversa contínua ativada" : "Conversa contínua desativada");
});

// ---- Configurações ----
const sheet = $("settings");
function openSettings() {
  if (ble.connected) ble.send({ t: "status" }).catch(() => {});
  sheet.showModal();
}
$("btn-settings").addEventListener("click", openSettings);
$("banner-action").addEventListener("click", openSettings);
sheet.addEventListener("click", (e) => {
  if (e.target === sheet) sheet.close(); // toque fora fecha
});

function requireConnection() {
  if (ble.connected) return true;
  toast("Conecte ao ESP32 primeiro.", { error: true });
  return false;
}

$("btn-save-wifi").addEventListener("click", () => {
  if (!requireConnection()) return;
  const ssid = $("cfg-ssid").value.trim();
  if (!ssid) return toast("Informe o nome da rede.", { error: true });
  ble.send({ t: "cfg", ssid, pass: $("cfg-pass").value }).catch((e) => toast(e.message, { error: true }));
  $("cfg-pass").value = "";
  toast("Enviando Wi-Fi ao ESP32…");
});

$("btn-save-ai").addEventListener("click", () => {
  if (!requireConnection()) return;
  const msg = { t: "cfg", model: $("cfg-model").value };
  const key = $("cfg-key").value.trim();
  if (key) msg.key = key;
  ble.send(msg).catch((e) => toast(e.message, { error: true }));
  $("cfg-key").value = "";
});

$("btn-refresh").addEventListener("click", () => requireConnection() && ble.send({ t: "status" }));
$("btn-reset").addEventListener("click", () => {
  clearChat();
  if (ble.connected) ble.send({ t: "reset" });
  else toast("Conversa local apagada.");
});
$("btn-disconnect").addEventListener("click", () => {
  ble.disconnect();
  sheet.close();
});

// ---- Voz ----
function populateVoices() {
  const sel = $("cfg-voice");
  const list = voice.ptVoices();
  const others = voice.voices.filter((v) => !list.includes(v));
  sel.replaceChildren();
  if (!voice.voices.length) {
    sel.add(new Option("Padrão do sistema", ""));
    return;
  }
  for (const v of [...list, ...others]) sel.add(new Option(`${v.name} (${v.lang})`, v.voiceURI));
  sel.value = voice.pickVoice(prefs.voiceURI)?.voiceURI || "";
}
document.addEventListener("voices", populateVoices);
$("cfg-voice").addEventListener("change", (e) => {
  prefs.voiceURI = e.target.value;
  savePrefs();
});

const rateEl = $("cfg-rate");
const syncRate = () => ($("rate-out").textContent = `${Number(prefs.rate).toFixed(1)}×`);
rateEl.value = prefs.rate;
rateEl.addEventListener("input", () => {
  prefs.rate = Number(rateEl.value);
  savePrefs();
  syncRate();
});

$("cfg-speak").checked = prefs.speak;
$("cfg-speak").addEventListener("change", (e) => {
  prefs.speak = e.target.checked;
  savePrefs();
});

$("btn-test-voice").addEventListener("click", () => {
  const name = lastStatus?.name || "Aurora";
  voice.speak(`Olá! Eu sou a ${name}, sua assistente rodando no ESP32. Como posso ajudar?`, prefs);
});

// --------------------------------------------------------------------------
// Inicialização
// --------------------------------------------------------------------------
function init() {
  chatLog.forEach(renderMessage);
  syncContinuous();
  syncRate();
  populateVoices();
  setComposerEnabled(false);
  setState("disconnected");

  if (!("bluetooth" in navigator)) {
    showBanner(
      window.isSecureContext
        ? "Este navegador não tem Web Bluetooth. Use o Chrome no Android."
        : "Web Bluetooth exige HTTPS. Abra pelo link do GitHub Pages.",
      "Entendi",
    );
    $("banner-action").onclick = hideBanner;
  } else {
    navigator.bluetooth.getAvailability?.().then((ok) => {
      if (!ok) showBanner("Ative o Bluetooth do celular.", "OK");
    });
  }
}

init();
