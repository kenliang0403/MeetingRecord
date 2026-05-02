// dashboard.js — 状态页：状态表 + VU 表 + 直播 stage
//
// 直播 stage 与回放页相同的布局机制（main-only / aux-only / pip / split），
// 但没有片段概念：HLS 流持续推到 #live-primary / #live-secondary。
// 切换布局只改 visibility，不重新加载流。
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

  // ---- 状态表 ----
  function el(id) { return document.getElementById(id); }
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

  // ---- 直播 stage ----
  const stage = document.getElementById('live-stage');
  const v1 = document.getElementById('live-primary');
  const v2 = document.getElementById('live-secondary');
  const swapBtn = document.getElementById('live-swap-btn');
  const msgEl = document.getElementById('live-msg');

  // 给两个 video 各挂一份 HLS 流；layout 切换不重新加载，只改 visibility
  // primarySource/secondarySource 表示哪个 video 在播 main / aux
  let primarySource   = 'main';
  let secondarySource = 'aux';
  let layout          = 'main-only';

  // 维护 hls.js 实例对象，方便 destroy/recreate
  const hlsInstances = { main: null, aux: null };

  function urlFor(srcKey) {
    const key = srcKey === 'main' ? MAIN_KEY : AUX_KEY;
    return `${SRS_BASE}/live/${key}.m3u8`;
  }

  // 把指定源 (main/aux) 加载到指定 video 元素
  function attachStream(srcKey, video) {
    if (!srcKey) {
      // 解绑：清掉 src，destroy 已有 hls
      try { video.pause(); video.removeAttribute('src'); video.load(); } catch (_) {}
      return;
    }
    const url = urlFor(srcKey);
    if (Hls.isSupported()) {
      // destroy 旧实例
      if (hlsInstances[srcKey]) {
        try { hlsInstances[srcKey].destroy(); } catch (_) {}
        hlsInstances[srcKey] = null;
      }
      const hls = new Hls({ liveSyncDurationCount: 2 });
      hls.loadSource(url);
      hls.attachMedia(video);
      hls.on(Hls.Events.ERROR, function (_, data) {
        if (data.fatal) {
          msgEl.textContent = `${srcKey} 流暂未推送（${url}）`;
        }
      });
      hlsInstances[srcKey] = hls;
    } else if (video.canPlayType("application/vnd.apple.mpegurl")) {
      video.src = url;
    } else {
      msgEl.textContent = "浏览器不支持 HLS";
    }
  }

  function applyLayout() {
    stage.classList.remove('layout-main-only','layout-aux-only','layout-pip','layout-split');
    stage.classList.add('layout-' + layout);

    // controls/mute 配置
    v1.controls = true;
    if (layout === 'split') v2.controls = true;
    else if (layout === 'pip') { v2.controls = false; v2.muted = true; }
    else v2.controls = false;

    // 高亮当前布局按钮
    document.querySelectorAll('#live-layout-segments button').forEach(b => {
      b.classList.toggle('active', b.dataset.layout === layout);
    });

    if (swapBtn) swapBtn.disabled = !(layout === 'pip' || layout === 'split');

    // 决定 primary/secondary
    if (layout === 'main-only') {
      primarySource = 'main';
      secondarySource = null;
    } else if (layout === 'aux-only') {
      primarySource = 'aux';
      secondarySource = null;
    } else {
      if (!primarySource)   primarySource   = 'main';
      if (!secondarySource) secondarySource = (primarySource === 'main') ? 'aux' : 'main';
    }
    attachStream(primarySource,   v1);
    attachStream(secondarySource, v2);
    msgEl.textContent = '';
  }

  function swapPrimary() {
    if (!secondarySource) return;
    [primarySource, secondarySource] = [secondarySource, primarySource];
    attachStream(primarySource,   v1);
    attachStream(secondarySource, v2);
  }

  document.querySelectorAll('#live-layout-segments button').forEach(b => {
    b.addEventListener('click', () => {
      layout = b.dataset.layout;
      applyLayout();
    });
  });
  if (swapBtn) swapBtn.addEventListener('click', swapPrimary);

  applyLayout();
})();
