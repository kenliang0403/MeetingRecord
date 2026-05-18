// player.js — 回放页：主辅流双源播放器 + meeting timeline 同步 + 播放列表
//
// 设计要点：
//   - 一个 #vid-primary + 一个 #vid-secondary，绑到 main / aux 源
//   - 4 种布局（segmented buttons）：main-only / aux-only / pip / split
//   - 多片段 chained playback：ended → 自动下一段
//   - playlist 直接点跳；当前播放高亮
//   - swap 按钮：互换 primary/secondary 源
//   - 主辅流同步（默认 ON）：主流 timeupdate 拉动辅流
//     * 同步开启时，点辅流 playlist 实际是把主流 seek 到对应 meeting time
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
    main: { name: '主流', files: MAIN, idx: 0, video: null },
    aux:  { name: '辅流', files: AUX,  idx: 0, video: null },
  };
  let primarySource   = 'main';
  let secondarySource = AUX.length ? 'aux' : null;
  let layout          = 'main-only';

  let syncEnabled = HAS_TIME_DATA && AUX.length > 0;
  let auxTouchUntil = 0;

  // --- 播放列表 active 高亮 ----------------------------------------
  function refreshPlaylist() {
    document.querySelectorAll('.playlist-item').forEach(el => {
      const src = el.dataset.source;
      const idx = parseInt(el.dataset.idx, 10);
      el.classList.toggle('active', sources[src] && sources[src].idx === idx);
    });
  }

  // --- 加载段 / 绑定源 --------------------------------------------
  function loadSegment(srcKey, idx, autoplay) {
    const s = sources[srcKey];
    if (!s.files.length) return;
    if (idx < 0) idx = 0;
    if (idx >= s.files.length) idx = s.files.length - 1;
    s.idx = idx;
    const seg = s.files[idx];
    if (s.video) {
      s.video.src = seg.url;
      if (autoplay) s.video.play().catch(() => {});
    }
    refreshPlaylist();
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

  // --- 布局 -------------------------------------------------------
  function applyLayout() {
    stage.classList.remove('layout-main-only','layout-aux-only','layout-pip','layout-split');
    stage.classList.add('layout-' + layout);
    v1.controls = true;
    if (layout === 'split') v2.controls = true;
    else if (layout === 'pip') { v2.controls = false; v2.muted = true; }
    else v2.controls = false;

    const swapBtn = document.getElementById('swap-btn');
    if (swapBtn) swapBtn.disabled = !(layout === 'pip' || layout === 'split');

    // 标记按钮组 active
    document.querySelectorAll('#layout-segments button').forEach(b => {
      b.classList.toggle('active', b.dataset.layout === layout);
    });

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

  // --- 主辅流时间同步 ---------------------------------------------
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
    if (sources.aux.video === v1) return v1;
    if (sources.aux.video === v2) return v2;
    return null;
  }

  function syncAuxToMain() {
    if (!syncEnabled || !HAS_TIME_DATA) return;
    if (Date.now() < auxTouchUntil) return;
    if (layout === 'main-only' || layout === 'aux-only') return;

    const mainVideo = sources.main.video;
    if (!mainVideo) return;
    const mainSeg = MAIN[sources.main.idx];
    if (!mainSeg || mainSeg.meeting_offset_ms == null) return;
    const t = mainSeg.meeting_offset_ms + (mainVideo.currentTime * 1000);

    const auxVideo = getAuxVideo();
    if (!auxVideo) return;

    const target = findContainingAux(t);
    if (!target) {
      if (!auxVideo.paused) try { auxVideo.pause(); } catch (_) {}
      return;
    }
    if (sources.aux.idx !== target.idx) {
      sources.aux.idx = target.idx;
      const seg = AUX[target.idx];
      auxVideo.src = seg.url;
      const offSec = target.offsetInSegMs / 1000;
      auxVideo.addEventListener('loadedmetadata', () => {
        try { auxVideo.currentTime = offSec; } catch (_) {}
        if (!mainVideo.paused) auxVideo.play().catch(() => {});
      }, { once: true });
      refreshPlaylist();
      return;
    }
    const wantSec = target.offsetInSegMs / 1000;
    const drift   = Math.abs(auxVideo.currentTime - wantSec);
    if (drift > 0.5) {
      try { auxVideo.currentTime = wantSec; } catch (_) {}
    }
    if (mainVideo.paused && !auxVideo.paused) try { auxVideo.pause(); } catch (_) {}
    else if (!mainVideo.paused && auxVideo.paused) auxVideo.play().catch(() => {});
  }

  // --- 跳到某 aux 段对应的主流时刻 --------------------------------
  function jumpMainToAuxStart(auxIdx) {
    if (auxIdx < 0 || auxIdx >= AUX.length) return;
    const aux = AUX[auxIdx];
    if (!aux || aux.meeting_offset_ms == null) return;
    const t = aux.meeting_offset_ms;
    for (let i = 0; i < MAIN.length; i++) {
      const m = MAIN[i];
      if (m.meeting_offset_ms == null || m.duration_ms == null) continue;
      const start = m.meeting_offset_ms;
      const end   = m.meeting_offset_ms + m.duration_ms;
      if (t >= start && t < end) {
        const mv = sources.main.video;
        const offSec = (t - start) / 1000;
        if (sources.main.idx !== i) {
          sources.main.idx = i;
          mv.src = m.url;
          mv.addEventListener('loadedmetadata', () => {
            try { mv.currentTime = offSec; } catch (_) {}
            mv.play().catch(() => {});
          }, { once: true });
          refreshPlaylist();
        } else {
          try { mv.currentTime = offSec; } catch (_) {}
          mv.play().catch(() => {});
        }
        return;
      }
    }
    auxTouchUntil = Date.now() + 5000;
    loadSegment('aux', auxIdx, true);
  }

  // --- playlist 点击 ----------------------------------------------
  document.querySelectorAll('.playlist-item').forEach(el => {
    el.addEventListener('click', () => {
      const src = el.dataset.source;
      const idx = parseInt(el.dataset.idx, 10);
      if (src === 'aux' && syncEnabled) {
        // 同步模式下，点 aux 是"跳到对应主流时刻"
        jumpMainToAuxStart(idx);
      } else {
        if (src === 'aux') auxTouchUntil = Date.now() + 5000;
        loadSegment(src, idx, true);
      }
    });
  });

  // --- ended → 自动下一段 -----------------------------------------
  function attachAutoChain(video) {
    video.addEventListener('ended', () => {
      const key = (sources.main.video === video) ? 'main' :
                  (sources.aux.video  === video) ? 'aux'  : null;
      if (!key) return;
      const s = sources[key];
      if (s.idx + 1 < s.files.length) loadSegment(key, s.idx + 1, true);
    });
  }
  attachAutoChain(v1);
  attachAutoChain(v2);

  // --- 布局按钮组 ------------------------------------------------
  document.querySelectorAll('#layout-segments button').forEach(b => {
    b.addEventListener('click', () => {
      layout = b.dataset.layout;
      applyLayout();
    });
  });

  const swapBtn = document.getElementById('swap-btn');
  if (swapBtn) swapBtn.addEventListener('click', swapPrimary);

  // --- 同步开关 + UI hint -----------------------------------------
  const syncToggle = document.getElementById('sync-toggle');
  const auxHint    = document.getElementById('aux-hint');
  function refreshAuxHint() {
    if (!auxHint) return;
    auxHint.textContent = syncEnabled
      ? '已开启同步：辅流列表点击跳到对应主流时刻'
      : '已关闭同步：辅流独立播放';
  }
  if (syncToggle) {
    syncToggle.addEventListener('change', () => {
      syncEnabled = syncToggle.checked;
      refreshAuxHint();
    });
    refreshAuxHint();
  } else {
    syncEnabled = false;
    if (auxHint) auxHint.textContent = '';
  }

  // --- timeupdate / play / pause / seeked → sync -------------------
  v1.addEventListener('timeupdate', () => { if (sources.main.video === v1) syncAuxToMain(); });
  v2.addEventListener('timeupdate', () => { if (sources.main.video === v2) syncAuxToMain(); });
  function attachPlayPauseSync(video) {
    video.addEventListener('play',   () => syncAuxToMain());
    video.addEventListener('pause',  () => syncAuxToMain());
    video.addEventListener('seeked', () => syncAuxToMain());
  }
  attachPlayPauseSync(v1);
  attachPlayPauseSync(v2);

  applyLayout();
  refreshPlaylist();

  // --- ASR 字幕回放同步（按主流 currentTime）--------------------------
  // /recordings/<m>/transcript.json 返回的 items 已按 meeting_offset_s 排序，
  // 每条对应一句 ASR final，含：
  //   meeting_offset_s — 该句相对 meeting 起点的秒数（== segment 开始时刻）
  //   text             — punct 版本（若有）否则 raw
  //   timestamps       — 每字相对 segment start_time 的秒数（typewriter 效果）
  //   punct            — 是否含标点
  //
  // 显示策略（解决"一次显示太多 / 话没说完就消失"两个问题）：
  //   - 用 timestamps 算"当前应播到第几字"，逐字浮现，跟说话节奏同步
  //   - 限制窗口最多 MAX_VISIBLE_CHARS 字符，超长就 ellipsis prefix 滚动
  //   - segment 结束时间 = max(timestamps) + EXTRA_HOLD_S，过了才清空；
  //     这样长句不会播一半字幕就空了
  //   - 下一个 segment 来时立即整体切换（不会两句重叠）
  //
  // 暴露 window.player_reloadCaptions(useRefined) 给 refine UI 切换 raw/refined.
  const captionEl = document.getElementById('replay-caption');
  if (captionEl && window.TRANSCRIPT_URL && HAS_TIME_DATA) {
    const MAX_VISIBLE_CHARS = 70;   // 屏幕上一次最多多少字
    const EXTRA_HOLD_S      = 2;    // 句末再保留 N 秒避免提早清空

    let captions = [];

    function loadCaptions(useRefined) {
      const url = window.TRANSCRIPT_URL + (useRefined === false ? '?refined=0' : '?refined=1');
      return fetch(url)
        .then(r => r.json())
        .then(j => {
          captions = (j && j.ok && Array.isArray(j.items)) ? j.items : [];
          captionEl.textContent = '';
          captionEl.classList.remove('final');
        })
        .catch(() => {});
    }
    window.player_reloadCaptions = loadCaptions;
    loadCaptions(true);

    // binary-search 最后一个 meeting_offset_s <= s 的 caption
    function findCaptionAt(s) {
      let lo = 0, hi = captions.length - 1, found = -1;
      while (lo <= hi) {
        const mid = (lo + hi) >> 1;
        if (captions[mid].meeting_offset_s <= s) { found = mid; lo = mid + 1; }
        else { hi = mid - 1; }
      }
      return found;
    }

    // 把"逐字时间戳"按 raw 字数索引，但展示的 c.text 可能是 punct 版本
    // （字数不同）。简单按比例换算 raw_idx → punct_idx。
    function visibleSlice(text, timestamps, segLocalT) {
      if (!timestamps || timestamps.length === 0) {
        return text;   // 没 timestamp 信息直接全显示
      }
      // raw 字数中已播放到第几字
      let rawShown = 0;
      for (let i = 0; i < timestamps.length; i++) {
        if (timestamps[i] <= segLocalT) rawShown = i + 1;
        else break;
      }
      if (rawShown === 0) return '';
      let cut;
      if (rawShown >= timestamps.length) {
        cut = text.length;
      } else {
        // text 可能比 timestamps 长（punct 加了标点），按比例换算
        cut = Math.ceil(text.length * rawShown / timestamps.length);
      }
      let shown = text.slice(0, cut);
      if (shown.length > MAX_VISIBLE_CHARS) {
        shown = '…' + shown.slice(shown.length - MAX_VISIBLE_CHARS);
      }
      return shown;
    }

    function setCaption(text, punct) {
      if (captionEl.textContent === text && captionEl.classList.contains('final') === !!punct) {
        return;   // 没变化就别 reflow
      }
      captionEl.textContent = text;
      captionEl.classList.toggle('final', !!punct);
    }

    function updateCaption() {
      if (!captions.length) return;
      const mainSrc = sources.main;
      if (!mainSrc.video) return;
      const seg = MAIN[mainSrc.idx];
      if (!seg || seg.meeting_offset_ms == null) return;

      const meetingS = (seg.meeting_offset_ms + mainSrc.video.currentTime * 1000) / 1000;
      const idx = findCaptionAt(meetingS);
      if (idx < 0) return setCaption('');

      const c = captions[idx];
      const ts = c.timestamps || [];
      const segLocalT = meetingS - c.meeting_offset_s;
      if (segLocalT < 0) return setCaption('');

      // 句末超过 timestamps 最后一个 + EXTRA_HOLD_S 才清空
      const segEnd = ts.length ? ts[ts.length - 1] : 5;
      if (segLocalT > segEnd + EXTRA_HOLD_S) return setCaption('');

      setCaption(visibleSlice(c.text, ts, segLocalT), c.punct);
    }

    ['timeupdate', 'seeked', 'play', 'pause'].forEach(ev => {
      v1.addEventListener(ev, updateCaption);
      v2.addEventListener(ev, updateCaption);
    });
  }
})();
