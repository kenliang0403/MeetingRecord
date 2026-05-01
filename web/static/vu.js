// VU meter Canvas renderer
// 单条柱形：dBFS 范围 -60..0，颜色分段：绿(<-12)、黄(-12..-3)、红(>-3)
// 浏览器 ~10 Hz 拉 /api/levels 触发 update
(function () {
  const MIN_DB = -60;
  const MAX_DB = 0;
  // segment thresholds in dBFS
  const SEG_GREEN_TOP = -12;  // < -12 = green
  const SEG_AMBER_TOP = -3;   // -12..-3 = amber, > -3 = red

  function makeBar(canvasId, numId) {
    const cvs = document.getElementById(canvasId);
    const ctx = cvs.getContext("2d");
    const numEl = document.getElementById(numId);
    const W = cvs.width;
    const H = cvs.height;

    function draw(dbfs) {
      ctx.fillStyle = "#11141a";
      ctx.fillRect(0, 0, W, H);

      // clamp + map dbfs -> bar height
      const v = Math.max(MIN_DB, Math.min(MAX_DB, dbfs));
      const ratio = (v - MIN_DB) / (MAX_DB - MIN_DB);  // 0..1
      const barH = Math.round(H * ratio);
      const top = H - barH;

      // gradient fill in segments
      const grad = ctx.createLinearGradient(0, H, 0, 0);
      grad.addColorStop(0, "#22c55e");                       // bottom green
      grad.addColorStop((SEG_GREEN_TOP - MIN_DB) / 60, "#22c55e");
      grad.addColorStop((SEG_GREEN_TOP - MIN_DB) / 60 + 0.001, "#facc15");
      grad.addColorStop((SEG_AMBER_TOP - MIN_DB) / 60, "#facc15");
      grad.addColorStop((SEG_AMBER_TOP - MIN_DB) / 60 + 0.001, "#ef4444");
      grad.addColorStop(1, "#ef4444");                       // top red
      ctx.fillStyle = grad;
      ctx.fillRect(4, top, W - 8, barH);

      // tick marks every 6 dB
      ctx.fillStyle = "#374151";
      for (let db = MAX_DB; db >= MIN_DB; db -= 6) {
        const y = Math.round(H * (1 - (db - MIN_DB) / (MAX_DB - MIN_DB)));
        ctx.fillRect(0, y, W, 1);
      }
      // numeric label
      if (numEl) {
        if (dbfs <= MIN_DB + 0.5) numEl.textContent = "—";
        else numEl.textContent = dbfs.toFixed(1);
      }
    }
    draw(MIN_DB);
    return { draw };
  }

  window.makeVuBar = makeBar;
})();
