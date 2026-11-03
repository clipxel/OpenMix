// Atención al usuario: formulario "¿Necesitás ayuda?" y botón flotante de ayuda.
// El mensaje llega al mismo formulario de reportes que usa el programa (Google Forms).

(() => {
  const CONTROLLERS = ["Akai MPD24", "AlphaTheta OMNIS-DUO", "AlphaTheta XDJ-AZ", "American Audio VMS2", "American Audio VMS4/4.1", "Behringer BCD2000", "Behringer BCD3000", "Behringer BCD3000 Advanced", "Behringer CMD Micro", "Behringer CMD STUDIO 4a", "DDJ-REV1", "DDJ-RX", "DDJ-SX2", "DDJ-SZ", "Denon DN SC2000", "Denon HS5500", "Denon MC3000", "Denon MC4000", "Denon MC6000MK2", "Denon MC7000", "Denon Prime 4", "Denon Prime GO", "Denon SC Live 4", "DJ-Tech i-Mix Reload", "DJ-Tech MIX-101", "Electrix Tweaker", "Evolution X-Session", "FaderFox DJ2", "Gemini FirstMix", "Hercules DJ Console 4-Mx", "Hercules DJ Console Mac Edition", "Hercules DJ Console Mk2", "Hercules DJ Console MK4", "Hercules DJ Console RMX", "Hercules DJ Console RMX Advanced", "Hercules DJ Console RMX2", "Hercules DJ Control AIR", "Hercules DJ Control Instinct", "Hercules DJ Control MP3", "Hercules DJ Control MP3 e2 / MP3 LE / Glow", "Hercules DJ Control Steel", "Hercules DJControl Compact", "Hercules DJControl Inpulse 200", "Hercules DJControl Inpulse 300", "Hercules DJControl Inpulse 500", "Hercules DJControl Jogvision", "Hercules DJControl MIX", "Hercules DJControl Starlight", "Hercules P32 DJ", "Ion Discover DJ", "Ion Discover DJ Pro", "Kontrol DJ KDJ500", "Korg KAOSS DJ", "Korg nanoKONTROL", "M-Audio X-Session Pro", "M-Audio Xponent (Advanced)", "Mixman DM2 (Linux)", "MixVibes U-Mix Control 2", "MixVibes U-Mix Control Pro 2", "Numark DJ2Go", "Numark DJ2GO2 Touch", "Numark Mixstream Pro", "Numark MIXTRACK", "Numark Mixtrack (Pro) 3", "Numark MixTrack (Pro) II", "Numark Mixtrack Go", "Numark Mixtrack Platinum", "Numark Mixtrack Platinum FX", "Numark MixTrack Pro", "Numark Mixtrack Pro FX", "Numark N4", "Numark NS4FX", "Numark NS6II", "Numark NS7", "Numark NV", "Numark Omni Control", "Numark Party Mix", "Numark Party Mix MKII", "Numark Total Control", "Numark V7", "Pioneer CDJ-2000", "Pioneer CDJ-850", "Pioneer DDJ-1000", "Pioneer DDJ-200", "Pioneer DDJ-400", "Pioneer DDJ-800", "Pioneer DDJ-ERGO", "Pioneer DDJ-FLX10", "Pioneer DDJ-FLX2", "Pioneer DDJ-FLX4", "Pioneer DDJ-FLX6", "Pioneer DDJ-GRV6", "Pioneer DDJ-RB", "Pioneer DDJ-REV5", "Pioneer DDJ-RR", "Pioneer DDJ-RZ", "Pioneer DDJ-RZX", "Pioneer DDJ-SB", "Pioneer DDJ-SB2", "Pioneer DDJ-SB3", "Pioneer DDJ-SR", "Pioneer DDJ-SR2", "Pioneer DDJ-SX", "Pioneer DDJ-WeGO", "Pioneer DDJ-WeGO2", "Pioneer DDJ-WeGO3", "Pioneer DDJ-WeGO4", "Pioneer OPUS-QUAD", "Pioneer XDJ-AERO", "Pioneer XDJ-R1", "Pioneer XDJ-RR", "Pioneer XDJ-RX", "Pioneer XDJ-RX2", "Pioneer XDJ-RX3", "Pioneer XDJ-XZ", "Reloop Beatmix", "Reloop Beatmix 2/4 MK2", "Reloop Beatpad", "Reloop Digital Jockey 2 Controller", "Reloop Digital Jockey 2 Interface Edition", "Reloop Digital Jockey 2 Master Edition", "Reloop Jockey 3 ME", "Reloop Mixage", "Reloop Terminal Mix 2/4", "Roland DJ-202", "Roland DJ-505", "Roland DJ-808", "Soundless joyMIDI", "Stanton DJC.4", "Stanton SCS.1m", "Vestax Spin", "Vestax Typhoon", "Vestax Typhoon Enhanced", "Vestax VCI-100", "Vestax VCI-100 (MixVibes 3DEX Editon)", "Vestax VCI-100MKII", "Vestax VCI-400"];
  const FORM = "https://docs.google.com/forms/d/e/1FAIpQLSdSf8hY9KvOsCrz8WXr0-ByGoLaHjSbbuH3lAZJqi7D9_Bvsg/formResponse";

  const list = document.getElementById("controllers");
  if (list) for (const n of CONTROLLERS) { const o = document.createElement("option"); o.value = n; list.append(o); }

  // El botón flotante se esconde mientras la sección de ayuda está a la vista.
  const fab = document.querySelector(".help-fab"), help = document.getElementById("ayuda");
  if (fab && help && "IntersectionObserver" in window)
    new IntersectionObserver(([e]) => fab.classList.toggle("away", e.isIntersecting), { threshold: 0.05 }).observe(help);

  const form = document.getElementById("help-form");
  if (!form) return;
  const msg = form.querySelector(".msg");
  const show = (text, kind) => { msg.textContent = text; msg.className = "msg " + (kind || ""); msg.hidden = !text; };

  // Tres tipos de mensaje: ayuda general, solicitar un controlador nuevo, pedir una revisión.
  const TYPES = {
    ayuda: { title: "Contanos qué pasa", ctrl: "Tu controlador", ctrlPh: "Ej.: DDJ-FLX4, Inpulse 500, ninguno",
             label: "¿Qué problema tenés?", ph: "Qué pasa, cuándo pasa y qué esperabas que pase.", send: "Enviar",
             head: "AYUDA DESDE LA WEB", ok: "¡Recibido! Te escribimos en menos de 24 horas." },
    solicitud: { title: "Solicitá tu controlador", ctrl: "¿Qué controlador es? (marca y modelo)", ctrlPh: "Ej.: Numark Mixstream Pro",
             label: "¿Algo que debamos saber?", ph: "Opcional: qué funciones usás más, si tiene pantallas o jog con display, etc.", send: "Solicitar mi controlador",
             head: "SOLICITUD DE CONTROLADOR", ok: "¡Recibido! Te avisamos apenas tu controlador esté agregado." },
    revision: { title: "Reportá un problema", ctrl: "Tu controlador", ctrlPh: "Ej.: DDJ-400",
             label: "¿Qué funciona mal?", ph: "Qué botón, perilla o luz no responde, o qué hace algo distinto a lo que debería.", send: "Reportar el problema",
             head: "PROBLEMA CON CONTROLADOR", ok: "¡Recibido! Revisamos tu controlador y te escribimos en menos de 24 horas." },
  };
  let type = "ayuda";
  const $ = (id) => document.getElementById(id);
  function setType(t, focus) {
    if (!TYPES[t]) return;
    type = t;
    const c = TYPES[t];
    form.querySelectorAll(".help-type button").forEach((b) => b.classList.toggle("on", b.dataset.type === t));
    form.elements.type.value = t;
    $("help-title").textContent = c.title;
    $("help-ctrl-label").textContent = c.ctrl;
    form.elements.controller.placeholder = c.ctrlPh;
    form.elements.controller.required = t !== "ayuda";
    $("help-problem-label").textContent = c.label;
    form.elements.problem.placeholder = c.ph;
    $("help-submit").textContent = c.send;
    show("");
    if (focus) {
      form.scrollIntoView({ behavior: "smooth", block: "center" });
      setTimeout(() => form.elements.controller.focus({ preventScroll: true }), 500);
    }
  }
  form.querySelectorAll(".help-type button").forEach((b) => b.addEventListener("click", () => setType(b.dataset.type)));
  document.querySelectorAll("a.ask[data-type], a.ctrl-card[data-type]").forEach((a) => a.addEventListener("click", (e) => { e.preventDefault(); setType(a.dataset.type, true); }));
  // Enlaces directos: index.html#solicitar, #revision (también desde Mapeos)
  const fromHash = () => {
    const h = location.hash.replace("#", "");
    if (h === "solicitar") setType("solicitud", true);
    else if (h === "revision" || h === "reportar") setType("revision", true);
  };
  window.addEventListener("hashchange", fromHash);
  fromHash();

  form.addEventListener("submit", async (e) => {
    e.preventDefault();
    const f = new FormData(form);
    if (f.get("website")) return;                                    // trampa para bots
    const controller = String(f.get("controller") || "").trim();
    const problem = String(f.get("problem") || "").trim();
    const contact = String(f.get("contact") || "").trim();
    if (type !== "ayuda" && !controller) { show("Decinos qué controlador es (marca y modelo).", "error"); return; }
    if ((type !== "solicitud" && !problem) || !contact) {
      show(type === "solicitud" ? "Decinos cómo te avisamos (WhatsApp, Instagram o mail)." : "Contanos qué pasa y cómo te respondemos (WhatsApp, Instagram o mail).", "error");
      return;
    }

    const message = [
      TYPES[type].head,
      "Controlador: " + (controller || "(sin controlador)"),
      "Sistema: " + (f.get("system") || "-"),
      "Contacto: " + contact,
      "",
      problem || "(sin detalle)",
    ].join("\n");
    const info = "Web · " + navigator.userAgent;
    const body = new URLSearchParams({ "entry.1136859481": message, "entry.1953322693": info });

    const btn = form.querySelector("button[type=submit]");
    btn.disabled = true;
    show("Enviando…");
    try {
      // Google Forms no responde a otros sitios (CORS): el envío sale igual y no se puede leer la respuesta.
      await fetch(FORM, { method: "POST", mode: "no-cors", body });
      form.reset();
      form.elements.type.value = type;
      show(TYPES[type].ok + " Si es urgente, escribinos ahora por WhatsApp.", "ok");
    } catch {
      show("No se pudo enviar. Escribinos por WhatsApp o Instagram y lo vemos al toque.", "error");
    } finally {
      btn.disabled = false;
    }
  });
})();
