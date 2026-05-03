// VU meter Canvas renderer — auto orientation (W>H = 横; H>W = 竖)
// dBFS 范围 -60..0，颜色分段：绿(<-12)、黄(-12..-3)、红(>-3)
// 浏览器 ~10 Hz 拉 /api/levels 触发 update
(function () {
  const MIN_DB = -60;
  const MAX_DB = 0;
  const SEG_GREEN_TOP = -12;
  const SEG_AMBER_TOP = -3;

  function makeBar(canvasId, numId) {
    const cvs = document.getElementById(canvasId);
    const ctx = cvs.getContext("2d");
    const numEl = document.getElementById(numId);
    const W = cvs.width;
    const H = cvs.height;
    const horizontal = W > H;

    function draw(dbfs) {
      ctx.fillStyle = "#11141a";
      ctx.fillRect(0, 0, W, H);

      const v = Math.max(MIN_DB, Math.min(MAX_DB, dbfs));
      const ratio = (v - MIN_DB) / (MAX_DB - MIN_DB);

      let grad;
      if (horizontal) {
        grad = ctx.createLinearGradient(0, 0, W, 0);
      } else {
        grad = ctx.createLinearGradient(0, H, 0, 0);
      }
      grad.addColorStop(0, "#22c55e");
      grad.addColorStop((SEG_GREEN_TOP - MIN_DB) / 60, "#22c55e");
      grad.addColorStop((SEG_GREEN_TOP - MIN_DB) / 60 + 0.001, "#facc15");
      grad.addColorStop((SEG_AMBER_TOP - MIN_DB) / 60, "#facc15");
      grad.addColorStop((SEG_AMBER_TOP - MIN_DB) / 60 + 0.001, "#ef4444");
      grad.addColorStop(1, "#ef4444");
      ctx.fillStyle = grad;

      if (horizontal) {
        const barW = Math.round(W * ratio);
        ctx.fillRect(0, 2, barW, H - 4);
        // 6 dB tick marks (vertical lines)
        ctx.fillStyle = "#374151";
        for (let db = MAX_DB; db >= MIN_DB; db -= 6) {
          const x = Math.round(W * ((db - MIN_DB) / (MAX_DB - MIN_DB)));
          ctx.fillRect(x, 0, 1, H);
        }
      } else {
        const barH = Math.round(H * ratio);
        ctx.fillRect(4, H - barH, W - 8, barH);
        ctx.fillStyle = "#374151";
        for (let db = MAX_DB; db >= MIN_DB; db -= 6) {
          const y = Math.round(H * (1 - (db - MIN_DB) / (MAX_DB - MIN_DB)));
          ctx.fillRect(0, y, W, 1);
        }
      }

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
