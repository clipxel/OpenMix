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

  // macOS: no se ofrece el .dmg. El botón copia el comando de la Terminal y abre un tutorial.
  const INSTALL_CMD = "curl -fsSL https://openmix.pages.dev/install.sh | sh";
  async function copyCmd() {
    try { await navigator.clipboard.writeText(INSTALL_CMD); return true; }
    catch {
      const t = el("textarea", { readonly: "" }); t.value = INSTALL_CMD;
      t.style.cssText = "position:fixed;opacity:0"; document.body.append(t); t.select();
      let ok = false; try { ok = document.execCommand("copy"); } catch {} t.remove(); return ok;
    }
  }
  let dialog;
  function macDialog() {
    if (dialog) return dialog;
    const copy = el("button", { class: "copy", type: "button" }, "Copiar");
    const status = el("p", { class: "mi-status" });
    copy.addEventListener("click", async () => {
      const ok = await copyCmd();
      copy.textContent = ok ? "Copiado" : "Copiar";
      status.textContent = ok ? "Código copiado. Ahora pegalo en la Terminal." : "Seleccioná el código y copialo con ⌘C.";
    });
    const step = (n, title, body) => el("li", {}, el("span", { class: "mi-n", text: n }), el("div", {}, el("b", { text: title }), body));
    dialog = el("dialog", { class: "mac-install", "aria-labelledby": "mi-title" },
      el("button", { class: "mi-close", type: "button", "aria-label": "Cerrar" }, "×"),
      el("span", { class: "kicker", text: "Instalar en macOS" }),
      el("h2", { id: "mi-title", text: "Tres pasos y listo" }),
      el("p", { class: "mi-lead", text: "En Mac, OpenMix se instala pegando un código en la Terminal. Es más rápido y sin avisos de seguridad." }),
      el("ol", { class: "mi-steps" },
        step("1", "Abrí la Terminal", el("p", {}, "Apretá ", el("kbd", { text: "⌘" }), " ", el("kbd", { text: "Espacio" }), ", escribí ", el("i", { text: "Terminal" }), " y apretá ", el("kbd", { text: "Enter" }), ".")),
        step("2", "Pegá el código", el("div", {},
          el("p", {}, "Ya lo copiamos por vos. Pegalo con ", el("kbd", { text: "⌘" }), " ", el("kbd", { text: "V" }), "."),
          el("div", { class: "terminal-install" }, el("div", { class: "cmd" }, el("code", { text: INSTALL_CMD }), copy)),
          status)),
        step("3", "Apretá Enter", el("p", { text: "OpenMix se descarga, se instala en Aplicaciones y se abre solo. Para actualizarlo más adelante, repetí los mismos pasos." }))),
      el("button", { class: "btn primary mi-done", type: "button" }, "Entendido"));
    dialog.addEventListener("click", (e) => {
      if (e.target === dialog || e.target.closest(".mi-close, .mi-done")) dialog.close();
    });
    document.body.append(dialog);
    return dialog;
  }
  async function openMacInstall(e) {
    if (e) e.preventDefault();
    const d = macDialog();
    const ok = await copyCmd();
    d.querySelector(".copy").textContent = ok ? "Copiado" : "Copiar";
    d.querySelector(".mi-status").textContent = ok ? "Código copiado. Ahora pegalo en la Terminal." : "Tocá Copiar, o seleccioná el código y copialo con ⌘C.";
    if (!d.open) d.showModal();
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

    if (os === "mac" && available("mac")) {
      btn.href = "#descarga";
      btn.removeAttribute("download");
      btn.innerHTML = `${ARROW}<span>Descargar para macOS</span>`;
      btn.addEventListener("click", openMacInstall);
      if (meta) meta.textContent = [`v${config.version}`, p.mac.requirements].filter(Boolean).join(" · ");
      if (notice) notice.textContent = "Se instala pegando un código en la Terminal: al tocar Descargar te lo copiamos y te mostramos cómo.";
      return;
    }

    if (available(os)) {
      btn.href = p[os].url;
      btn.setAttribute("download", p[os].file);
      btn.innerHTML = `${ARROW}<span></span>`;
      btn.querySelector("span").textContent = `Descargar para ${OS_NAMES[os]}`;
      if (meta) meta.textContent = [`v${config.version}`, p[os].size, p[os].requirements].filter(Boolean).join(" · ");
      const firstRun = {
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
    const action = ready && key === "mac"
      ? el("a", { class: key === os ? "btn primary" : "btn", href: "#descarga", onclick: openMacInstall }, "Instalar con la Terminal")
      : ready
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
        el("p", { class: "file", text: !ready ? "Sin compilación publicada" : key === "mac" ? "Con un código en la Terminal" : info.file })));
  }

  // ?descargar en la URL inicia la descarga directamente (para enlaces cortos).
  if (new URLSearchParams(location.search).has("descargar") && os === "mac" && available("mac")) {
    openMacInstall();
  } else if (new URLSearchParams(location.search).has("descargar") && available(os)) {
    const a = el("a", { href: p[os].url, download: p[os].file });
    document.body.append(a);
    a.click();
    a.remove();
  }
})();
