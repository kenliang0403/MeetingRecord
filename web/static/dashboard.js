// dashboard: 状态表 + VU 表 + 主辅流 HLS player
(function () {
  // ---- VU bars ----
  const rmsBar  = window.makeVuBar("vu-rms",  "vu-rms-num");
  const peakBar = window.makeVuBar("vu-peak", "vu-peak-num");

  async function pollLevels() {
    try {
      const r = await fetch("/api/levels", { cache: "no-store" });
      const j = await r.json();
      if (j.ok && j.data) {
        rmsBar.draw(j.data.rms_dbfs);
        peakBar.draw(j.data.peak_dbfs);
      }
    } catch {}
  }
  setInterval(pollLevels, 100);

  // ---- Status table ----
  function el(id) { return document.getElementById(id); }
  function fmtBool(b, ok = "✅", bad = "—") { return b ? ok : bad; }

  async function pollStatus() {
    try {
      const r = await fetch("/api/status", { cache: "no-store" });
      const j = await r.json();
      if (!j.ok) return;
      const d = j.data || {};
      el("s-reg").textContent     = d.gk_registered ? "已注册" : "未注册";
      el("s-reg").className       = d.gk_registered ? "ok" : "bad";
      el("s-alias").textContent   = d.alias || "—";
      el("s-gk").textContent      = (d.gk_host || "—") + ":" + (d.gk_port || "—");
      el("s-incall").textContent  = d.in_call ? "进行中" : "空闲";
      el("s-incall").className    = d.in_call ? "ok" : "muted";
      el("s-meeting").textContent = d.meeting_id
        ? `${d.meeting_id} ${d.meeting_name ? '(' + d.meeting_name + ')' : ''}`
        : "—";
      el("s-mainfile").textContent = d.main_file || "—";
      el("s-auxfile").textContent  = d.aux_file  || "—";
    } catch {}
  }
  pollStatus();
  setInterval(pollStatus, 2000);

  // ---- HLS players ----
  function setupPlayer(videoId, msgId, streamKey) {
    const video = document.getElementById(videoId);
    const msg   = document.getElementById(msgId);
    const url   = `${SRS_BASE}/live/${streamKey}.m3u8`;
    msg.textContent = `源: ${url}`;
    if (Hls.isSupported()) {
      const hls = new Hls({ liveSyncDurationCount: 2 });
      hls.loadSource(url);
      hls.attachMedia(video);
      hls.on(Hls.Events.ERROR, function (_, data) {
        if (data.fatal) {
          msg.textContent = `源: ${url} — 等待推流`;
        }
      });
    } else if (video.canPlayType("application/vnd.apple.mpegurl")) {
      video.src = url;
    } else {
      msg.textContent = "浏览器不支持 HLS";
    }
  }
  setupPlayer("v-main", "v-main-msg", MAIN_KEY);
  setupPlayer("v-aux",  "v-aux-msg",  AUX_KEY);
})();
