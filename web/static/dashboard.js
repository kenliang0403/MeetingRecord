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
          headers: { 'Content-Type': 'application/json' },
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
