// Portada: lista de novedades con los últimos temas del foro.

(async () => {
  const { el, api } = OM;
  const host = document.getElementById("news");
  if (!host) return;
  const ARROW = () => {
    const s = document.createElementNS("http://www.w3.org/2000/svg", "svg");
    s.setAttribute("viewBox", "0 0 16 16");
    s.setAttribute("class", "arrow");
    s.innerHTML = '<path d="M2 8h11M9 4l4 4-4 4" fill="none" stroke="currentColor" stroke-width="1.3"/>';
    return s;
  };
  const fmt = new Intl.DateTimeFormat("es", { day: "numeric", month: "long", year: "numeric" });

  try {
    const [{ threads }, { categories }] = await Promise.all([api("api/forum/threads"), api("api/forum/categories")]);
    const name = Object.fromEntries(categories.map((c) => [c.id, c.name]));
    if (!threads.length) {
      host.replaceChildren(el("li", { class: "row", text: "Todavía no hay temas. Sé el primero en escribir en el foro." }));
      return;
    }
    host.replaceChildren(...threads.slice(0, 6).map((t) => el("li", {},
      el("a", { href: `foro.html#t=${encodeURIComponent(t.id)}` },
        el("span", { class: "tag" + (t.category === "anuncios" ? " accent" : ""), text: name[t.category] || t.category }),
        el("span", { class: "date", text: fmt.format(new Date(t.activity)) }),
        el("span", { class: "title", text: t.title }),
        ARROW()))));
  } catch {
    // Sitio estático: la novedad es la versión publicada; "Ver todo" lleva a GitHub.
    const cfg = await OM.site();
    const releases = await OM.repoUrl("/releases");
    const discussions = await OM.repoUrl("/discussions");
    const all = document.querySelector(".news .all a");
    if (all && discussions) { all.href = discussions; all.target = "_blank"; all.rel = "noopener"; }
    if (!cfg.version) {
      host.replaceChildren(el("li", { class: "row", text: "Pronto vas a ver las novedades acá." }));
      return;
    }
    host.replaceChildren(el("li", {},
      el("a", { href: releases ? `${releases}/tag/v${cfg.version}` : "#descarga", target: releases ? "_blank" : null, rel: "noopener" },
        el("span", { class: "tag accent", text: "Versión" }),
        el("span", { class: "date", text: cfg.released ? fmt.format(new Date(cfg.released + "T12:00:00")) : "" }),
        el("span", { class: "title", text: `OpenMix ${cfg.version} disponible para macOS` }),
        ARROW())));
  }
})();
