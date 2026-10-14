// Descarga: detecta el sistema operativo y apunta el botón principal al instalador correcto.
// Las URLs viven en config/downloads.json; una plataforma con "url": null se muestra como
// "próximamente" en lugar de ofrecer un archivo que no existe.

(async () => {
  const { el, detectOS, OS_NAMES } = OM;
  const os = detectOS();
  const MONITOR = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linejoin="round"><rect x="3" y="4" width="18" height="12" rx="1.5"/><path d="M9 20h6M12 16v4"/></svg>';
  const ARROW ='<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"><path d="M8 2v9M4 7l4 4 4-4M3 14h10"/></svg>';

  let config;
  try {
    const res = await fetch("config/downloads.json", { cache: "no-store" });
    config = await res.json();
  } catch {
    document.querySelectorAll("[data-download-main]").forEach((b) => {
      b.textContent = "Descargas no disponibles";
      b.setAttribute("aria-disabled", "true");
    });
    return;
  }

  const p = config.platforms;
  // "release": true → el instalador está en GitHub Releases (repo de config.repo, tag v<versión>).
  for (const info of Object.values(p))
    if (info && info.release)
      info.url = config.repo ? `https://github.com/${config.repo}/releases/download/v${config.version}/${info.file}` : null;
  const available = (key) => p[key] && p[key].url;

  // ── Botón principal (hero y sección de descarga) ──────────────────────────
  document.querySelectorAll("[data-download-main]").forEach((btn) => {
    const meta = btn.parentElement.querySelector("[data-download-meta]");
    const notice = btn.parentElement.querySelector("[data-download-notice]");

    if (available(os)) {
      btn.href = p[os].url;
      btn.setAttribute("download", p[os].file);
      btn.innerHTML = `${ARROW}<span></span>`;
      btn.querySelector("span").textContent = `Descargar para ${OS_NAMES[os]}`;
      if (meta) meta.textContent = [`v${config.version}`, p[os].size, p[os].requirements].filter(Boolean).join(" · ");
      const firstRun = {
        mac: "Primera vez: si macOS avisa que no puede verificar OpenMix, andá a Ajustes del Sistema › Privacidad y seguridad › Abrir igualmente.",
        windows: "Abrí el instalador y seguí los pasos (no pide permisos de administrador). Si Windows muestra “Windows protegió su PC”, hacé clic en Más información › Ejecutar de todas formas.",
        linux: "Descomprimí el archivo y ejecutá ./instalar.sh: OpenMix aparece en tus aplicaciones.",
      };
      if (notice) notice.textContent = firstRun[os] || "";
      return;
    }

    btn.href = "#descarga";
    btn.removeAttribute("download");
    if (os === "mobile") {
      btn.textContent = "OpenMix es para computadora";
      if (notice) notice.textContent = "Abrí esta página desde tu Mac, PC o Linux para descargar la versión correcta.";
    } else if (os === "windows" || os === "linux") {
      btn.textContent = `${OS_NAMES[os]}: próximamente`;
      if (notice) notice.textContent = `Todavía no hay versión para ${OS_NAMES[os]}. Por ahora OpenMix está disponible para macOS.`;
    } else {
      btn.textContent = "Elegí tu sistema";
    }
    if (meta) meta.textContent = `v${config.version}`;
  });

  // ── Tarjetas por plataforma ───────────────────────────────────────────────
  const host = document.getElementById("platforms");
  if (!host) return;
  for (const key of ["mac", "windows", "linux"]) {
    const info = p[key];
    const ready = available(key);
    const action = ready
      ? el("a", { class: key === os ? "btn primary" : "btn", href: info.url, download: info.file }, `Descargar${info.size ? " · " + info.size : ""}`)
      : el("span", { class: "btn", "aria-disabled": "true" }, "Próximamente");

    const title = el("h3", {}, OS_NAMES[key]);
    title.insertAdjacentHTML("afterbegin", MONITOR);
    host.append(
      el("div", { class: "platform" + (key === os ? " current" : "") },
        title,
        key === os ? el("span", { class: "pill" }, el("span", { class: "dot" }), "Tu sistema") : null,
        action,
        el("p", { class: "req", text: info.requirements }),
        el("p", { class: "file", text: ready ? info.file : "Sin compilación publicada" })));
  }

  // ?descargar en la URL inicia la descarga directamente (para enlaces cortos).
  if (new URLSearchParams(location.search).has("descargar") && available(os)) {
    const a = el("a", { href: p[os].url, download: p[os].file });
    document.body.append(a);
    a.click();
    a.remove();
  }
})();
