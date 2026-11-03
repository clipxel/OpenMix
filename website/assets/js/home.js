// Portada: Noticias. Las entradas se escriben en config/news.json (de la más nueva a la más vieja);
// se suma sola la última versión publicada (config/downloads.json).

(async () => {
  const { el } = OM;
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
  const day = (iso) => (iso ? fmt.format(new Date(iso + "T12:00:00")) : "");

  const [cfg, list] = await Promise.all([
    OM.site(),
    fetch("config/news.json", { cache: "no-store" }).then((r) => r.json()).catch(() => ({ news: [] })),
  ]);
  const releases = await OM.repoUrl("/releases");

  const items = (list.news || []).map((n, i) => ({ ...n, order: i }));
  if (cfg.version)
    items.push({ date: cfg.released, tag: "Versión", title: `OpenMix ${cfg.version} disponible`,
                 text: "Descargala gratis para macOS, Windows y Linux.",
                 link: releases ? `${releases}/tag/v${cfg.version}` : "#descarga", order: 999 });
  items.sort((a, b) => (b.date || "").localeCompare(a.date || "") || a.order - b.order);

  if (!items.length) {
    host.replaceChildren(el("li", { class: "row", text: "Pronto vas a ver las novedades acá." }));
    return;
  }
  host.replaceChildren(...items.slice(0, 8).map((n) => {
    const external = n.link && /^https?:/.test(n.link);
    const body = [
      el("span", { class: "tag" + (n.tag === "Próximamente" ? " soon" : n.tag === "Versión" ? "" : " accent"), text: n.tag || "Novedad" }),
      el("span", { class: "date", text: day(n.date) }),
      el("span", { class: "title" }, el("b", { text: n.title }), n.text ? el("small", { text: n.text }) : null),
      n.link ? ARROW() : el("span"),
    ];
    return el("li", {}, n.link
      ? el("a", { href: n.link, target: external ? "_blank" : null, rel: external ? "noopener" : null }, ...body)
      : el("div", { class: "news-row" }, ...body));
  }));

  const all = document.querySelector(".news .all a");
  if (all && releases) { all.href = releases; all.target = "_blank"; all.rel = "noopener"; all.textContent = "Todas las versiones"; }
})();
