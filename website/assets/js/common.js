// Utilidades compartidas por todas las páginas. Todo el texto que viene de usuarios
// se inserta con textContent (nunca innerHTML).

const OM = (() => {
  // Colores de hot cue del programa (Theme::hotCueColour), usados para los avatares.
  const CUE_COLOURS = ["#EE5A52", "#7FD457", "#3FC7E6", "#5C7CF0", "#E064C4", "#F0B347", "#2ED1B0", "#A474F0"];

  function el(tag, attrs = {}, ...children) {
    const node = document.createElement(tag);
    for (const [k, v] of Object.entries(attrs || {})) {
      if (v === null || v === undefined || v === false) continue;
      if (k === "class") node.className = v;
      else if (k === "text") node.textContent = v;
      else if (k.startsWith("on") && typeof v === "function") node.addEventListener(k.slice(2), v);
      else node.setAttribute(k, v === true ? "" : v);
    }
    for (const c of children.flat()) {
      if (c === null || c === undefined || c === false) continue;
      node.append(c instanceof Node ? c : document.createTextNode(String(c)));
    }
    return node;
  }

  async function api(path, options = {}) {
    const init = { ...options, headers: { ...(options.headers || {}) } };
    if (init.body && typeof init.body !== "string") {
      init.body = JSON.stringify(init.body);
      init.headers["Content-Type"] = "application/json";
    }
    // Sin servidor (p. ej. GitHub Pages) la API no existe: el error lleva offline = true y cada
    // página muestra su versión estática.
    const offline = (msg) => Object.assign(new Error(msg), { offline: true });
    let res;
    try {
      res = await fetch(path, init);
    } catch {
      throw offline("No se pudo conectar con el servidor de OpenMix.");
    }
    const data = await res.json().catch(() => null);
    if (data === null) throw offline("Esta sección necesita el servidor de OpenMix.");
    if (!res.ok) throw new Error(data.error || `Error ${res.status}`);
    return data;
  }

  /** config/downloads.json (versión, repositorio, descargas), leído una sola vez. */
  let siteConfig = null;
  function site() {
    siteConfig = siteConfig || fetch("config/downloads.json", { cache: "no-store" }).then((r) => r.json()).catch(() => ({}));
    return siteConfig;
  }
  async function repoUrl(path = "") {
    const { repo } = await site();
    return repo ? `https://github.com/${repo}${path}` : "";
  }

  /** "mac" | "windows" | "linux" | "mobile" | "other" */
  function detectOS() {
    const uaData = navigator.userAgentData;
    const platform = ((uaData && uaData.platform) || navigator.platform || "").toLowerCase();
    const ua = navigator.userAgent.toLowerCase();

    if (/iphone|ipad|ipod|android/.test(ua) || (uaData && uaData.mobile)) return "mobile";
    // iPadOS se presenta como Mac; lo delata la pantalla táctil.
    if ((platform.includes("mac") || ua.includes("mac os x")) && navigator.maxTouchPoints > 1) return "mobile";
    if (platform.includes("mac") || ua.includes("mac os x") || ua.includes("macintosh")) return "mac";
    if (platform.includes("win") || ua.includes("windows")) return "windows";
    if (platform.includes("linux") || ua.includes("linux") || ua.includes("x11") || ua.includes("cros")) return "linux";
    return "other";
  }

  const OS_NAMES = { mac: "macOS", windows: "Windows", linux: "Linux" };

  function avatar(name) {
    let h = 0;
    for (const ch of name) h = (h * 31 + ch.codePointAt(0)) >>> 0;
    const initials = name.trim().split(/\s+/).slice(0, 2).map((w) => w[0]).join("").toUpperCase() || "?";
    return el("span", { class: "avatar", style: `background:${CUE_COLOURS[h % CUE_COLOURS.length]}`, text: initials });
  }

  function timeAgo(iso) {
    const s = Math.max(0, (Date.now() - new Date(iso).getTime()) / 1000);
    if (s < 60) return "recién";
    const units = [[31536000, "año", "años"], [2592000, "mes", "meses"], [86400, "día", "días"], [3600, "hora", "horas"], [60, "minuto", "minutos"]];
    for (const [sec, one, many] of units)
      if (s >= sec) { const n = Math.floor(s / sec); return `hace ${n} ${n === 1 ? one : many}`; }
    return "recién";
  }

  function plural(n, one, many) { return `${n} ${n === 1 ? one : many}`; }

  // Nombre de usuario recordado entre visitas (solo comodidad local).
  const store = {
    get(k) { try { return localStorage.getItem(k) || ""; } catch { return ""; } },
    set(k, v) { try { localStorage.setItem(k, v); } catch { /* sin almacenamiento */ } },
  };

  function initChrome() {
    const toggle = document.querySelector(".menu-toggle");
    const nav = document.querySelector(".nav");
    if (toggle && nav) toggle.addEventListener("click", () => nav.classList.toggle("open"));
    const year = document.getElementById("year");
    if (year) year.textContent = new Date().getFullYear();
  }

  document.addEventListener("DOMContentLoaded", initChrome);

  return { el, api, site, repoUrl, detectOS, OS_NAMES, avatar, timeAgo, plural, store };
})();
