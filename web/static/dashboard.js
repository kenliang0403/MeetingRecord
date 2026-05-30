// dashboard.js — 状态页：设备 + 4 路状态徽章 + 启停按钮 + 接收主流 VU
//
// 不再嵌入 HLS 视频预览（迁到 /live 页）。这里只关心：
//   1. 状态轮询 (/api/status) → 4 个 badge + 设备表
//   2. 音频电平 (/api/levels) → 横向 VU
//   3. 启停按钮 → POST /api/control/<cmd>
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

  // ---- 设备状态表 + 流徽章 ----
  function el(id) { return document.getElementById(id); }

  function setBadge(id, active, text) {
    const e = el(id);
    if (!e) return;
    e.textContent = text;
    e.classList.toggle('badge-active', !!active);
    e.classList.toggle('badge-idle',   !active);
  }

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

      // 接收-主流：通话中且 main_file 非空
      const rxMainActive = !!d.in_call && !!d.main_file;
      setBadge("rx-main-badge", rxMainActive, rxMainActive ? "● 接收中" : "空闲");
      el("rx-main-file").textContent = d.main_file || "—";

      // 接收-辅流：远端有人在演示 (h239_received) 或 aux 正在录
      const rxAuxActive = !!(d.h239_received || d.aux_recording);
      setBadge("rx-aux-badge", rxAuxActive, rxAuxActive ? "● 接收中" : "空闲");
      el("rx-aux-file").textContent = d.aux_file || "—";

      // 发送-主流
      setBadge("tx-main-badge", !!d.main_sending,
               d.main_sending ? "● 发送中" : "空闲");
      const auxTx = !!d.has_presentation;
      setBadge("tx-aux-badge", auxTx, auxTx ? "● 发送中" : "空闲");
    } catch {}
  }
  pollStatus();
  setInterval(pollStatus, 2000);

  // ---- 服务器资源（CPU / 内存 / 磁盘）----
  function fmtBytes(n) {
    if (!n && n !== 0) return "—";
    const u = ["B", "KB", "MB", "GB", "TB"];
    let i = 0;
    while (n >= 1024 && i < u.length - 1) { n /= 1024; i++; }
    return n.toFixed(i >= 3 ? 1 : 0) + " " + u[i];
  }
  function fmtUptime(s) {
    if (!s) return "";
    const d = Math.floor(s / 86400), h = Math.floor((s % 86400) / 3600),
          m = Math.floor((s % 3600) / 60);
    return "已运行 " + (d ? d + "天 " : "") + h + "时 " + m + "分";
  }
  function setBar(barId, numId, pct, text) {
    const bar = el(barId), num = el(numId);
    if (bar) {
      const p = (pct == null) ? 0 : Math.max(0, Math.min(100, pct));
      bar.style.width = p + "%";
      bar.classList.toggle("res-warn", p >= 75 && p < 90);
      bar.classList.toggle("res-crit", p >= 90);
    }
    if (num) num.textContent = text;
  }
  async function pollSystem() {
    try {
      const r = await fetch("/api/system", { cache: "no-store" });
      const j = await r.json();
      if (!j.ok) return;
      const d = j.data;
      // CPU
      const cpuPct = d.cpu.pct;
      setBar("sys-cpu-bar", "sys-cpu-num", cpuPct,
             cpuPct == null ? "采样中…" : cpuPct + "%");
      // 内存
      setBar("sys-mem-bar", "sys-mem-num", d.mem.pct,
             `${d.mem.pct}% (${fmtBytes(d.mem.used)}/${fmtBytes(d.mem.total)})`);
      // 磁盘
      setBar("sys-disk-bar", "sys-disk-num", d.disk.pct,
             `${d.disk.pct}% (剩 ${fmtBytes(d.disk.free)})`);
      // 负载 + uptime
      const ld = el("sys-load");
      if (ld) ld.textContent =
        `负载 ${d.cpu.load1} / ${d.cpu.load5} / ${d.cpu.load15}（${d.cpu.ncpu} 核）`;
      const up = el("sys-uptime");
      if (up) up.textContent = fmtUptime(d.uptime_s);
    } catch {}
  }
  pollSystem();
  setInterval(pollSystem, 3000);

  // ---- SRS 直播流状态 ----
  async function pollSrs() {
    try {
      const r = await fetch("/api/srs", { cache: "no-store" });
      const j = await r.json();
      const streams = (j.data && j.data.streams) || [];
      // 取 recorder-main（没有就取第一条）
      const s = streams.find(x => /main/.test(x.name || "")) || streams[0];
      const badge = el("srs-badge");
      if (!s || !s.publish) {
        if (badge) { badge.textContent = "无推流"; badge.className = "stream-badge badge-idle"; }
        el("srs-publish").textContent = s ? "无（流存在但未推流）" : "无流";
        el("srs-video").textContent = "—";
        el("srs-audio").textContent = "—";
        el("srs-kbps").textContent = "—";
        el("srs-clients").textContent = s ? String(s.clients || 0) : "0";
        return;
      }
      if (badge) { badge.textContent = "● 推流中"; badge.className = "stream-badge badge-active"; }
      el("srs-publish").textContent = "● 活跃 (" + s.name + ")";
      const v = s.video || {};
      el("srs-video").textContent = (v.codec || "?") +
        (v.w && v.h ? ` ${v.w}×${v.h}` : "") + (v.profile ? ` (${v.profile})` : "");
      const a = s.audio || {};
      el("srs-audio").textContent = (a.codec || "?") +
        (a.sample_rate ? ` ${a.sample_rate}Hz` : "");
      el("srs-kbps").textContent = `↓${s.recv_kbps || 0} / ↑${s.send_kbps || 0} kbps`;
      el("srs-clients").textContent = String(s.clients || 0);
    } catch {}
  }
  pollSrs();
  setInterval(pollSrs, 3000);

  // ---- 6 个服务状态 ----
  function svcClass(state) {
    if (state === "active") return "svc-ok";
    if (state === "activating" || state === "reloading") return "svc-warn";
    return "svc-bad";   // inactive / failed / unknown / error
  }
  function svcText(state) {
    return ({ active: "运行中", inactive: "已停止", failed: "失败",
              activating: "启动中", reloading: "重载中" })[state] || state;
  }
  async function pollServices() {
    try {
      const r = await fetch("/api/services", { cache: "no-store" });
      const j = await r.json();
      const grid = el("svc-grid");
      if (!j.ok || !grid) return;
      grid.innerHTML = "";
      (j.data.services || []).forEach(s => {
        const pill = document.createElement("div");
        pill.className = "svc-pill " + svcClass(s.state);
        pill.innerHTML =
          `<span class="svc-dot"></span>` +
          `<div class="svc-info">` +
            `<span class="svc-name">${s.label}</span>` +
            `<span class="svc-unit">${s.unit}</span>` +
          `</div>` +
          `<span class="svc-state">${svcText(s.state)}</span>`;
        grid.appendChild(pill);
      });
    } catch {}
  }
  pollServices();
  setInterval(pollServices, 5000);

  // ---- 复制按钮 ----
  document.querySelectorAll(".copy-btn").forEach(btn => {
    btn.addEventListener("click", async () => {
      const target = el(btn.dataset.copy);
      if (!target) return;
      const text = target.textContent.trim();
      try {
        await navigator.clipboard.writeText(text);
        const orig = btn.textContent;
        btn.textContent = "✓ 已复制";
        setTimeout(() => { btn.textContent = orig; }, 1500);
      } catch {
        // 降级：选中文本
        const range = document.createRange();
        range.selectNode(target);
        window.getSelection().removeAllRanges();
        window.getSelection().addRange(range);
      }
    });
  });

  // ---- 启停按钮 ----
  const msgEl = document.getElementById('control-msg');
  const defaultMsg = msgEl ? msgEl.innerHTML : '';
  function flashMsg(text, isError) {
    if (!msgEl) return;
    msgEl.textContent = text;
    msgEl.classList.toggle('control-msg-err', !!isError);
    setTimeout(() => {
      msgEl.innerHTML = defaultMsg;
      msgEl.classList.remove('control-msg-err');
    }, 4000);
  }

  document.querySelectorAll('[data-control]').forEach(btn => {
    btn.addEventListener('click', async () => {
      const cmd = btn.dataset.control;
      btn.disabled = true;
      try {
        const r = await fetch(`/api/control/${cmd}`, {
          method: 'POST',
          headers: { 'Content-Type': 'application/json', ...window.csrfHeaders() },
          body: '{}',
        });
        const j = await r.json();
        if (j.ok) {
          flashMsg(`✓ ${cmd}: ${j.message || 'ok'}`);
          pollStatus();    // 立即拉新状态刷徽章
        } else {
          flashMsg(`✗ ${cmd}: ${j.error || 'failed'}`, true);
        }
      } catch (e) {
        flashMsg(`✗ ${cmd}: ${e}`, true);
      } finally {
        setTimeout(() => { btn.disabled = false; }, 600);
      }
    });
  });
})();
