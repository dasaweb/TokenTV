// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Arduino.h>

// Login/status page for the Claude usage screen. Embedded in the firmware so no LittleFS
// update is needed; it reuses the stylesheet and scripts already on the filesystem.
// clang-format off
static const char CLAUDE_PAGE_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="de">
<head>
<meta charset="UTF-8" />
<meta name="viewport" content="width=device-width, initial-scale=1" />
<title>Claude Usage</title>
<link rel="stylesheet" href="./css/pico.min.css" />
<link rel="stylesheet" href="./css/style.css" />
<script src="./js/alpinejs.min.js" defer></script>
<script src="./js/utils.js"></script>
<script src="./js/themeSwitcher.js"></script>
<script src="./js/main.js"></script>
<script>
function claudeHandler() {
  return {
    hasToken: !!localStorage.getItem("Authorization"), tokenRejected: false,
    s: null, url: "", code: "", scope: "user:profile", msg: "", busy: false, timer: null, layout: 0, pace: true,
    async init() {
      if (!this.hasToken) return;
      await this.load();
      this.timer = setInterval(() => { if (!this.busy) this.load(); }, 15000);
    },
    async call(path, body) {
      const opts = body === undefined ? {} : { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) };
      const r = await apiFetch(path, opts);
      this.tokenRejected = r.status === 401;
      const j = await r.json().catch(() => ({}));
      if (!r.ok) throw new Error(j.message || ("HTTP " + r.status));
      return j;
    },
    async load() {
      try { this.s = await this.call("/api/v1/claude/status"); this.layout = this.s.layout || 0; this.pace = this.s.pace !== false; }
      catch (e) { this.msg = "Status: " + e.message; }
    },
    async saveDisplay() {
      try { await this.call("/api/v1/claude/display", { layout: this.layout, pace: this.pace }); this.msg = "Anzeige gespeichert."; }
      catch (e) { this.msg = "Anzeige: " + e.message; }
    },
    async start() {
      this.busy = true; this.msg = "";
      try {
        const j = await this.call("/api/v1/claude/login/start", { scope: this.scope });
        this.url = j.url;
        window.open(j.url, "_blank", "noopener");
      } catch (e) { this.msg = e.message; }
      this.busy = false;
    },
    async finish() {
      this.busy = true; this.msg = "Pruefe Code (dauert einige Sekunden)...";
      try {
        await this.call("/api/v1/claude/login/finish", { code: this.code });
        // The device exchanges the code in the background; poll until it reports a result
        // First poll only after the device had time for the TLS exchange; open browser connections cost heap
        for (let i = 0; i < 20; i++) {
          await new Promise((r) => setTimeout(r, i === 0 ? 8000 : 3000));
          try { this.s = await this.call("/api/v1/claude/status"); } catch (e) { continue; }
          if (this.s.logged_in) { this.msg = "Angemeldet."; this.code = ""; this.url = ""; break; }
          if (this.s.login_error) { this.msg = "Fehler: " + this.s.login_error + " (bitte Login neu starten)"; break; }
        }
      } catch (e) { this.msg = "Fehler: " + e.message; }
      this.busy = false;
    },
    async refresh() {
      try { await this.call("/api/v1/claude/refresh", {}); this.msg = "Abruf angestossen."; setTimeout(() => this.load(), 6000); }
      catch (e) { this.msg = e.message; }
    },
    async logout() {
      if (!confirm("Token vom Geraet loeschen?")) return;
      try { await this.call("/api/v1/claude/logout", {}); this.msg = "Abgemeldet."; await this.load(); }
      catch (e) { this.msg = e.message; }
    },
    pct(l) { return l && l.valid ? Math.round(l.percent) + " %" : "-"; },
    when(ts) { return ts ? new Date(ts * 1000).toLocaleString() : "-"; },
    get needsLogin() { return this.s && (!this.s.logged_in || this.s.state === "relogin_needed"); },
  };
}
</script>
</head>
<body>
<main class="container">
<header id="header-placeholder"></header>
<a href="./index.html" class="wifi-back-link">&larr; Back to Home</a>

<section x-data="claudeHandler()" x-init="init()">
  <h2>Claude Usage</h2>

  <template x-if="!hasToken || tokenRejected">
    <p>Das Geraet lehnt den API-Token dieses Browsers ab oder es ist keiner gespeichert. Bitte den aktuellen
      Token auf der <a href="./token.html">Token-Seite</a> eintragen und speichern.</p>
  </template>

  <template x-if="s">
    <article>
      <p><strong>Status:</strong> <span x-text="s.state"></span>
        <span x-show="s.error" x-text="' – ' + s.error"></span></p>
      <p><strong>5 Stunden:</strong> <span x-text="pct(s.session)"></span>,
        Reset <span x-text="when(s.session.resets_at)"></span></p>
      <p><strong>Woche:</strong> <span x-text="pct(s.weekly)"></span>,
        Reset <span x-text="when(s.weekly.resets_at)"></span></p>
      <p><small>Zuletzt abgerufen: <span x-text="when(s.fetched_at)"></span> ·
        Heap frei: <span x-text="s.free_heap"></span> B (Block <span x-text="s.max_free_block"></span> B)</small></p>
      <div x-show="s.logged_in" style="display:flex;gap:1em;flex-wrap:wrap">
        <button type="button" class="secondary" @click="refresh()">Jetzt abrufen</button>
        <button type="button" class="contrast" @click="logout()">Abmelden</button>
      </div>
    </article>
  </template>

  <template x-if="s">
    <article>
      <label for="claude-layout"><strong>Layout auf dem Display</strong></label>
      <select id="claude-layout" x-model.number="layout" @change="saveDisplay()">
        <option value="0">Ringe</option>
        <option value="1">Balken</option>
      </select>
      <label>
        <input type="checkbox" role="switch" x-model="pace" @change="saveDisplay()" />
        Zeit-Markierung anzeigen
      </label>
      <small>Der weisse Strich zeigt, wie viel Zeit des Fensters (5 Stunden bzw. Woche) schon vergangen ist.
        Liegt der Verbrauch dahinter, bist du langsamer als gleichmaessig verteilt. Bei den Balken steht
        zusaetzlich die Reserve darunter.</small>
    </article>
  </template>

  <template x-if="needsLogin">
    <article>
      <h3>Mit Claude anmelden</h3>
      <p>Das Geraet meldet sich mit einem eigenen OAuth-Login an (unabhaengig von Claude Code auf deinem Rechner).</p>
      <p><strong>1.</strong> Login starten, im neuen Tab bei Claude anmelden und autorisieren.</p>
      <button type="button" @click="start()" :disabled="busy">Login starten</button>
      <p x-show="url"><small>Falls kein Tab aufging: <a :href="url" target="_blank" rel="noopener">Login-Link oeffnen</a></small></p>
      <p><strong>2.</strong> Den angezeigten Code (Format <code>code#state</code>) hier einfuegen:</p>
      <input type="text" x-model="code" placeholder="Code einfuegen" autocomplete="off" />
      <button type="button" @click="finish()" :disabled="busy || !code">Code bestaetigen</button>
      <details>
        <summary>Erweitert</summary>
        <label>OAuth-Scope <input type="text" x-model="scope" /></label>
        <small>Standard ist <code>user:profile</code>. Falls der Abruf mit HTTP 403 scheitert:
          <code>user:profile user:inference</code> verwenden.</small>
      </details>
    </article>
  </template>

  <p x-text="msg"></p>
</section>

<footer id="footer-placeholder"></footer>
</main>
</body>
</html>
)HTML";
// clang-format on
