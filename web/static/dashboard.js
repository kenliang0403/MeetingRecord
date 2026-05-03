// dashboard.js — 状态页：设备 + 4 路视频流 + 启停按钮
//
// 4 卡片：
//   接收-主流：HLS player 拉 SRS recorder-main + VU 表（H.323 唯一音频）
//   接收-辅流：HLS player 拉 SRS recorder-aux （远端 H.239 演示）
//   发送-主流：状态 LED + 启停按钮（POST /api/control/start_main_video|stop_main_video）
//   发送-辅流：状态 LED + 启停按钮（POST /api/control/start_presentation|stop_presentation）
(function () {
  // ---- VU bars (横向) ----
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

  // ---- 设备状态表 + 流状态徽章 ----
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

      // 接收-主流：通话中且有 main_file 视为活跃
      const rxMainActive = !!d.in_call && !!d.main_file;
      setBadge("rx-main-badge", rxMainActive, rxMainActive ? "● 接收中" : "空闲");
      el("rx-main-file").textContent = d.main_file || "—";

      // 接收-辅流：has_presentation 或 aux_recording
      const rxAuxActive = !!(d.has_presentation || d.aux_recording);
      setBadge("rx-aux-badge", rxAuxActive, rxAuxActive ? "● 接收中" : "空闲");
      el("rx-aux-file").textContent = d.aux_file || "—";

      // 发送-主流
      setBadge("tx-main-badge", !!d.main_sending,
               d.main_sending ? "● 发送中" : "空闲");

      // 发送-辅流（H.239）— 暂无显式 sending 字段，从 has_presentation 反推不可靠
      // 我们让按钮反馈控制；实际状态从 9001 命令的 ok=false 反映
      // 这里先保留 badge 仅在用户点过"开始演示"后由 client-side 更新
    } catch {}
  }
  pollStatus();
  setInterval(pollStatus, 2000);

  // ---- HLS 播放接收两路 ----
  function attachHls(srcKey, videoId, fileEl) {
    const v = document.getElementById(videoId);
    const url = `${SRS_BASE}/live/${srcKey === 'main' ? MAIN_KEY : AUX_KEY}.m3u8`;
    if (Hls.isSupported()) {
      const hls = new Hls({ liveSyncDurationCount: 2 });
      hls.loadSource(url);
      hls.attachMedia(v);
      hls.on(Hls.Events.ERROR, function (_, data) {
        if (data.fatal) {
          // 流不存在时 hls.js 会持续重试；这里仅静默
        }
      });
    } else if (v.canPlayType("application/vnd.apple.mpegurl")) {
      v.src = url;
    }
  }
  attachHls('main', 'rx-main');
  attachHls('aux',  'rx-aux');

  // ---- 启停按钮 ----
  const msgEl = document.getElementById('control-msg');
  function flashMsg(text, isError) {
    msgEl.textContent = text;
    msgEl.classList.toggle('control-msg-err', !!isError);
    setTimeout(() => { msgEl.textContent = ''; msgEl.classList.remove('control-msg-err'); }, 4000);
  }

  document.querySelectorAll('[data-control]').forEach(btn => {
    btn.addEventListener('click', async () => {
      const cmd = btn.dataset.control;
      btn.disabled = true;
      try {
        const r = await fetch(`/api/control/${cmd}`, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: '{}' });
        const j = await r.json();
        if (j.ok) {
          flashMsg(`✓ ${cmd}: ${j.message || 'ok'}`);
          // 立即刷新状态
          pollStatus();
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
