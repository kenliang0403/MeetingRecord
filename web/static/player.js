// player.js — 回放页：主辅流双源播放器
//
// 设计：
//   - 一个主播放器 (#vid-primary) + 一个辅播放器 (#vid-secondary)
//   - "源"概念：MAIN 与 AUX 各有自己的片段列表 + 当前 idx
//   - 一个"播放器"绑定一个"源"。primarySource / secondarySource 指向 main 或 aux
//   - 布局模式控制可见性 + 大小：
//       main-only / aux-only：只显示主播放器（其源对应展示的那一路）
//       pip：主播放器大，辅播放器右下角小窗
//       split：左右各 50%
//   - 主画面切换：swap primary/secondary 的源
//   - 多片段无缝衔接：监听 ended 事件加载下一段，自动 play
//
// 简化决定：主辅流不做帧级时间对齐；两路独立，用户用 PiP 时自行 sync。

(function () {
  const MAIN = window.MAIN_SEGMENTS || [];
  const AUX  = window.AUX_SEGMENTS  || [];

  const stage = document.getElementById('stage');
  const v1 = document.getElementById('vid-primary');
  const v2 = document.getElementById('vid-secondary');

  // --- "源" 状态 ----------------------------------------------------
  // 每个源记录：files 列表、当前 idx、绑定的 video 元素引用
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

  // 当前 primary / secondary 各自播哪个源
  let primarySource   = 'main';
  let secondarySource = AUX.length ? 'aux' : null;
  let layout          = 'main-only';

  // --- helpers ------------------------------------------------------
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
      // 源置空：清掉 video src
      try { video.removeAttribute('src'); video.load(); } catch (_) {}
      return;
    }
    s.video = video;
    loadSegment(srcKey, s.idx, autoplay);
  }

  function applyLayout() {
    stage.classList.remove('layout-main-only','layout-aux-only','layout-pip','layout-split');
    stage.classList.add('layout-' + layout);

    // controls visibility — 仅主播放器有 controls，辅是辅助显示
    v1.controls = true;
    if (layout === 'split') {
      v2.controls = true;          // split 模式两个都给 controls，方便分别控制
    } else if (layout === 'pip') {
      v2.controls = false;         // pip 小窗不要 controls，避免误点
      v2.muted = true;             // pip 默认静音，只看画面
    } else {
      v2.controls = false;
    }

    // swap 按钮可用性
    const swapBtn = document.getElementById('swap-btn');
    if (swapBtn) {
      swapBtn.disabled = !(layout === 'pip' || layout === 'split');
    }

    // 单流模式时把对应源绑到 primary
    if (layout === 'main-only') {
      primarySource = 'main';
      secondarySource = null;
    } else if (layout === 'aux-only') {
      primarySource = 'aux';
      secondarySource = null;
    } else {
      // pip / split：必有 aux；保证两路都活着
      if (!primarySource)   primarySource   = 'main';
      if (!secondarySource) secondarySource = (primarySource === 'main') ? 'aux' : 'main';
    }
    bindSourceToVideo(primarySource,   v1, false);
    bindSourceToVideo(secondarySource, v2, false);
  }

  function swapPrimary() {
    if (!secondarySource) return;
    // 互换源；保留各自当前 idx 不变
    [primarySource, secondarySource] = [secondarySource, primarySource];
    // 记录两边播放进度，重绑后尽量恢复
    const p1 = v1.currentTime || 0;
    const p2 = v2.currentTime || 0;
    bindSourceToVideo(primarySource,   v1, false);
    bindSourceToVideo(secondarySource, v2, false);
    // 进度回填（同段内）
    v1.addEventListener('loadedmetadata', () => { try { v1.currentTime = p2; } catch (_) {} }, { once: true });
    v2.addEventListener('loadedmetadata', () => { try { v2.currentTime = p1; } catch (_) {} }, { once: true });
  }

  // --- 绑定按钮事件 -------------------------------------------------
  function wireSegmentButtons(srcKey) {
    const s = sources[srcKey];
    if (!s) return;
    if (s.prevBtn) s.prevBtn.addEventListener('click', () => loadSegment(srcKey, s.idx - 1, true));
    if (s.nextBtn) s.nextBtn.addEventListener('click', () => loadSegment(srcKey, s.idx + 1, true));
  }
  wireSegmentButtons('main');
  wireSegmentButtons('aux');

  // 当前 video 'ended' 时自动跳下一段
  function attachAutoChain(video) {
    video.addEventListener('ended', () => {
      // 找出这个 video 当前绑哪个源，下一段
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

  // 布局选择
  document.getElementById('layout-mode').addEventListener('change', (ev) => {
    layout = ev.target.value;
    applyLayout();
  });
  // 主画面切换
  const swapBtn = document.getElementById('swap-btn');
  if (swapBtn) swapBtn.addEventListener('click', swapPrimary);

  // --- 初始化 -------------------------------------------------------
  applyLayout();
})();
