// Foro: categorías, lista de hilos y vista de hilo. La navegación usa el hash
// (#c=ayuda, #t=<id>) para que los enlaces a un hilo se puedan compartir.

(() => {
  const { el, api, avatar, timeAgo, plural, store } = OM;
  const $ = (id) => document.getElementById(id);
  let categories = [];
  let current = { category: "", thread: "" };
  let timer = 0;

  const catName = (id) => (categories.find((c) => c.id === id) || {}).name || id;

  function parseHash() {
    const h = new URLSearchParams(location.hash.slice(1));
    current = { category: h.get("c") || "", thread: h.get("t") || "" };
  }

  async function loadCategories() {
    try {
      ({ categories } = await api("api/forum/categories"));
    } catch (e) {
      if (e.offline) { await staticForum(); return false; }
      $("threads").replaceChildren(el("div", { class: "empty" }, el("strong", { text: "El foro no está disponible" }), e.message));
      $("new-thread").disabled = true;
      return false;
    }
    const total = categories.reduce((n, c) => n + c.threads, 0);
    const btn = (id, name, n) => el("button", { class: current.category === id && !current.thread ? "on" : "",
      onclick: () => { location.hash = id ? `c=${id}` : ""; } },
      el("span", { text: name }), el("span", { class: "n", text: n }));
    $("cats").replaceChildren(btn("", "Todos los temas", total), ...categories.map((c) => btn(c.id, c.name, c.threads)));
    $("compose-cat").replaceChildren(...categories.filter((c) => c.id !== "anuncios")
      .map((c) => el("option", { value: c.id, text: c.name })));
    return true;
  }

  async function showList() {
    $("view-list").hidden = false;
    $("view-thread").hidden = true;
    const params = new URLSearchParams({ q: $("search").value.trim() });
    if (current.category) params.set("category", current.category);
    try {
      const { threads } = await api(`api/forum/threads?${params}`);
      if (!threads.length) {
        $("threads").replaceChildren(el("div", { class: "empty" },
          el("strong", { text: $("search").value.trim() ? "No hay temas que coincidan." : "Todavía no hay temas acá." }),
          "Arrancá la conversación con “Nuevo tema”."));
        return;
      }
      $("threads").replaceChildren(...threads.map((t) => el("a", { class: "item", href: `#t=${t.id}` },
        el("div", {},
          el("div", { class: "title" },
            t.pinned ? el("span", { class: "pill official" }, "Fijado") : null,
            el("span", { text: t.title })),
          el("div", { class: "sub", text: `${catName(t.category)} · ${t.author} · ${timeAgo(t.created)}` }),
          el("p", { class: "desc", style: "display:-webkit-box;-webkit-line-clamp:2;-webkit-box-orient:vertical;overflow:hidden", text: t.excerpt })),
        el("div", { class: "side" },
          el("span", { class: "stat", text: plural(t.replies, "respuesta", "respuestas") }),
          el("span", { class: "stat", text: "· " + timeAgo(t.activity) })))));
    } catch (e) {
      $("threads").replaceChildren(el("div", { class: "empty" }, el("strong", { text: "No se pudieron cargar los temas" }), e.message));
    }
  }

  function post(author, body, created, isOp) {
    return el("article", { class: "post" },
      el("div", { class: "who" }, avatar(author), el("b", { text: author }), el("span", { text: timeAgo(created) }),
        isOp ? el("span", { class: "pill" }, "Autor") : null),
      el("div", { class: "body", text: body }));
  }

  async function showThread() {
    $("view-list").hidden = true;
    $("view-thread").hidden = false;
    $("reply-msg").hidden = true;
    $("reply-form").author.value = store.get("om-author");
    try {
      const { thread: t } = await api(`api/forum/threads/${encodeURIComponent(current.thread)}`);
      document.title = `${t.title} — Foro OpenMix`;
      $("thread").replaceChildren(
        el("div", { class: "post" },
          el("p", { class: "label", text: catName(t.category) }),
          el("h2", { style: "font-size:24px", text: t.title })),
        post(t.author, t.body, t.created, false),
        ...t.replies.map((r) => post(r.author, r.body, r.created, r.author === t.author)));
      $("reply-form").hidden = false;
    } catch (e) {
      $("thread").replaceChildren(el("div", { class: "empty" }, el("strong", { text: "No se pudo abrir el tema" }), e.message));
      $("reply-form").hidden = true;
    }
  }

  // Sitio estático (GitHub Pages): el foro vive en GitHub Discussions del repositorio.
  async function staticForum() {
    const discussions = await OM.repoUrl("/discussions");
    $("cats").hidden = true;
    $("search").hidden = true;
    $("view-thread").hidden = true;
    $("view-list").hidden = false;
    const nt = $("new-thread");
    if (discussions) nt.replaceWith(el("a", { class: nt.className, href: `${discussions}/new/choose`, target: "_blank", rel: "noopener" }, "Nuevo tema"));
    else nt.hidden = true;
    $("threads").replaceChildren(el("div", { class: "empty" },
      el("strong", { text: "El foro de OpenMix está en GitHub Discussions" }),
      "Preguntas, ideas, errores, mapeos y técnica de mezcla, con tu cuenta de GitHub.",
      discussions ? el("a", { class: "more", href: discussions, target: "_blank", rel: "noopener", style: "justify-self:center;margin-top:12px" }, "Entrar al foro") : null));
  }

  async function route() {
    parseHash();
    document.title = "Foro — OpenMix";
    if (!(await loadCategories())) return;
    if (current.thread) showThread(); else showList();
    window.scrollTo({ top: 0 });
  }

  $("search").addEventListener("input", () => { clearTimeout(timer); timer = setTimeout(showList, 200); });
  $("back").addEventListener("click", () => { history.length > 1 ? history.back() : (location.hash = ""); });
  window.addEventListener("hashchange", route);

  // ── Responder ─────────────────────────────────────────────────────────────
  $("reply-form").addEventListener("submit", async (e) => {
    e.preventDefault();
    const f = e.target.elements;
    const msg = $("reply-msg");
    if (!f.author.value.trim() || !f.body.value.trim()) {
      msg.textContent = "Escribí tu nombre y un mensaje."; msg.className = "msg error"; msg.hidden = false; return;
    }
    try {
      await api(`api/forum/threads/${encodeURIComponent(current.thread)}/replies`, { method: "POST",
        body: { author: f.author.value, body: f.body.value, website: f.website.value } });
      store.set("om-author", f.author.value.trim());
      f.body.value = "";
      await showThread();
      window.scrollTo({ top: document.body.scrollHeight, behavior: "smooth" });
    } catch (err) {
      msg.textContent = err.message; msg.className = "msg error"; msg.hidden = false;
    }
  });

  // ── Nuevo tema ────────────────────────────────────────────────────────────
  const compose = $("compose");
  const cform = $("compose-form");
  const cmsg = $("compose-msg");
  $("new-thread").addEventListener("click", () => {
    cform.reset();
    cmsg.hidden = true;
    cform.author.value = store.get("om-author");
    if (current.category && current.category !== "anuncios") cform.category.value = current.category;
    compose.hidden = false;
    cform.title.focus();
  });
  compose.addEventListener("click", (e) => { if (e.target === compose || e.target.closest("[data-close]")) compose.hidden = true; });
  document.addEventListener("keydown", (e) => { if (e.key === "Escape") compose.hidden = true; });

  cform.addEventListener("submit", async (e) => {
    e.preventDefault();
    const f = cform.elements;
    if (!f.author.value.trim() || !f.title.value.trim() || !f.body.value.trim()) {
      cmsg.textContent = "Completá nombre, título y mensaje."; cmsg.className = "msg error"; cmsg.hidden = false; return;
    }
    try {
      const { thread } = await api("api/forum/threads", { method: "POST", body: {
        category: f.category.value, author: f.author.value, title: f.title.value, body: f.body.value, website: f.website.value } });
      store.set("om-author", f.author.value.trim());
      compose.hidden = true;
      location.hash = `t=${thread.id}`;
    } catch (err) {
      cmsg.textContent = err.message; cmsg.className = "msg error"; cmsg.hidden = false;
    }
  });

  route();
})();
