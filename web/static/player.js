// player.js — 回放页：主辅流双源播放器 + meeting timeline 同步
//
// 设计要点：
//   - 一个 #vid-primary + 一个 #vid-secondary，绑到 main / aux 源
//   - 4 种布局：main-only / aux-only / pip / split
//   - 多片段 chained playback：ended → 自动加载下一段 + play
//   - swap 按钮：互换 primary/secondary 的源（保留进度）
//   - 主辅流同步（默认开启，仅当 meeting_t0 + segment offset 可用时）：
//       * 主流的 video timeupdate（~250ms）→ 计算当前 meeting time
//       * 找包含该时刻的 aux 段；切段或 seek 微调
//       * 用户主动操作辅流（点上/下一段）→ 临时停 sync 5 秒，避免被立即拉回
//
// 简化决定：
//   - 同步基准始终是"主流"，辅流被动跟随
//   - sync 的对齐误差容忍 500ms（小于则不动，避免抖动）
//   - 用户一旦关闭 sync 开关就完全独立播放
(function () {
  const MAIN = window.MAIN_SEGMENTS || [];
  const AUX  = window.AUX_SEGMENTS  || [];
  const T0   = window.MEETING_T0_MS;
  const HAS_TIME_DATA = T0 != null && MAIN.every(s => s.meeting_offset_ms != null && s.duration_ms != null);

  const stage = document.getElementById('stage');
  const v1 = document.getElementById('vid-primary');
  const v2 = document.getElementById('vid-secondary');

  // --- 源 ----------------------------------------------------------
  const sources = {
    main: { name: '主流', files: MAIN, idx: 0, video: null,
            indicator: document.getElementById('main-indicator'),
            nameEl:    document.getElementById('main-name'),
            prevBtn:   document.getElementById('main-prev'),
            nextBtn:   document.getElementById('main-next') },
    aux:  { name: '辅流', files: AUX,  idx: 0, video: null,
            indicator: document.getElementById('aux-indicator'),
            nameEl:    document.getElementById('aux-name'),
            prevBtn:   document.getElementById('aux-prev'),
            nextBtn:   document.getElementById('aux-next') },
  };
  let primarySource   = 'main';
  let secondarySource = AUX.length ? 'aux' : null;
  let layout          = 'main-only';

  // 同步控制
  let syncEnabled = HAS_TIME_DATA && AUX.length > 0;
  let auxTouchUntil = 0;   // ms epoch；早于此时间 sync 暂停（用户刚动了辅流）
  let lastSyncSeekMs = 0;

  // --- 基础：加载某段、绑定源到 video ------------------------------
  function loadSegment(srcKey, idx, autoplay) {
    const s = sources[srcKey];
    if (!s.files.length) return;
    if (idx < 0) idx = 0;
    if (idx >= s.files.length) idx = s.files.length - 1;
    s.idx = idx;
    const seg = s.files[idx];
    if (s.video) {
      s.video.src = seg.url;
      if (autoplay) {
        s.video.play().catch(() => {});
      }
    }
    if (s.indicator) s.indicator.textContent = `片段 ${idx + 1} / ${s.files.length}`;
    if (s.nameEl)    s.nameEl.textContent    = `${seg.name} · ${seg.size_mb} MB`;
  }

  function bindSourceToVideo(srcKey, video, autoplay) {
    const s = sources[srcKey];
    if (!s) {
      try { video.removeAttribute('src'); video.load(); } catch (_) {}
      return;
    }
    s.video = video;
    loadSegment(srcKey, s.idx, autoplay);
  }

  function applyLayout() {
    stage.classList.remove('layout-main-only','layout-aux-only','layout-pip','layout-split');
    stage.classList.add('layout-' + layout);
    v1.controls = true;
    if (layout === 'split') {
      v2.controls = true;
    } else if (layout === 'pip') {
      v2.controls = false;
      v2.muted = true;
    } else {
      v2.controls = false;
    }
    const swapBtn = document.getElementById('swap-btn');
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
    bindSourceToVideo(primarySource,   v1, false);
    bindSourceToVideo(secondarySource, v2, false);
  }

  function swapPrimary() {
    if (!secondarySource) return;
    [primarySource, secondarySource] = [secondarySource, primarySource];
    const p1 = v1.currentTime || 0;
    const p2 = v2.currentTime || 0;
    bindSourceToVideo(primarySource,   v1, false);
    bindSourceToVideo(secondarySource, v2, false);
    v1.addEventListener('loadedmetadata', () => { try { v1.currentTime = p2; } catch (_) {} }, { once: true });
    v2.addEventListener('loadedmetadata', () => { try { v2.currentTime = p1; } catch (_) {} }, { once: true });
  }

  // --- 同步：以主流为基准把辅流定位到对应 meeting timeline 时刻 ----
  // 当前主流位置（meeting timeline ms）= mainSeg.meeting_offset_ms + mainVideo.currentTime * 1000
  // 找包含该时刻的 aux 段：
  //   aux.meeting_offset_ms <= t < aux.meeting_offset_ms + aux.duration_ms
  // 否则视为"当前主流时刻没有对应辅流" — 暂停辅流（pause），等下一帧 timeupdate
  function findContainingAux(meetingTimeMs) {
    for (let i = 0; i < AUX.length; i++) {
      const a = AUX[i];
      if (a.meeting_offset_ms == null || a.duration_ms == null) continue;
      const start = a.meeting_offset_ms;
      const end   = a.meeting_offset_ms + a.duration_ms;
      if (meetingTimeMs >= start && meetingTimeMs < end) {
        return { idx: i, offsetInSegMs: meetingTimeMs - start };
      }
    }
    return null;
  }

  function getAuxVideo() {
    // 当前哪个 video 元素正在播 aux？
    if (sources.aux.video === v1) return v1;
    if (sources.aux.video === v2) return v2;
    return null;
  }

  function syncAuxToMain() {
    if (!syncEnabled || !HAS_TIME_DATA) return;
    if (Date.now() < auxTouchUntil) return;     // 用户刚动过辅流，暂停 sync
    if (layout === 'main-only' || layout === 'aux-only') return;  // 只有一路时不需同步

    // 主流当前哪个段
    const mainVideo = sources.main.video;
    if (!mainVideo) return;
    const mainSeg = MAIN[sources.main.idx];
    if (!mainSeg || mainSeg.meeting_offset_ms == null) return;
    const t = mainSeg.meeting_offset_ms + (mainVideo.currentTime * 1000);

    const auxVideo = getAuxVideo();
    if (!auxVideo) return;

    const target = findContainingAux(t);
    if (!target) {
      // 当前时刻没辅流：暂停辅流（避免它独自跑飞）
      if (!auxVideo.paused) {
        try { auxVideo.pause(); } catch (_) {}
      }
      return;
    }

    // 切段？
    if (sources.aux.idx !== target.idx) {
      sources.aux.idx = target.idx;
      const seg = AUX[target.idx];
      auxVideo.src = seg.url;
      const offSec = target.offsetInSegMs / 1000;
      auxVideo.addEventListener('loadedmetadata', () => {
        try { auxVideo.currentTime = offSec; } catch (_) {}
        if (!mainVideo.paused) auxVideo.play().catch(() => {});
      }, { once: true });
      sources.aux.indicator.textContent = `片段 ${target.idx + 1} / ${AUX.length}`;
      sources.aux.nameEl.textContent    = `${seg.name} · ${seg.size_mb} MB`;
      lastSyncSeekMs = Date.now();
      return;
    }

    // 同段，校准位置
    const wantSec = target.offsetInSegMs / 1000;
    const drift   = Math.abs(auxVideo.currentTime - wantSec);
    if (drift > 0.5) {                     // 容忍 500ms
      try { auxVideo.currentTime = wantSec; } catch (_) {}
      lastSyncSeekMs = Date.now();
    }

    // 同步主辅流的 play/pause 状态
    if (mainVideo.paused && !auxVideo.paused) {
      try { auxVideo.pause(); } catch (_) {}
    } else if (!mainVideo.paused && auxVideo.paused) {
      auxVideo.play().catch(() => {});
    }
  }

  // --- 按钮事件 ----------------------------------------------------
  function wireSegmentButtons(srcKey) {
    const s = sources[srcKey];
    if (!s) return;
    if (s.prevBtn) s.prevBtn.addEventListener('click', () => {
      if (srcKey === 'aux') auxTouchUntil = Date.now() + 5000;   // 用户主动操作 aux：暂停 sync 5s
      loadSegment(srcKey, s.idx - 1, true);
    });
    if (s.nextBtn) s.nextBtn.addEventListener('click', () => {
      if (srcKey === 'aux') auxTouchUntil = Date.now() + 5000;
      loadSegment(srcKey, s.idx + 1, true);
    });
  }
  wireSegmentButtons('main');
  wireSegmentButtons('aux');

  function attachAutoChain(video) {
    video.addEventListener('ended', () => {
      const key = (sources.main.video === video) ? 'main' :
                  (sources.aux.video  === video) ? 'aux'  : null;
      if (!key) return;
      const s = sources[key];
      if (s.idx + 1 < s.files.length) {
        loadSegment(key, s.idx + 1, true);
      }
    });
  }
  attachAutoChain(v1);
  attachAutoChain(v2);

  document.getElementById('layout-mode').addEventListener('change', (ev) => {
    layout = ev.target.value;
    applyLayout();
  });
  const swapBtn = document.getElementById('swap-btn');
  if (swapBtn) swapBtn.addEventListener('click', swapPrimary);

  const syncToggle = document.getElementById('sync-toggle');
  if (syncToggle) {
    syncToggle.addEventListener('change', () => {
      syncEnabled = syncToggle.checked;
    });
  } else {
    // 没有同步开关（无 aux 或无 timing meta）→ 永久关 sync
    syncEnabled = false;
  }

  // 主流 timeupdate（每 ~250ms 触发一次）→ 同步辅流
  v1.addEventListener('timeupdate', () => {
    if (sources.main.video === v1) syncAuxToMain();
  });
  v2.addEventListener('timeupdate', () => {
    if (sources.main.video === v2) syncAuxToMain();
  });

  // 主流播放/暂停 → 立即同步状态
  function attachPlayPauseSync(video) {
    video.addEventListener('play',  () => syncAuxToMain());
    video.addEventListener('pause', () => syncAuxToMain());
    video.addEventListener('seeked', () => syncAuxToMain());
  }
  attachPlayPauseSync(v1);
  attachPlayPauseSync(v2);

  applyLayout();
})();
