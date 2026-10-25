"use strict";

// Rasterizes an SVG string to a PNG data URL. Mermaid (htmlLabels: false) and PlantUML draw their labels as
// plain SVG text, so there is no <foreignObject> and the canvas is not tainted.
function svgToPng(svg, scale) {
  return new Promise((resolve, reject) => {
    const doc = new DOMParser().parseFromString(svg, "image/svg+xml");
    const el = doc.documentElement;
    let w = 0, h = 0;
    const vb = el.getAttribute("viewBox");
    if (vb) {
      const p = vb.trim().split(/[\s,]+/).map(Number);
      w = p[2]; h = p[3];
    }
    w = w || parseFloat(el.getAttribute("width")) || 800;
    h = h || parseFloat(el.getAttribute("height")) || 600;
    el.setAttribute("width", w);
    el.setAttribute("height", h);
    el.removeAttribute("style");
    const xml = new XMLSerializer().serializeToString(el);
    const img = new Image();
    img.onload = () => {
      const c = document.createElement("canvas");
      c.width = Math.ceil(w * scale);
      c.height = Math.ceil(h * scale);
      const ctx = c.getContext("2d");
      ctx.scale(scale, scale);
      ctx.drawImage(img, 0, 0, w, h);
      resolve(c.toDataURL("image/png"));
    };
    img.onerror = () => reject(new Error("could not rasterize the diagram"));
    img.src = "data:image/svg+xml;charset=utf-8," + encodeURIComponent(xml);
  });
}
