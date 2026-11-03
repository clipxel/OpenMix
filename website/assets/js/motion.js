// Movimiento del sitio: entrada con el logo, barra que se oscurece al bajar, revelado al hacer
// scroll, la forma de onda de la portada y una animación propia para cada función. Cada escena
// corre solo mientras está en pantalla.

(() => {
  const reduce = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
  const BPM = 124;
  const C = { a: "#93cbe3", b: "#dfe5e9", body: "#3f6be8", accent: "#93cbe3", white: "#ffffff" };   // paleta del logo

  // ── Entrada: solo la primera vez de cada visita ──────────────────────────────
  const intro = document.querySelector(".intro");
  let seen = false;
  try { seen = sessionStorage.getItem("om-intro") === "1"; } catch { /* sin almacenamiento */ }
  if (intro) {
    if (seen || reduce) intro.remove();
    else {
      try { sessionStorage.setItem("om-intro", "1"); } catch { /* sin almacenamiento */ }
      const finish = () => intro.classList.add("done");
      window.addEventListener("load", () => setTimeout(finish, 1500));
      setTimeout(finish, 3200);
      intro.addEventListener("transitionend", () => intro.remove());
    }
  }

  // ── Entrada de la MacBook: cuando termina el logo (o enseguida si no hay intro) ──
  const mbpHero = document.querySelector(".mbp-hero");
  if (mbpHero) {
    const keys = mbpHero.querySelector(".mbp-keys");
    if (keys) for (let i = 0; i < 14 * 6; i++) keys.appendChild(document.createElement("i"));
    const go = () => mbpHero.classList.add("go");
    if (reduce) go();
    else {
      const img = new Image();
      img.src = "assets/img/app-screen.jpg";
      const delay = document.querySelector(".intro") ? 1700 : 150;
      Promise.race([img.decode().catch(() => {}), new Promise((r) => setTimeout(r, 2500))])
        .then(() => setTimeout(go, delay));
    }
  }

  // ── Barra superior ──────────────────────────────────────────────────────────
  const bar = document.querySelector(".topbar");
  const hero = document.querySelector(".hero");
  const onScroll = () => bar && bar.classList.toggle("solid", !hero || window.scrollY > 40);
  window.addEventListener("scroll", onScroll, { passive: true });
  onScroll();

  // ── Revelado al hacer scroll ────────────────────────────────────────────────
  const io = "IntersectionObserver" in window && !reduce
    ? new IntersectionObserver((entries) => entries.forEach((e) => {
        if (e.isIntersecting) { e.target.classList.add("in"); io.unobserve(e.target); }
      }), { rootMargin: "0px 0px -8% 0px", threshold: 0.08 })
    : null;
  document.querySelectorAll(".reveal").forEach((n) => (io ? io.observe(n) : n.classList.add("in")));

  // ── Utilidades de dibujo ────────────────────────────────────────────────────
  const hash = (n) => { n = Math.sin(n * 127.1) * 43758.5453; return n - Math.floor(n); };
  const clamp = (v, a, b) => Math.max(a, Math.min(b, v));
  const ease = (x) => 1 - Math.pow(1 - clamp(x, 0, 1), 3);

  // Energía de un tema house en la posición `beat`: kick en cada beat, clap en 2 y 4, hats.
  function level(beat, seed) {
    const b = beat - Math.floor(beat);
    const barN = Math.floor(beat / 4), inBar = ((Math.floor(beat) % 4) + 4) % 4;
    const breakdown = ((Math.floor(barN / 8) + seed) % 4 + 4) % 4 === 3;
    const kick = breakdown ? 0 : Math.exp(-b * 9) * 0.95;
    const clap = (inBar === 1 || inBar === 3) ? Math.exp(-b * 14) * 0.45 : 0;
    const hat = Math.exp(-((b * 2) % 1) * 22) * 0.22;
    const bass = (breakdown ? 0.18 : 0.3) * (0.6 + 0.4 * Math.sin(beat * Math.PI * 0.5 + seed));
    const grain = hash(Math.floor(beat * 64) + seed * 1000) * 0.12;
    return { low: Math.min(1, kick + bass * 0.6), high: Math.min(1, clap + hat + grain) };
  }

  function canvasFor(el) {
    const cv = el.tagName === "CANVAS" ? el : el.querySelector("canvas");
    const ctx = cv.getContext("2d");
    const s = { cv, ctx, w: 0, h: 0 };
    const fit = () => {
      const dpr = Math.min(2, window.devicePixelRatio || 1);
      s.w = cv.clientWidth; s.h = cv.clientHeight;
      cv.width = Math.round(s.w * dpr); cv.height = Math.round(s.h * dpr);
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    };
    fit();
    window.addEventListener("resize", fit);
    return s;
  }

  function text(ctx, str, x, y, { size = 11, weight = 600, color = "#fff", align = "left", wide = false, spacing = 0 } = {}) {
    ctx.font = `${weight} ${wide ? "125% " : ""}${size}px Archivo, "Helvetica Neue", Arial, sans-serif`;
    if ("letterSpacing" in ctx) ctx.letterSpacing = `${spacing}px`;
    ctx.fillStyle = color;
    ctx.textAlign = align;
    ctx.fillText(str, x, y);
    if ("letterSpacing" in ctx) ctx.letterSpacing = "0px";
  }

  // Dos carriles de forma de onda como en el programa.
  function lane(ctx, w, y, laneH, beatAt, seed, alphaPast, alphaFuture) {
    const BAR = 3, GAP = 1;
    for (let x = 0; x < w; x += BAR + GAP) {
      const { low, high } = level(beatAt(x), seed);
      const a = x < w / 2 ? alphaPast : alphaFuture;
      const lh = low * laneH, hh = high * laneH * 0.9;
      ctx.fillStyle = `rgba(63,107,232,${a})`;
      ctx.fillRect(x, y - lh, BAR, lh * 2);
      ctx.fillStyle = `rgba(147,203,227,${a * 0.9})`;
      ctx.fillRect(x, y - hh * 0.55, BAR, hh * 1.1);
    }
  }

  // ── Escenas ─────────────────────────────────────────────────────────────────
  const scenes = {};

  // Portada: la pantalla de la MacBook. Sobre la captura real de OpenMix, la franja de formas de onda
  // corre como en el programa (8 s a la vista, dos decks a 124 BPM, cabezal fijo en el centro).
  scenes.hero = (el) => {
    const s = canvasFor(el);
    const BG = "rgb(13,14,16)", BLUE = "rgb(44,95,212)", WHITE = "rgb(213,215,219)", ORANGE = "rgb(192,128,64)";
    function column(ctx, x, cy, half, b, seed) {
      const f = b - Math.floor(b);
      const barN = Math.floor(b / 4);
      const brk = ((Math.floor(barN / 8) + seed) % 4 + 4) % 4 === 3;
      const grain = hash(Math.floor(b * 48) + seed * 777);
      if (brk) {                                              // break: solo agudos y algún golpe suave
        const a = (0.1 + 0.12 * grain) * half;
        ctx.fillStyle = ORANGE; ctx.fillRect(x, cy - a, 2, a * 2);
        if (f < 0.08 && Math.floor(b) % 2) { const p = (1 - f / 0.08) * 0.55 * half; ctx.fillStyle = WHITE; ctx.fillRect(x, cy - p, 2, p * 2); }
        return;
      }
      const env = Math.exp(-f * 6.5);
      const amp = (0.12 + 0.84 * env + 0.05 * grain) * half;
      ctx.fillStyle = BLUE;
      ctx.fillRect(x, cy - amp, 2, amp * 2);
      const hatF = Math.abs(f - 0.5);                         // hats en el contratiempo
      if (hatF < 0.06) { const hA = (0.32 - hatF * 3) * half; ctx.fillStyle = ORANGE; ctx.fillRect(x, cy - hA, 2, hA * 2); }
      const core = Math.max(0, (env - 0.45) / 0.55);
      if (core > 0) { const c = amp * (0.35 + 0.65 * core); ctx.fillStyle = WHITE; ctx.fillRect(x, cy - c, 2, c * 2); }
    }
    return (t) => {
      const { ctx, w, h } = s;
      if (!w || !h) return;
      ctx.fillStyle = BG; ctx.fillRect(0, 0, w, h);
      const PX = w / (8 * BPM / 60);                          // 8 s de tema a lo ancho
      const beat = t * BPM / 60 + 32;
      const lanes = [[h * 0.248, 0, "#2ec6e8", "1"], [h * 0.752, 1, "#f29a3a", "2"]];
      const half = h * 0.2;
      for (const [cy, seed, col, n] of lanes) {
        for (let k = Math.ceil(beat - (w / 2) / PX); ; k++) {           // grilla
          const x = w / 2 + (k - beat) * PX;
          if (x > w) break;
          ctx.fillStyle = k % 4 === 0 ? "rgba(255,255,255,0.30)" : "rgba(255,255,255,0.13)";
          ctx.fillRect(Math.round(x), cy - half - h * 0.04, 1, half * 2 + h * 0.08);
        }
        for (let x = 0; x < w; x += 2) column(ctx, x, cy, half, beat + (x - w / 2) / PX + seed * 0.001, seed);
        const tag = Math.max(8, h * 0.11);                               // número de deck
        ctx.fillStyle = col; ctx.fillRect(4, cy - half - h * 0.02, tag, tag);
        text(ctx, n, 4 + tag / 2, cy - half - h * 0.02 + tag * 0.8, { size: Math.max(6, tag * 0.75), weight: 700, color: "#0b0b0b", align: "center" });
      }
      ctx.fillStyle = "rgb(232,233,235)";                                // cabezal
      ctx.fillRect(Math.round(w / 2) - 1, 0, 2, h);
      const tr = Math.max(3, h * 0.035);
      ctx.beginPath(); ctx.moveTo(w / 2 - tr, 0); ctx.lineTo(w / 2 + tr, 0); ctx.lineTo(w / 2, tr * 1.6); ctx.fill();   // ▼ arriba
      ctx.beginPath(); ctx.moveTo(w / 2 - tr, h); ctx.lineTo(w / 2 + tr, h); ctx.lineTo(w / 2, h - tr * 1.6); ctx.fill(); // ▲ abajo
    };
  };

  // Portada: el resto de la pantalla también vive. Coordenadas en píxeles de la captura (2000 × 1250):
  // platos que giran, tiempos que corren, vúmetros, PLAY que late, hot cues y el ON/OFF de Beat FX.
  scenes.screenfx = (el) => {
    const s = canvasFor(el);
    const DX = 1271;                                          // del deck 1 al deck 2
    const FONT = '-apple-system, "Helvetica Neue", Arial, sans-serif';
    const CUES = ["#EE5A52", "#7FD457", "#3FC7E6", "#5C7CF0", "#E064C4", "#F0B347", "#2ED1B0", "#A474F0"];
    const PADX = [[12, 180], [190, 357], [368, 535], [546, 713]], PADY = [[669, 721], [730, 782]];
    const fmt = (sec, sign) => {
      const m = Math.floor(sec / 60), r = sec - m * 60;
      return (sign ? "-" : "") + m + ":" + r.toFixed(1).padStart(4, "0");
    };
    const round = (ctx, x, y, w, h, r) => { ctx.beginPath(); ctx.roundRect ? ctx.roundRect(x, y, w, h, r) : ctx.rect(x, y, w, h); };
    // Energía en el cabezal (el mismo tema que corre en la franja de arriba)
    const energy = (beat, seed) => {
      const f = beat - Math.floor(beat);
      const barN = Math.floor(beat / 4);
      const brk = ((Math.floor(barN / 8) + seed) % 4 + 4) % 4 === 3;
      const env = Math.exp(-f * 6.5);
      const hat = Math.abs(f - 0.5) < 0.06 ? 0.25 : 0;
      return brk ? 0.25 + 0.15 * hat * 4 : 0.45 + 0.5 * env + hat * 0.3;
    };
    function meter(ctx, k, x, y0, y1, w, lvl) {
      const segH = 3, gap = 1.6;
      const n = Math.floor((y1 - y0) / (segH + gap));
      const lit = Math.round(n * Math.max(0, Math.min(1, lvl)));
      for (let i = 0; i < n; i++) {
        const y = y1 - (i + 1) * (segH + gap);
        const frac = i / n;
        ctx.fillStyle = i < lit ? (frac > 0.9 ? "#ff5a4f" : frac > 0.72 ? "#f2c94c" : "#3ccb8b") : "rgba(255,255,255,0.05)";
        ctx.fillRect(x * k, y * k, w * k, segH * k);
      }
    }
    const peaks = [0, 0, 0, 0, 0, 0];
    return (t) => {
      const { ctx, w, h } = s;
      if (!w || !h) return;
      const k = w / 2000;
      ctx.clearRect(0, 0, w, h);
      const beat = t * BPM / 60 + 32;
      const f = beat - Math.floor(beat);
      const pulse = Math.exp(-f * 8);
      for (const [d, seed] of [[0, 0], [1, 1]]) {
        const ox = d * DX;
        // Plato: tapar la marca fija y dibujarla girando (33⅓ rpm)
        ctx.fillStyle = "rgb(23,24,27)";
        ctx.fillRect((ox + 391) * k, 395 * k, 9 * k, 26 * k);
        const ang = t * 2 * Math.PI * 0.555 + d * 1.3;
        const cx = (ox + 395.5) * k, cy = 476 * k;
        ctx.save();
        ctx.translate(cx, cy); ctx.rotate(ang);
        ctx.fillStyle = "rgb(211,212,215)";
        ctx.fillRect(-1.7 * k, -79 * k, 3.4 * k, 21 * k);
        ctx.restore();
        // Tiempos: restante y transcurrido en la cabecera, restante en el centro del plato
        const total = 150, el = (45 + t * (d ? 1.0 : 1.0) + d * 6) % total, rem = total - el;
        ctx.fillStyle = "rgb(23,24,27)";
        ctx.fillRect((ox + 484) * k, 271 * k, 92 * k, 29 * k);
        ctx.fillRect((ox + 512) * k, 300 * k, 64 * k, 19 * k);
        ctx.textAlign = "right";
        ctx.fillStyle = "rgb(232,233,235)"; ctx.font = `400 ${24 * k}px ${FONT}`;
        ctx.fillText(fmt(rem, true), (ox + 568) * k, 294 * k);
        ctx.fillStyle = "rgb(170,172,176)"; ctx.font = `400 ${16 * k}px ${FONT}`;
        ctx.fillText(fmt(el, false), (ox + 568) * k, 315 * k);
        ctx.fillStyle = "rgb(29,30,34)";
        ctx.fillRect((ox + 368) * k, 465 * k, 56 * k, 23 * k);
        ctx.textAlign = "center"; ctx.fillStyle = "rgb(232,233,235)"; ctx.font = `400 ${16 * k}px ${FONT}`;
        ctx.fillText(fmt(rem, true).replace(/\.\d$/, ""), (ox + 395.5) * k, 482 * k);
        // PLAY late con cada golpe
        round(ctx, (ox + 13) * k, 529 * k, 122 * k, 56 * k, 7 * k);
        ctx.strokeStyle = `rgba(60,203,139,${0.35 + 0.55 * pulse})`; ctx.lineWidth = 2 * k; ctx.stroke();
        ctx.fillStyle = `rgba(60,203,139,${0.05 + 0.12 * pulse})`; ctx.fill();
        // Hot cues: cada 4 compases se dispara uno, se ilumina un beat
        const barN = Math.floor(beat / 4);
        const which = (Math.floor(barN / 2) + d * 3) % 8;
        const inBar = beat - barN * 4;
        if ((barN + d) % 2 === 0 && inBar < 1) {
          const [x0, x1] = PADX[which % 4], [y0, y1] = PADY[Math.floor(which / 4)];
          const a = 1 - inBar * 0.6;
          round(ctx, (ox + x0) * k, y0 * k, (x1 - x0) * k, (y1 - y0) * k, 6 * k);
          ctx.globalAlpha = a; ctx.fillStyle = CUES[which]; ctx.fill(); ctx.globalAlpha = 1;
          ctx.fillStyle = "#0b0b0b"; ctx.font = `600 ${15 * k}px ${FONT}`; ctx.textAlign = "center";
          ctx.fillText(String(which + 1), (ox + (x0 + x1) / 2) * k, ((y0 + y1) / 2 + 5) * k);
        }
      }
      // Vúmetros: dos por canal y el master, con caída suave y pico
      const l1 = energy(beat, 0), l2 = energy(beat + 0.003, 1);
      const targets = [l1 * 0.95, l1 * 0.9, l2 * 0.92, l2 * 0.88, (l1 + l2) * 0.47, (l1 + l2) * 0.45];
      for (let i = 0; i < 6; i++) peaks[i] = Math.max(targets[i], peaks[i] - 0.025);
      ctx.fillStyle = "rgb(13,14,16)";
      meter(ctx, k, 959, 443, 605, 3, peaks[0]);
      meter(ctx, k, 964, 443, 605, 3, peaks[1]);
      meter(ctx, k, 977, 443, 605, 3, peaks[2]);
      meter(ctx, k, 982, 443, 605, 3, peaks[3]);
      meter(ctx, k, 770, 413, 475, 5, peaks[4]);
      meter(ctx, k, 778, 413, 475, 5, peaks[5]);
      // Beat FX: ECHO se prende 4 compases de cada 8
      const fxOn = Math.floor(beat / 16) % 2 === 1;
      if (fxOn) {
        round(ctx, 1131 * k, 734 * k, 130 * k, 46 * k, 7 * k);
        ctx.fillStyle = `rgba(147,203,227,${0.75 + 0.2 * pulse})`; ctx.fill();
        ctx.fillStyle = "#0b0b0b"; ctx.font = `600 ${16 * k}px ${FONT}`; ctx.textAlign = "center";
        ctx.fillText("ON/OFF", 1196 * k, 763 * k);
      }
    };
  };

  // SYNC: el deck 2 entra corrido; al apretar SYNC toma el tempo y la fase del deck 1.
  scenes.sync = (el) => {
    const s = canvasFor(el);
    const CYCLE = 9, PRESS = 3;
    return (t) => {
      const { ctx, w, h } = s;
      ctx.clearRect(0, 0, w, h);
      const tc = t % CYCLE;
      const PX = w / 7;
      const beatA = t * BPM / 60;
      const drift = 0.32 + ((127.4 - BPM) / 60) * Math.min(tc, PRESS);              // beats de desfase
      const err = tc < PRESS ? drift : drift * (1 - ease((tc - PRESS) / 0.45));
      const beatB = beatA + err;
      const laneH = h * 0.1;
      const yA = h * 0.3, yB = h * 0.56;
      // grilla de cada deck
      for (const [y, b, col] of [[yA, beatA, C.a], [yB, beatB, C.b]]) {
        for (let k = Math.ceil(b - (w / 2) / PX); ; k++) {
          const x = w / 2 + (k - b) * PX;
          if (x > w) break;
          ctx.fillStyle = "rgba(255,255,255,0.08)";
          ctx.fillRect(Math.round(x), y - laneH - 6, 1, laneH * 2 + 12);
          ctx.fillStyle = col;
          ctx.fillRect(Math.round(x) - 3, y - laneH - 10, 7, 3);
        }
      }
      lane(ctx, w, yA, laneH, (x) => beatA + (x - w / 2) / PX, 0, 0.5, 0.95);
      lane(ctx, w, yB, laneH, (x) => beatB + (x - w / 2) / PX, 1, 0.5, 0.95);
      ctx.fillStyle = "rgba(255,255,255,0.9)";
      ctx.fillRect(Math.round(w / 2), yA - laneH - 16, 2, yB - yA + laneH * 2 + 32);

      const synced = tc >= PRESS;
      const bpmB = synced ? BPM : 127.4;
      text(ctx, "DECK 1", 22, 30, { size: 10, color: C.a, spacing: 1.5 });
      text(ctx, `${BPM.toFixed(2)} BPM`, 82, 30, { size: 10, color: "#bbb", spacing: 1 });
      text(ctx, "DECK 2", 22, yB + laneH + 34, { size: 10, color: C.b, spacing: 1.5 });
      text(ctx, `${bpmB.toFixed(2)} BPM`, 82, yB + laneH + 34, { size: 10, color: synced ? "#fff" : "#bbb", spacing: 1 });

      // botón SYNC
      const bx = 22, by = h - 58, bw = 92, bh = 34;
      const flash = synced && tc - PRESS < 0.25;
      ctx.fillStyle = synced ? (flash ? "#fff" : C.accent) : "rgba(255,255,255,0.06)";
      ctx.fillRect(bx, by, bw, bh);
      text(ctx, "SYNC", bx + bw / 2, by + 22, { size: 12, weight: 700, color: synced ? "#08161d" : "#888", align: "center", spacing: 2 });

      // medidor de fase
      const mx = w - 22 - 220, my = h - 41;
      ctx.fillStyle = "rgba(255,255,255,0.12)";
      ctx.fillRect(mx, my, 220, 2);
      ctx.fillRect(mx + 110, my - 8, 1, 18);
      let ph = ((err % 1) + 1.5) % 1 - 0.5;
      const ix = mx + 110 + ph * 220;
      ctx.fillStyle = Math.abs(ph) < 0.01 ? C.accent : "#fff";
      ctx.fillRect(ix - 4, my - 5, 8, 12);
      text(ctx, Math.abs(ph) < 0.01 ? "EN FASE" : "FASE", mx + 220, my - 16, { size: 10, color: Math.abs(ph) < 0.01 ? C.accent : "#888", align: "right", spacing: 1.5 });
    };
  };

  // BEAT FX: golpes que se repiten a 1/2 beat — DELAY (una vez), ECHO (se apaga), PING PONG (L/R).
  scenes.fx = (el) => {
    const s = canvasFor(el);
    const TYPES = ["DELAY", "ECHO", "PING PONG"];
    const ALL = ["DELAY", "ECHO", "PING PONG", "SPIRAL", "REVERB", "TRANS", "FILTER", "FLANGER", "PHASER", "PITCH", "SLIP ROLL", "ROLL", "VINYL BRAKE", "HELIX"];
    return (t) => {
      const { ctx, w, h } = s;
      ctx.clearRect(0, 0, w, h);
      const ti = Math.floor(t / 4.5) % TYPES.length;
      const type = TYPES[ti];
      const beat = t * BPM / 60;
      const PX = w / 7, playX = w * 0.72, cy = h * 0.5;
      const maxH = h * 0.26;
      // grilla
      for (let k = Math.ceil(beat - playX / PX); k <= beat; k++) {
        const x = playX - (beat - k) * PX;
        ctx.fillStyle = k % 4 === 0 ? "rgba(255,255,255,0.16)" : "rgba(255,255,255,0.07)";
        ctx.fillRect(Math.round(x), cy - maxH - 10, 1, maxH * 2 + 20);
      }
      ctx.fillStyle = "rgba(255,255,255,0.12)";
      ctx.fillRect(0, cy, playX, 1);
      // golpes: uno cada 2 beats; repeticiones cada 1/2 beat
      const repeats = type === "DELAY" ? 1 : 7;
      let glow = 0;
      for (let k = Math.floor((beat - playX / PX) / 2) * 2 - 8; k <= beat; k += 2) {
        for (let n = 0; n <= repeats; n++) {
          const eb = k + n * 0.5;
          if (eb > beat) break;
          const amp = n === 0 ? 1 : type === "DELAY" ? 0.85 : Math.pow(0.62, n);
          const x = playX - (beat - eb) * PX;
          if (x < -10) continue;
          const hgt = maxH * amp;
          const col = n === 0 ? "#ffffff" : C.accent;
          ctx.fillStyle = col;
          ctx.globalAlpha = n === 0 ? 0.95 : 0.4 + 0.55 * amp;
          if (type === "PING PONG" && n > 0) {
            if (n % 2) ctx.fillRect(x - 2, cy - hgt, 5, hgt);      // izquierda arriba
            else ctx.fillRect(x - 2, cy, 5, hgt);                // derecha abajo
          } else ctx.fillRect(x - 2, cy - hgt, 5, hgt * 2);
          ctx.globalAlpha = 1;
          if (beat - eb < 0.15) glow = Math.max(glow, (1 - (beat - eb) / 0.15) * amp);
        }
      }
      // cabezal
      ctx.fillStyle = `rgba(255,255,255,${0.5 + glow * 0.5})`;
      ctx.fillRect(Math.round(playX), cy - maxH - 16, 2, maxH * 2 + 32);
      if (type === "PING PONG") {
        text(ctx, "L", playX + 12, cy - maxH + 10, { size: 10, color: "#777" });
        text(ctx, "R", playX + 12, cy + maxH, { size: 10, color: "#777" });
      }
      // nombre, beat y level
      text(ctx, type, 24, 44, { size: 22, weight: 700, wide: true, spacing: 1 });
      text(ctx, "BEAT 1/2", 24, 66, { size: 10, color: "#999", spacing: 1.5 });
      const lv = 0.7 + 0.1 * Math.sin(t * 1.3);
      ctx.fillStyle = "rgba(255,255,255,0.12)"; ctx.fillRect(w - 24 - 120, 38, 120, 3);
      ctx.fillStyle = C.accent; ctx.fillRect(w - 24 - 120, 38, 120 * lv, 3);
      text(ctx, "LEVEL", w - 24, 60, { size: 10, color: "#999", align: "right", spacing: 1.5 });
      // lista de los 14
      const fs = Math.max(8, Math.min(10, w / 70));
      let x = 24;
      const y = h - 22;
      for (const name of ALL) {
        ctx.font = `600 ${fs}px Archivo, Arial, sans-serif`;
        const tw = ctx.measureText(name).width;
        if (x + tw > w - 20) break;
        text(ctx, name, x, y, { size: fs, color: name === type ? C.accent : "#5c5c5c" });
        x += tw + 14;
      }
    };
  };

  // Análisis: la forma de onda se dibuja, aparece la grilla y se resuelven BPM y tonalidad.
  scenes.analysis = (el) => {
    const s = canvasFor(el);
    const CYCLE = 9;
    return (t) => {
      const { ctx, w, h } = s;
      ctx.clearRect(0, 0, w, h);
      const tc = t % CYCLE;
      const fade = tc > CYCLE - 0.7 ? 1 - (tc - (CYCLE - 0.7)) / 0.7 : 1;
      ctx.globalAlpha = fade;
      const scan = clamp(tc / 3.2, 0, 1) * w;
      const cy = h * 0.5, laneH = h * 0.22, PX = w / 16;
      const BAR = 2, GAP = 1;
      for (let x = 0; x < scan; x += BAR + GAP) {
        const { low, high } = level(x / PX + 0.0001, 3);
        const lh = low * laneH, hh = high * laneH;
        ctx.fillStyle = "rgba(63,107,232,0.95)"; ctx.fillRect(x, cy - lh, BAR, lh * 2);
        ctx.fillStyle = "rgba(147,203,227,0.9)"; ctx.fillRect(x, cy - hh * 0.6, BAR, hh * 1.2);
        ctx.fillStyle = "rgba(255,255,255,0.85)"; ctx.fillRect(x, cy - hh * 0.22, BAR, hh * 0.44);
      }
      if (tc < 3.2) { ctx.fillStyle = "#fff"; ctx.fillRect(scan, cy - laneH - 18, 2, laneH * 2 + 36); }
      // grilla que cae
      const g = ease((tc - 3.2) / 0.8);
      if (g > 0) for (let k = 0; k * PX < w; k++) {
        const x = Math.round(k * PX);
        const len = (laneH * 2 + 30) * g;
        ctx.fillStyle = k % 4 === 0 ? C.accent : "rgba(255,255,255,0.35)";
        ctx.fillRect(x, cy - laneH - 15, 1, len);
      }
      // BPM, tonalidad, estado
      const bpm = tc < 3.2 ? (90 + hash(Math.floor(t * 20)) * 60).toFixed(2) : "124.00";
      text(ctx, "BPM", 24, 34, { size: 10, color: "#888", spacing: 1.5 });
      text(ctx, bpm, 24, 64, { size: 26, weight: 700, color: tc < 3.2 ? "#777" : "#fff", wide: true });
      const k = ease((tc - 3.6) / 0.5);
      text(ctx, "KEY", w - 24, 34, { size: 10, color: "#888", align: "right", spacing: 1.5 });
      ctx.globalAlpha = fade * k;
      text(ctx, "8A", w - 24, 64, { size: 26, weight: 700, align: "right", wide: true });
      ctx.globalAlpha = fade;
      const done = tc > 4.1;
      text(ctx, done ? "ANALIZADO" : "ANALIZANDO…", 24, h - 22, { size: 10, color: done ? C.accent : "#888", spacing: 2 });
      text(ctx, done ? "GRILLA · TONALIDAD · BPM" : "", w - 24, h - 22, { size: 10, color: "#666", align: "right", spacing: 1.5 });
      ctx.globalAlpha = 1;
    };
  };

  // SAMPLES / RMX: los 8 pads tocan un patrón en la grilla.
  scenes.rmx = (el) => {
    const PADS = [["KICK", "X-PAD", "#6f8ff2"], ["SNARE", "X-PAD", "#7FD457"], ["CLAP", "X-PAD", "#3FC7E6"], ["HAT", "X-PAD", "#5C7CF0"],
                  ["BUILD UP", "SCENE", "#93cbe3"], ["BREAK DOWN", "SCENE", "#93cbe3"], ["ECHO OUT", "SCENE", "#93cbe3"], ["BRAKE", "SCENE", "#93cbe3"]];
    const grid = document.createElement("div"); grid.className = "pads";
    const pads = PADS.map(([n, sub, c]) => {
      const p = document.createElement("div"); p.className = "pad"; p.style.setProperty("--c", c);
      p.innerHTML = `<span></span><small></small>`; p.firstChild.textContent = n; p.lastChild.textContent = sub;
      grid.append(p); return p;
    });
    const steps = document.createElement("div"); steps.className = "steps";
    const cells = Array.from({ length: 16 }, () => steps.appendChild(document.createElement("span")));
    const meter = document.createElement("div"); meter.className = "rmx-meter"; meter.textContent = "1/16 · 124 BPM";
    el.append(grid, meter, steps);
    let lastStep = -1;
    const hit = (i) => { pads[i].classList.remove("hit"); void pads[i].offsetWidth; pads[i].classList.add("hit"); setTimeout(() => pads[i].classList.remove("hit"), 90); };
    return (t) => {
      const stepAll = Math.floor(t * BPM / 60 * 4);
      if (stepAll === lastStep) return;
      lastStep = stepAll;
      const st = stepAll % 16, barN = Math.floor(stepAll / 16) % 8;
      cells.forEach((c, k) => c.classList.toggle("on", k === st));
      if (barN < 6 || barN === 7) {
        if (st % 4 === 0) hit(0);
        if (st % 4 === 2) hit(3);
        if (st === 4 || st === 12) hit(2);
        if ((st === 15 || st === 7) && barN % 2) hit(1);
      }
      if (barN === 6 && st % (st < 8 ? 2 : 1) === 0) hit(1);           // redoble antes del drop
      pads[4].classList.toggle("hit", barN === 6);                     // BUILD UP mantenido
      if (barN === 3 && st === 0) hit(6);
      if (barN === 5 && st === 8) hit(5);
    };
  };

  // Pendrives: las playlists entran desde el pendrive, en solo lectura.
  scenes.usb = (el) => {
    const ROWS = [["Peak Time", "128.00", "8A", "#6f8ff2"], ["Warm Up", "122.00", "5B", "#93cbe3"], ["Closing", "126.00", "11A", "#7FD457"],
                  ["Techno", "132.00", "2A", "#3FC7E6"], ["House", "124.00", "9B", "#A474F0"]];
    el.innerHTML = `<div class="usb"><div class="stick">
        <svg viewBox="0 0 120 220" fill="none" stroke="#fff" stroke-width="2"><rect x="30" y="4" width="60" height="44" rx="2" opacity=".55"/>
        <rect x="44" y="16" width="10" height="10" opacity=".55"/><rect x="66" y="16" width="10" height="10" opacity=".55"/>
        <rect x="14" y="48" width="92" height="166" rx="10" fill="#111"/><circle cx="60" cy="182" r="6" fill="#93cbe3" stroke="none"/></svg>
        <span class="lock"><svg viewBox="0 0 16 16" fill="none" stroke="#fff" stroke-width="1.6"><rect x="3" y="7" width="10" height="7" rx="1"/><path d="M5 7V5a3 3 0 0 1 6 0v2"/></svg></span>
      </div><div class="rows"><div class="head"><span>PLAYLISTS</span><span>BPM · KEY</span></div></div></div>`;
    const rows = el.querySelector(".rows");
    ROWS.forEach(([n, bpm, key, c], i) => {
      const r = document.createElement("div"); r.className = "row"; r.style.setProperty("--i", i); r.style.setProperty("--c", c);
      r.innerHTML = "<i></i><b></b><span></span><span></span>";
      r.children[1].textContent = n; r.children[2].textContent = bpm; r.children[3].textContent = key;
      rows.append(r);
    });
    const ro = document.createElement("div"); ro.className = "ro"; ro.textContent = "SOLO LECTURA · NADA SE ESCRIBE EN EL PENDRIVE";
    rows.append(ro);
    const box = el.querySelector(".usb");
    return { start: () => box.classList.add("run"), stop: () => box.classList.remove("run") };
  };

  // Teclado: las teclas se presionan solas y se ve qué hace cada una.
  scenes.keys = (el) => {
    const A = "a", B = "b";
    const ROWS = [
      [["1", "PLAY", A], ["2", "CUE", A], ["3", "STOP", A], ["4", "LOOP", A], ["5", "LOOP /2", A], ["6", "LOOP X2", A],
       ["7", "PLAY", B], ["8", "CUE", B], ["9", "STOP", B], ["0", "LOOP", B], ["-", "LOOP /2", B], ["=", "LOOP X2", B]],
      [["Q", "SYNC", A], ["W", "NUDGE -", A], ["E", "NUDGE +", A], ["R", "PITCH -", A], ["T", "PITCH +", A], ["Y", "PITCH 0", A],
       ["U", "SYNC", B], ["I", "NUDGE -", B], ["O", "NUDGE +", B], ["P", "PITCH -", B], ["[", "PITCH +", B], ["]", "PITCH 0", B]],
      [["A", "PAD 1", A], ["S", "PAD 2", A], ["D", "PAD 3", A], ["F", "PAD 4", A], ["G", "PAGE", A], ["H", "", ""],
       ["J", "PAD 1", B], ["K", "PAD 2", B], ["L", "PAD 3", B], [";", "PAD 4", B], ["'", "PAGE", B]],
      [["Z", "PAD 5", A], ["X", "PAD 6", A], ["C", "PAD 7", A], ["V", "PAD 8", A], ["B", "", ""], ["N", "", ""],
       ["M", "PAD 5", B], [",", "PAD 6", B], [".", "PAD 7", B], ["/", "PAD 8", B]],
      [["SPACE", "EMERGENCY PLAY", "", 6], ["←", "CROSS <", ""], ["→", "CROSS >", ""], ["↑", "PITCH -", ""], ["↓", "PITCH +", ""]],
    ];
    const kb = document.createElement("div"); kb.className = "kb";
    const keys = {};
    for (const row of ROWS) {
      const r = document.createElement("div"); r.className = "r";
      r.style.gridTemplateColumns = row.map((k) => `${k[3] || 1}fr`).join(" ");
      for (const [cap, label, deck] of row) {
        const k = document.createElement("div"); k.className = "k" + (deck ? " " + deck : "");
        k.innerHTML = "<span></span><em></em>"; k.firstChild.textContent = label; k.lastChild.textContent = cap;
        r.append(k); keys[cap] = { el: k, label, deck };
      }
      kb.append(r);
    }
    const cap = document.createElement("div"); cap.className = "kb-caption";
    cap.innerHTML = "<span>Deck <b>1</b></span><b></b>";
    el.append(kb, cap);
    const SEQ = ["1", "Q", "A", "S", "D", "F", "7", "U", "J", "K", "L", ";", "←", "→", "4", "5", "Z", "X", "M", ","];
    let last = -1;
    return (t) => {
      const i = Math.floor(t / 0.55);
      if (i === last) return;
      last = i;
      const k = keys[SEQ[i % SEQ.length]];
      k.el.classList.add("down");
      setTimeout(() => k.el.classList.remove("down"), 260);
      cap.children[0].innerHTML = k.deck ? `Deck <b>${k.deck === "a" ? 1 : 2}</b>` : "Global";
      cap.children[1].textContent = k.label;
    };
  };

  // ── Motor: cada escena corre solo mientras se ve ────────────────────────────
  const running = new Map();
  function attach(el, make) {
    const r = make(el);
    const api = typeof r === "function" ? { draw: r } : r;
    const entry = { api, t0: performance.now(), visible: false };
    if (reduce) { api.draw && api.draw(1.5); api.start && api.start(); return; }
    const watch = new IntersectionObserver(([e]) => {
      entry.visible = e.isIntersecting;
      if (entry.visible) { api.start && api.start(); running.set(el, entry); }
      else { api.stop && api.stop(); running.delete(el); }
    }, { threshold: 0.05 });
    watch.observe(el);
  }
  const heroCanvas = document.querySelector(".hero canvas.wave");
  if (heroCanvas) attach(heroCanvas, scenes.hero);
  const screenFx = document.querySelector(".hero canvas.screen-fx");
  if (screenFx) attach(screenFx, scenes.screenfx);
  document.querySelectorAll("[data-scene]").forEach((el) => {
    const make = scenes[el.dataset.scene];
    if (make) attach(el, make);
  });

  function loop(now) {
    if (!document.hidden)
      for (const entry of running.values())
        if (entry.api.draw) entry.api.draw((now - entry.t0) / 1000);
    requestAnimationFrame(loop);
  }
  if (!reduce) requestAnimationFrame(loop);
})();
