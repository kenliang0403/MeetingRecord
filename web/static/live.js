// live.js — 直播页：同回放页相同的 stage + 4 布局，但拉的是 HLS 直播流
//
// 在 status 给出"未在通话 / 无辅流"时，覆盖一层占位文字提示，
// 而不是任由 hls.js 在黑屏上无限重试。
(function () {
  const stage = document.getElementById('live-stage');
  const v1 = document.getElementById('live-primary');
  const v2 = document.getElementById('live-secondary');
  const overlayP = document.getElementById('live-overlay-primary');
  const overlayS = document.getElementById('live-overlay-secondary');
  const msgEl    = document.getElementById('live-msg');
  const swapBtn  = document.getElementById('live-swap-btn');

  let primarySource   = 'main';
  let secondarySource = 'aux';
  let layout          = 'main-only';

  const hlsInstances = { main: null, aux: null };
  const sourceVisible = { main: false, aux: false };  // 该源是否有人订阅

  function urlFor(srcKey) {
    return `${SRS_BASE}/live/${srcKey === 'main' ? MAIN_KEY : AUX_KEY}.m3u8`;
  }

  function detachStream(srcKey) {
    if (hlsInstances[srcKey]) {
      try { hlsInstances[srcKey].destroy(); } catch (_) {}
      hlsInstances[srcKey] = null;
    }
  }

  // 把指定源 (main/aux) 加载到指定 video。null 表示解绑。
  function attachStream(srcKey, video) {
    if (!srcKey) {
      try { video.pause(); video.removeAttribute('src'); video.load(); } catch (_) {}
      return;
    }
    detachStream(srcKey);
    const url = urlFor(srcKey);
    if (Hls.isSupported()) {
      const hls = new Hls({ liveSyncDurationCount: 2, manifestLoadingMaxRetry: 1 });
      hls.loadSource(url);
      hls.attachMedia(video);
      hlsInstances[srcKey] = hls;
    } else if (video.canPlayType("application/vnd.apple.mpegurl")) {
      video.src = url;
    }
  }

  function applyLayout() {
    stage.classList.remove('layout-main-only','layout-aux-only','layout-pip','layout-split');
    stage.classList.add('layout-' + layout);

    v1.controls = true;
    if (layout === 'split') v2.controls = true;
    else if (layout === 'pip') { v2.controls = false; v2.muted = true; }
    else v2.controls = false;

    document.querySelectorAll('#live-layout-segments button').forEach(b => {
      b.classList.toggle('active', b.dataset.layout === layout);
    });

    if (swapBtn) swapBtn.disabled = !(layout === 'pip' || layout === 'split');

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
    refreshOverlays();
  }

  function swapPrimary() {
    if (!secondarySource) return;
    [primarySource, secondarySource] = [secondarySource, primarySource];
    attachStream(primarySource,   v1);
    attachStream(secondarySource, v2);
    refreshOverlays();
  }

  // ---- 状态轮询，决定占位提示文案 ----------------------------------
  // 主层 (overlayP) 关联当前 primary 源；辅层 (overlayS) 关联当前 secondary 源
  // 文案规则：
  //   未通话 (in_call=false)            → "当前未在会议中"
  //   通话中、main_file 空              → "等待主流到达..."
  //   通话中、has_presentation=false    → "当前无辅流演示"
  //   通话中、aux 流活跃                → 不显示 overlay (隐藏)
  let lastStatus = {};
  async function pollStatus() {
    try {
      const r = await fetch('/api/status', { cache: 'no-store' });
      const j = await r.json();
      if (j.ok) {
        lastStatus = j.data || {};
        refreshOverlays();
      }
    } catch {}
  }
  function overlayTextFor(srcKey) {
    if (!srcKey) return '';
    const d = lastStatus;
    if (!d.in_call) return '当前未在会议中';
    if (srcKey === 'main') {
      return d.main_file ? '' : '等待主流到达…';
    }
    // aux
    const auxActive = !!(d.has_presentation || d.h239_received || d.aux_recording);
    return auxActive ? '' : '当前无辅流演示';
  }
  function setOverlay(el, text) {
    if (!el) return;
    if (text) {
      el.textContent = text;
      el.classList.add('show');
    } else {
      el.textContent = '';
      el.classList.remove('show');
    }
  }
  function refreshOverlays() {
    setOverlay(overlayP, overlayTextFor(primarySource));
    setOverlay(overlayS, overlayTextFor(secondarySource));
  }

  // ---- 事件 -----------------------------------------------------------
  document.querySelectorAll('#live-layout-segments button').forEach(b => {
    b.addEventListener('click', () => {
      layout = b.dataset.layout;
      applyLayout();
    });
  });
  if (swapBtn) swapBtn.addEventListener('click', swapPrimary);

  applyLayout();
  pollStatus();
  setInterval(pollStatus, 2000);
})();
