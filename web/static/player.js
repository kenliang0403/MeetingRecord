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

  // --- ASR 字幕回放同步（电影字幕风格）------------------------------
  // 跟实时直播不同：回放场景下用户可暂停/拖动/快进，逐字浮现反而碎，
  // 看上去像流式输出。电影字幕的做法是 —— 每句完整显示，跟着音频同步进退，
  // 下一句来时一次性切换。
  //
  // ASR 返回的 final 一段可能 30 秒 100 字，必须先**拆**成多条短字幕：
  //   1. 按中文句末标点（。！？；）和句中标点（，、）作软切点
  //   2. 每段最长 ~35 字，超过强切
  //   3. 每段最短 1.5 秒，避免一闪而过
  //   4. 用 timestamps 按比例映射出每段的 start..end 时间
  //
  // 显示时：binary-search 当前应该哪段 → 整段固定显示 → 时间窗口过去切下一段
  // 暴露 window.player_reloadCaptions(useRefined) 给 refine UI 切换 raw/refined.
  const captionEl = document.getElementById('replay-caption');
  if (captionEl && window.TRANSCRIPT_URL && HAS_TIME_DATA) {
    const MAX_CHARS  = 35;     // 单条字幕最长字数（中文 ~ 2 行 18px）
    const MIN_DUR_S  = 1.5;    // 单条字幕最短秒数（不要一闪而过）
    const TAIL_HOLD  = 0.5;    // 末尾再保留秒数避免句末提前消失

    // --- 字幕开关 + 偏移（localStorage 持久化）--------------------
    // captionOffset > 0 把"当前时刻"视为已经过去了 N 秒，等于字幕提前 N 秒
    // 出现；负数则推后。用 number input + change/input 即时生效。
    const LS_ENABLED = 'replayCaptionEnabled_v2';   // v2: 默认关闭（旧键作废）
    const LS_OFFSET  = 'replayCaptionOffset';
    let captionsEnabled = false;                    // 默认不显示字幕，可在 UI 手动开
    let captionOffset   = 0;
    try {
      const v = localStorage.getItem(LS_ENABLED);
      if (v !== null) captionsEnabled = (v === '1');
      const o = localStorage.getItem(LS_OFFSET);
      if (o !== null) captionOffset = parseFloat(o) || 0;
    } catch {}
    const capToggleEl  = document.getElementById('cap-toggle');
    const capOffsetEl  = document.getElementById('cap-offset');
    if (capToggleEl) {
      capToggleEl.checked = captionsEnabled;
      capToggleEl.addEventListener('change', () => {
        captionsEnabled = capToggleEl.checked;
        try { localStorage.setItem(LS_ENABLED, captionsEnabled ? '1' : '0'); } catch {}
        updateCaption();
      });
    }
    if (capOffsetEl) {
      capOffsetEl.value = captionOffset.toFixed(1);
      capOffsetEl.addEventListener('input', () => {
        const v = parseFloat(capOffsetEl.value);
        captionOffset = Number.isFinite(v) ? v : 0;
        try { localStorage.setItem(LS_OFFSET, captionOffset.toFixed(2)); } catch {}
        updateCaption();
      });
    }
    // 句末标点：硬切；句中标点：软切（仅当当前段已经达到一定长度才切）
    const HARD_PUNCT = /[。！？]/;          // 。！？
    const SOFT_PUNCT = /[，；、：]/;     // ，；、：
    const MIN_HARD_CUT_CHARS = 4;
    const MIN_SOFT_CUT_CHARS = 12;

    let captions = [];
    let subtitles = [];    // 拆好的 cue 列表 [{ start, end, text, punct }]

    function buildSubtitles() {
      subtitles = [];
      for (const c of captions) {
        const ts = c.timestamps || [];
        if (!c.text) continue;
        if (!ts.length) {
          subtitles.push({
            start: c.meeting_offset_s,
            end:   c.meeting_offset_s + 5,
            text:  c.text,
            punct: c.punct,
          });
          continue;
        }
        // 1. 在 punct 文本上找切点
        const cuts = [0];
        for (let i = 0; i < c.text.length; i++) {
          const ch = c.text[i];
          const since = i + 1 - cuts[cuts.length - 1];
          if (HARD_PUNCT.test(ch) && since >= MIN_HARD_CUT_CHARS) {
            cuts.push(i + 1);
          } else if (SOFT_PUNCT.test(ch) && since >= MIN_SOFT_CUT_CHARS) {
            cuts.push(i + 1);
          }
        }
        if (cuts[cuts.length - 1] < c.text.length) cuts.push(c.text.length);

        // 2. 二次按字数强切
        const fine = [0];
        for (let i = 1; i < cuts.length; i++) {
          let cur = fine[fine.length - 1];
          while (cuts[i] - cur > MAX_CHARS) {
            cur += MAX_CHARS;
            fine.push(cur);
          }
          fine.push(cuts[i]);
        }

        // 3. 每段算时间窗口（按 punct text 字数比例映射回 raw_idx）
        const totalChars = c.text.length;
        const tEnd = ts[ts.length - 1];
        for (let i = 0; i < fine.length - 1; i++) {
          const aChar = fine[i], bChar = fine[i + 1];
          const text  = c.text.slice(aChar, bChar).trim();
          if (!text) continue;
          // 按比例换算到 raw_idx → timestamps 时间
          const rawA = Math.floor(ts.length * aChar / totalChars);
          const rawB = Math.min(ts.length - 1,
                                Math.max(rawA, Math.floor(ts.length * bChar / totalChars) - 1));
          const tA = (rawA < ts.length ? ts[rawA] : tEnd);
          // 段尾时间：下一段开头 - 0；最后一段直到 segment 结束
          let tB;
          if (i === fine.length - 2) {
            tB = tEnd;
          } else {
            const nextChar = fine[i + 1];
            const nextRaw  = Math.floor(ts.length * nextChar / totalChars);
            tB = (nextRaw < ts.length ? ts[nextRaw] : tEnd);
          }
          // 最短 MIN_DUR_S
          if (tB - tA < MIN_DUR_S) tB = tA + MIN_DUR_S;
          subtitles.push({
            start: c.meeting_offset_s + tA,
            end:   c.meeting_offset_s + tB + TAIL_HOLD,
            text:  text,
            punct: c.punct,
          });
        }
      }
      // 排序确保 binary-search 正确
      subtitles.sort((a, b) => a.start - b.start);
    }

    function loadCaptions(useRefined) {
      const url = window.TRANSCRIPT_URL + (useRefined === false ? '?refined=0' : '?refined=1');
      return fetch(url)
        .then(r => r.json())
        .then(j => {
          captions = (j && j.ok && Array.isArray(j.items)) ? j.items : [];
          buildSubtitles();
          captionEl.textContent = '';
          captionEl.classList.remove('final');
        })
        .catch(() => {});
    }
    window.player_reloadCaptions = loadCaptions;
    loadCaptions(true);

    function findCueAt(s) {
      let lo = 0, hi = subtitles.length - 1, found = -1;
      while (lo <= hi) {
        const mid = (lo + hi) >> 1;
        if (subtitles[mid].start <= s) { found = mid; lo = mid + 1; }
        else { hi = mid - 1; }
      }
      return found;
    }

    function setCaption(text, punct) {
      if (captionEl.textContent === text &&
          captionEl.classList.contains('final') === !!punct) return;
      captionEl.textContent = text;
      captionEl.classList.toggle('final', !!punct);
    }

    function updateCaption() {
      if (!captionsEnabled) return setCaption('');
      if (!subtitles.length) return;
      const mainSrc = sources.main;
      if (!mainSrc.video) return;
      const seg = MAIN[mainSrc.idx];
      if (!seg || seg.meeting_offset_ms == null) return;
      // captionOffset > 0 → 字幕提前：相当于把"当前时刻"虚拟前推
      const meetingS = (seg.meeting_offset_ms + mainSrc.video.currentTime * 1000) / 1000
                    + captionOffset;
      const idx = findCueAt(meetingS);
      if (idx < 0) return setCaption('');
      const cue = subtitles[idx];
      if (meetingS > cue.end) return setCaption('');
      setCaption(cue.text, cue.punct);
    }

    ['timeupdate', 'seeked', 'play', 'pause'].forEach(ev => {
      v1.addEventListener(ev, updateCaption);
      v2.addEventListener(ev, updateCaption);
    });
  }
})();
