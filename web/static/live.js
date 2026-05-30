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

  // ---- ASR 实时字幕（SSE）-------------------------------------------------
  // 直播字幕策略 = E 方案：实时纠正 + 限尾 + 不主动消失。
  //
  //   partial：每 ~100ms 整段刷新（跟着说话节奏），但**限尾 MAX_CHARS 字**，
  //            超出加 "…" 前缀，长 segment 也不会铺满屏。
  //   final  ：punct 版优先；显示后**不主动消失**，等下一个 partial/final 覆盖。
  //            纯沉默兜底：30s 后才清空（避免一句话挂半小时）。
  //   punct  ：同 segment 后到的 punct final 替换 raw final 的待显示 timer。
  //
  // 为什么这是直播监控最优解：
  //   - 不闪：partial 限尾后旧字滚走，新字进来，没"一大段同时变化"
  //   - 不空白：final 不主动消失，句间隔的 5-15s 都能看到刚说的话
  //   - 跟说同步：保留 partial 实时刷新
  //   - 4s delay：匹配 HLS 视频缓冲（用户已实测）
  //
  // 控件：
  //   显示字幕 toggle    —— 关闭即刻清空，不再接收
  //   字幕延迟 + 应用按钮 —— 改完点【应用】才生效（避免边输入边跳）
  const captionEl = document.getElementById('live-caption');
  if (captionEl && typeof EventSource !== 'undefined') {
    const LS_ENABLED = 'liveCaptionEnabled_v2';   // v2: 默认关闭（旧键作废）
    const LS_DELAY   = 'liveCaptionDelay';
    let captionsEnabled = false;                  // 默认不显示字幕，可在 UI 手动开
    let captionDelaySec = 4.0;
    try {
      const e = localStorage.getItem(LS_ENABLED);
      if (e !== null) captionsEnabled = (e === '1');
      const v = localStorage.getItem(LS_DELAY);
      if (v !== null) {
        const n = parseFloat(v);
        if (Number.isFinite(n) && n >= 0) captionDelaySec = n;
      }
    } catch {}

    // 显示参数（不暴露给 UI，需要调整改这里）
    const MAX_CHARS       = 50;     // 单次显示最多字数（超出 "…" 前缀截尾）
    const SILENCE_HOLD_MS = 15000;  // 末次更新后多久没新内容自动清空（15s）

    let silenceTimer          = null;
    let pendingPartialTimer   = null;
    let lastFinalSeg          = -1;
    let lastFinalDisplayTimer = null;

    function clearCaption() {
      if (silenceTimer)          { clearTimeout(silenceTimer);          silenceTimer = null; }
      if (pendingPartialTimer)   { clearTimeout(pendingPartialTimer);   pendingPartialTimer = null; }
      if (lastFinalDisplayTimer) { clearTimeout(lastFinalDisplayTimer); lastFinalDisplayTimer = null; }
      captionEl.textContent = '';
      captionEl.classList.remove('final');
    }

    function trimTail(text) {
      if (!text) return '';
      return (text.length > MAX_CHARS) ? '…' + text.slice(-MAX_CHARS) : text;
    }

    function applyCaption(d) {
      if (!captionsEnabled) return;
      // 每次更新都重置沉默兜底定时器：连续有新内容就一直挂着；
      // 15s 内没任何新内容（真静默 / 无人说话）才清空。
      if (silenceTimer) clearTimeout(silenceTimer);
      captionEl.textContent = trimTail(d.text);
      captionEl.classList.toggle('final', !!d.is_final);
      silenceTimer = setTimeout(() => {
        captionEl.textContent = '';
        captionEl.classList.remove('final');
      }, SILENCE_HOLD_MS);
    }

    // ---- UI controls ----
    const toggleEl   = document.getElementById('live-cap-toggle');
    const delayInput = document.getElementById('live-cap-delay');
    const delayApply = document.getElementById('live-cap-delay-apply');
    if (toggleEl) {
      toggleEl.checked = captionsEnabled;
      toggleEl.addEventListener('change', () => {
        captionsEnabled = toggleEl.checked;
        try { localStorage.setItem(LS_ENABLED, captionsEnabled ? '1' : '0'); } catch {}
        if (!captionsEnabled) clearCaption();
      });
    }
    if (delayInput) {
      delayInput.value = captionDelaySec.toFixed(1);
    }
    function applyDelay() {
      if (!delayInput) return;
      const n = parseFloat(delayInput.value);
      captionDelaySec = (Number.isFinite(n) && n >= 0) ? n : 0;
      delayInput.value = captionDelaySec.toFixed(1);
      try { localStorage.setItem(LS_DELAY, captionDelaySec.toFixed(2)); } catch {}
    }
    if (delayApply) delayApply.addEventListener('click', applyDelay);
    if (delayInput) delayInput.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') { e.preventDefault(); applyDelay(); }
    });

    // ---- SSE ----
    const es = new EventSource('/api/transcript/stream');
    es.onmessage = (ev) => {
      if (!captionsEnabled) return;
      let d;
      try { d = JSON.parse(ev.data); } catch { return; }
      const text = d.text || '';
      if (!text) return;
      const delayMs = Math.max(0, captionDelaySec * 1000);

      if (d.is_final) {
        // 同 segment 的 punct final 后到 → 覆盖 raw final 的 pending 显示
        const seg = d.segment;
        if (d.punct && seg === lastFinalSeg && lastFinalDisplayTimer) {
          clearTimeout(lastFinalDisplayTimer);
        }
        lastFinalSeg = seg;
        if (delayMs === 0) {
          applyCaption(d);
        } else {
          lastFinalDisplayTimer = setTimeout(() => applyCaption(d), delayMs);
        }
        return;
      }

      // partial：每条都进队列；新 partial 来时取消旧 pending（只保留最新一个待显示）
      // 显示出来后字幕就是当时最新的全文。
      if (pendingPartialTimer) clearTimeout(pendingPartialTimer);
      if (delayMs === 0) {
        applyCaption(d);
      } else {
        pendingPartialTimer = setTimeout(() => applyCaption(d), delayMs);
      }
    };
    es.onerror = () => {
      captionEl.classList.add('disconnected');
      setTimeout(() => captionEl.classList.remove('disconnected'), 1000);
    };
  }
})();
