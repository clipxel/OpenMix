// Biblioteca de mapeos: lista oficiales + comunidad, filtra, descarga y sube.

(() => {
  const { el, api, timeAgo, plural, store } = OM;
  const list = document.getElementById("list");
  const search = document.getElementById("search");
  const sort = document.getElementById("sort");
  const filter = document.getElementById("filter");
  let type = "";
  let timer = 0;

  async function refresh() {
    const params = new URLSearchParams({ q: search.value.trim(), sort: sort.value });
    if (type) params.set("type", type);
    try {
      const { mappings } = await api(`api/mappings?${params}`);
      render(mappings);
    } catch (e) {
      if (e.offline) return staticMode();
      list.replaceChildren(el("div", { class: "empty" }, el("strong", { text: "No se pudieron cargar los mapeos" }), e.message));
    }
  }

  // Sitio estático (GitHub Pages): los oficiales salen de official/index.json y los de la
  // comunidad se comparten en GitHub Discussions.
  let staticItems = null;
  async function staticMode() {
    document.body.classList.add("static-site");
    const discussions = await OM.repoUrl("/discussions");
    if (staticItems === null) {
      try {
        const idx = await fetch("official/index.json", { cache: "no-store" }).then((r) => r.json());
        staticItems = (idx.mappings || []).map((m) => ({ ...m, official: true, downloads: null, href: `official/${m.file}` }));
      } catch { staticItems = []; }
      const up = document.getElementById("open-upload");
      if (discussions) {
        const link = el("a", { class: up.className, href: discussions, target: "_blank", rel: "noopener" }, "Compartir en GitHub");
        up.replaceWith(link);
      } else up.hidden = true;
    }
    const q = search.value.trim().toLowerCase();
    const items = type === "community" ? [] : staticItems.filter((m) =>
      !q || `${m.brand} ${m.model} ${m.name} ${m.author} ${m.description || ""}`.toLowerCase().includes(q));
    render(items, type === "community"
      ? el("div", { class: "empty" }, el("strong", { text: "Los mapeos de la comunidad se comparten en GitHub" }),
          discussions ? el("a", { class: "more", href: discussions, target: "_blank", rel: "noopener", style: "justify-self:center;margin-top:10px" }, "Ver mapeos de la comunidad") : null)
      : null);
  }

  function render(items, emptyNode = null) {
    if (!items.length && emptyNode) return list.replaceChildren(emptyNode);
    if (!items.length) {
      const searching = search.value.trim();
      const msg = searching ? "Ningún mapeo coincide con la búsqueda."
        : type === "official" ? "Todavía no hay mapeos oficiales publicados."
        : type === "community" ? "Todavía nadie compartió un mapeo."
        : "Todavía no hay mapeos publicados.";
      list.replaceChildren(el("div", { class: "empty" },
        el("strong", { text: msg }),
        searching ? null : "¿Tenés uno armado? Subilo y aparece acá."));
      return;
    }

    list.replaceChildren(...items.map((m) => el("div", { class: "item" },
      el("div", {},
        el("div", { class: "title" },
          el("span", { text: m.name }),
          m.official ? el("span", { class: "pill official" }, el("span", { class: "dot" }), "Oficial")
                     : el("span", { class: "pill" }, "Comunidad")),
        el("div", { class: "sub" },
          `${m.brand} · por ${m.author} · ${plural(m.bindings, "asignación", "asignaciones")} · ${plural(m.actions, "función", "funciones")}`
          + (m.created ? ` · ${timeAgo(m.created)}` : "")),
        m.devices && m.devices.length ? el("div", { class: "sub", text: "Dispositivo: " + m.devices.join(", ") }) : null,
        m.description ? el("p", { class: "desc", text: m.description }) : null),
      el("div", { class: "side" },
        m.downloads === null ? null : el("span", { class: "stat", text: plural(m.downloads || 0, "descarga", "descargas") }),
        m.href ? el("a", { class: "btn small", href: m.href, download: `openmix-${m.id}.midi.json` }, "Descargar")
               : el("a", { class: "btn small", href: `api/mappings/${encodeURIComponent(m.id)}/download`,
                           onclick: () => setTimeout(refresh, 800) }, "Descargar")))));
  }

  filter.addEventListener("click", (e) => {
    const b = e.target.closest("button");
    if (!b) return;
    filter.querySelectorAll("button").forEach((x) => x.classList.toggle("on", x === b));
    type = b.dataset.type;
    refresh();
  });
  search.addEventListener("input", () => { clearTimeout(timer); timer = setTimeout(refresh, 200); });
  sort.addEventListener("change", refresh);

  // ── Subida ────────────────────────────────────────────────────────────────
  const dialog = document.getElementById("upload");
  const form = document.getElementById("upload-form");
  const drop = document.getElementById("drop");
  const fileInput = document.getElementById("file");
  const msg = document.getElementById("upload-msg");
  const submit = document.getElementById("upload-submit");
  let mappingText = "";

  function show(text, kind) { msg.textContent = text; msg.className = "msg " + (kind || ""); msg.hidden = !text; }

  function open() {
    form.reset();
    mappingText = "";
    drop.classList.remove("ok");
    drop.innerHTML = "Arrastrá tu <strong>midi.json</strong> acá o hacé clic para elegirlo";
    form.author.value = store.get("om-author");
    show("");
    dialog.hidden = false;
    form.brand.focus();
  }
  function close() { dialog.hidden = true; }

  document.getElementById("open-upload").addEventListener("click", open);
  dialog.addEventListener("click", (e) => { if (e.target === dialog || e.target.closest("[data-close]")) close(); });
  document.addEventListener("keydown", (e) => { if (e.key === "Escape" && !dialog.hidden) close(); });

  async function takeFile(file) {
    if (!file) return;
    if (file.size > 512 * 1024) return show("El archivo supera los 512 KB.", "error");
    const text = await file.text();
    try {
      const doc = JSON.parse(text);
      if (!doc || !Array.isArray(doc.bindings) || !doc.bindings.length) throw new Error();
      mappingText = text;
      drop.classList.add("ok");
      drop.replaceChildren(el("strong", { text: file.name }), ` · ${plural(doc.bindings.length, "asignación", "asignaciones")}`);
      show("");
    } catch {
      mappingText = "";
      drop.classList.remove("ok");
      show("Ese archivo no es un midi.json de OpenMix.", "error");
    }
  }

  drop.addEventListener("click", () => fileInput.click());
  drop.addEventListener("keydown", (e) => { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); fileInput.click(); } });
  fileInput.addEventListener("change", () => takeFile(fileInput.files[0]));
  drop.addEventListener("dragover", (e) => { e.preventDefault(); drop.classList.add("over"); });
  drop.addEventListener("dragleave", () => drop.classList.remove("over"));
  drop.addEventListener("drop", (e) => { e.preventDefault(); drop.classList.remove("over"); takeFile(e.dataTransfer.files[0]); });

  form.addEventListener("submit", async (e) => {
    e.preventDefault();
    const f = form.elements;
    if (!f.brand.value.trim() || !f.model.value.trim() || !f.author.value.trim())
      return show("Completá marca, modelo y tu nombre.", "error");
    if (!mappingText) return show("Adjuntá el archivo midi.json.", "error");

    submit.disabled = true;
    try {
      await api("api/mappings", { method: "POST", body: {
        brand: f.brand.value, model: f.model.value, author: f.author.value,
        description: f.description.value, mapping: mappingText, website: f.website.value } });
      store.set("om-author", f.author.value.trim());
      close();
      type = "community";
      filter.querySelectorAll("button").forEach((x) => x.classList.toggle("on", x.dataset.type === "community"));
      search.value = "";
      sort.value = "recent";
      refresh();
    } catch (err) {
      show(err.message, "error");
    } finally {
      submit.disabled = false;
    }
  });

  refresh();
})();
