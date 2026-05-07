// live.js — 直播页：同回放页相同的 stage + 4 布局，但拉的是 HLS 直播流
//
// 行为：
//   - video 元素带 autoplay+muted（浏览器策略），用户首次在页面上点击/触摸
//     一次后自动取消 primary 的静音，相当于"打开声音"。
//   - 流是否真的 attach hls.js 由后端 /api/status 状态决定：
//       main 活跃   ⇔ in_call && main_file 非空
//       aux 活跃    ⇔ in_call && (has_presentation || h239_received || aux_recording)
//     不活跃时不 attach（避免 hls.js 在没有 manifest 的 URL 上反复重试）；
//     一旦活跃，下一轮 poll 立即 attach + play。
//   - 不活跃时叠加 overlay 文字（"等待主流到达 / 当前无辅流演示 / 未在会议中"），
//     比 hls.js 的黑屏重试体验好。
(function () {
  const stage = document.getElementById('live-stage');
  const v1 = document.getElementById('live-primary');
  const v2 = document.getElementById('live-secondary');
  const overlayP = document.getElementById('live-overlay-primary');
  const overlayS = document.getElementById('live-overlay-secondary');
  const swapBtn  = document.getElementById('live-swap-btn');

  let primarySource   = 'main';
  let secondarySource = 'aux';
  let layout          = 'main-only';
  let lastStatus      = {};

  // 当前实际 attach 在每个 video 上的源（'main' / 'aux' / null）
  const attached = { primary: null, secondary: null };
  const hlsInstances = { main: null, aux: null };

  function urlFor(srcKey) {
    return `${SRS_BASE}/live/${srcKey === 'main' ? MAIN_KEY : AUX_KEY}.m3u8`;
  }

  function destroyHls(srcKey) {
    if (hlsInstances[srcKey]) {
      try { hlsInstances[srcKey].destroy(); } catch (_) {}
      hlsInstances[srcKey] = null;
    }
  }

  // 把 srcKey 流加载到 video 并 play；srcKey=null 表示彻底解绑
  function loadInto(srcKey, video) {
    if (!srcKey) {
      try { video.pause(); video.removeAttribute('src'); video.load(); } catch (_) {}
      return;
    }
    destroyHls(srcKey);
    const url = urlFor(srcKey);
    if (Hls.isSupported()) {
      const hls = new Hls({ liveSyncDurationCount: 2, manifestLoadingMaxRetry: 1 });
      hls.loadSource(url);
      hls.attachMedia(video);
      hls.on(Hls.Events.MANIFEST_PARSED, () => {
        video.play().catch(() => {});
      });
      hlsInstances[srcKey] = hls;
    } else if (video.canPlayType("application/vnd.apple.mpegurl")) {
      video.src = url;
      video.play().catch(() => {});
    }
  }

  // 后端状态 → 单源是否"活跃"（值得 attach）
  function sourceLive(srcKey, d) {
    if (!d || !d.in_call) return false;
    if (srcKey === 'main') return !!d.main_file;
    if (srcKey === 'aux')  return !!(d.has_presentation || d.h239_received || d.aux_recording);
    return false;
  }

  // 当前布局是否需要 secondary 槽
  function layoutNeedsSecondary() {
    return layout === 'pip' || layout === 'split';
  }

  // 综合 layout + 状态，决定每个 video 应该承载的源（null = 不该 attach）
  function targetFor(role) {
    if (role === 'primary') {
      if (!primarySource) return null;
      return sourceLive(primarySource, lastStatus) ? primarySource : null;
    }
    // secondary
    if (!layoutNeedsSecondary()) return null;
    if (!secondarySource) return null;
    return sourceLive(secondarySource, lastStatus) ? secondarySource : null;
  }

  // 单一 reconciler：让实际挂载状态向"目标"收敛
  function syncStreams() {
    const tgtP = targetFor('primary');
    const tgtS = targetFor('secondary');
    if (attached.primary !== tgtP) {
      loadInto(tgtP, v1);
      attached.primary = tgtP;
    }
    if (attached.secondary !== tgtS) {
      loadInto(tgtS, v2);
      attached.secondary = tgtS;
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
    syncStreams();
    refreshOverlays();
  }

  function swapPrimary() {
    if (!secondarySource) return;
    [primarySource, secondarySource] = [secondarySource, primarySource];
    syncStreams();
    refreshOverlays();
  }

  // ---- 状态轮询 ----------------------------------------------------------
  async function pollStatus() {
    try {
      const r = await fetch('/api/status', { cache: 'no-store' });
      const j = await r.json();
      if (j.ok) {
        lastStatus = j.data || {};
        syncStreams();      // 状态变 → 可能要 attach / detach
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
    return sourceLive('aux', d) ? '' : '当前无辅流演示';
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
    // secondary overlay 仅在 layout 用到 secondary 时显示
    setOverlay(overlayS, layoutNeedsSecondary() ? overlayTextFor(secondarySource) : '');
  }

  // ---- 用户首次交互后取消 primary 静音 ----------------------------------
  // 浏览器自动播放策略禁止 unmuted autoplay；先 muted autoplay 让画面出来，
  // 用户随便点一下页面就把声音打开。secondary 永远 muted（小窗 / 副屏不发声）。
  function unmutePrimary() {
    try { v1.muted = false; } catch (_) {}
  }
  ['pointerdown', 'keydown', 'touchstart'].forEach(ev => {
    document.addEventListener(ev, unmutePrimary, { once: true, passive: true });
  });

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
