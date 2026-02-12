const cropCanvas = document.getElementById("cropCanvas");
const previewCanvas = document.getElementById("previewCanvas");
const fileInput = document.getElementById("fileInput");
const zoomInput = document.getElementById("zoom");
const rotationInput = document.getElementById("rotation");
const ditherInput = document.getElementById("dither");
const roundMaskInput = document.getElementById("roundMask");
const convertBtn = document.getElementById("convertBtn");
const uploadBtn = document.getElementById("uploadBtn");
const downloadBtn = document.getElementById("downloadBtn");
const statusEl = document.getElementById("status");
const deviceUrlInput = document.getElementById("deviceUrl");

const PALETTE = [
  { name: "black", rgb: [0, 0, 0], code: 0x0 },
  { name: "white", rgb: [255, 255, 255], code: 0x1 },
  { name: "yellow", rgb: [255, 255, 0], code: 0x2 },
  { name: "red", rgb: [255, 0, 0], code: 0x3 },
  { name: "blue", rgb: [0, 0, 255], code: 0x5 },
  { name: "green", rgb: [0, 255, 0], code: 0x6 },
];

const PANEL_ROTATION_DEG = 180;

let sourceImage = null;
let scale = 1;
let offset = { x: 0, y: 0 };
let isDragging = false;
let dragStart = { x: 0, y: 0 };
let pendingRender = false;
let lastRawBytes = null;
let userRotationDeg = 0;

function setStatus(text) {
  statusEl.textContent = text;
}

function fitImage() {
  if (!sourceImage) return;
  const canvasSize = cropCanvas.width;
  const scaleX = canvasSize / sourceImage.width;
  const scaleY = canvasSize / sourceImage.height;
  scale = Math.max(scaleX, scaleY);
  zoomInput.value = scale.toFixed(2);
  offset.x = 0;
  offset.y = 0;
}

function drawCrop() {
  const ctx = cropCanvas.getContext("2d");
  ctx.clearRect(0, 0, cropCanvas.width, cropCanvas.height);
  ctx.fillStyle = "#f7f5f1";
  ctx.fillRect(0, 0, cropCanvas.width, cropCanvas.height);

  if (!sourceImage) return;
  const angle = (userRotationDeg * Math.PI) / 180;
  const center = cropCanvas.width / 2;
  ctx.translate(center + offset.x, center + offset.y);
  ctx.rotate(angle);
  ctx.scale(scale, scale);
  ctx.drawImage(sourceImage, -sourceImage.width / 2, -sourceImage.height / 2);
  ctx.setTransform(1, 0, 0, 1, 0, 0);
}

function scheduleRender() {
  if (pendingRender) return;
  pendingRender = true;
  requestAnimationFrame(() => {
    pendingRender = false;
    drawCrop();
    renderPreview();
  });
}

function nearestColor(r, g, b) {
  let best = PALETTE[0];
  let bestDist = Infinity;
  for (const entry of PALETTE) {
    const dr = r - entry.rgb[0];
    const dg = g - entry.rgb[1];
    const db = b - entry.rgb[2];
    const dist = dr * dr + dg * dg + db * db;
    if (dist < bestDist) {
      best = entry;
      bestDist = dist;
    }
  }
  return best;
}

function renderPreview() {
  const ctx = previewCanvas.getContext("2d");
  const cropCtx = cropCanvas.getContext("2d");
  const imageData = cropCtx.getImageData(0, 0, cropCanvas.width, cropCanvas.height);
  const data = imageData.data;

  if (ditherInput.checked) {
    floydSteinberg(data, cropCanvas.width, cropCanvas.height);
  } else {
    for (let i = 0; i < data.length; i += 4) {
      const color = nearestColor(data[i], data[i + 1], data[i + 2]);
      data[i] = color.rgb[0];
      data[i + 1] = color.rgb[1];
      data[i + 2] = color.rgb[2];
    }
  }

  if (roundMaskInput.checked) {
    applyRoundMask(data, cropCanvas.width, cropCanvas.height);
  }

  ctx.putImageData(imageData, 0, 0);
}

function applyRoundMask(data, width, height) {
  const cx = (width - 1) / 2;
  const cy = (height - 1) / 2;
  const radius = width / 2;
  const r2 = radius * radius;
  for (let y = 0; y < height; y++) {
    const dy = y - cy;
    for (let x = 0; x < width; x++) {
      const dx = x - cx;
      if (dx * dx + dy * dy > r2) {
        const idx = (y * width + x) * 4;
        data[idx] = 255;
        data[idx + 1] = 255;
        data[idx + 2] = 255;
      }
    }
  }
}

function floydSteinberg(data, width, height) {
  const buffer = new Float32Array(data.length);
  for (let i = 0; i < data.length; i++) buffer[i] = data[i];

  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const idx = (y * width + x) * 4;
      const oldR = buffer[idx];
      const oldG = buffer[idx + 1];
      const oldB = buffer[idx + 2];
      const color = nearestColor(oldR, oldG, oldB);
      const newR = color.rgb[0];
      const newG = color.rgb[1];
      const newB = color.rgb[2];

      buffer[idx] = newR;
      buffer[idx + 1] = newG;
      buffer[idx + 2] = newB;

      const errR = oldR - newR;
      const errG = oldG - newG;
      const errB = oldB - newB;

      distributeError(buffer, width, height, x + 1, y, errR, errG, errB, 7 / 16);
      distributeError(buffer, width, height, x - 1, y + 1, errR, errG, errB, 3 / 16);
      distributeError(buffer, width, height, x, y + 1, errR, errG, errB, 5 / 16);
      distributeError(buffer, width, height, x + 1, y + 1, errR, errG, errB, 1 / 16);
    }
  }

  for (let i = 0; i < data.length; i++) {
    data[i] = Math.max(0, Math.min(255, buffer[i]));
  }
}

function distributeError(buffer, width, height, x, y, errR, errG, errB, factor) {
  if (x < 0 || y < 0 || x >= width || y >= height) return;
  const idx = (y * width + x) * 4;
  buffer[idx] += errR * factor;
  buffer[idx + 1] += errG * factor;
  buffer[idx + 2] += errB * factor;
}

function packSp6() {
  const ctx = previewCanvas.getContext("2d");
  const imageData = ctx.getImageData(0, 0, previewCanvas.width, previewCanvas.height);
  const data = imageData.data;
  const width = previewCanvas.width;
  const height = previewCanvas.height;
  const out = new Uint8Array((width * height) / 2);
  const rotatePanel = ((PANEL_ROTATION_DEG % 360) + 360) % 360;

  let outIndex = 0;
  for (let y = height - 1; y >= 0; y--) {
    for (let x = width - 1; x >= 0; x--) {
      let srcX = x;
      let srcY = y;
      if (rotatePanel === 180) {
        srcX = width - 1 - x;
        srcY = height - 1 - y;
      }
      const idx = (srcY * width + srcX) * 4;
      const color = nearestColor(data[idx], data[idx + 1], data[idx + 2]);
      if ((x & 1) === 0) {
        out[outIndex] = color.code << 4;
      } else {
        out[outIndex] |= color.code & 0x0f;
        outIndex++;
      }
    }
  }

  return out;
}

fileInput.addEventListener("change", (event) => {
  const file = event.target.files[0];
  if (!file) return;
  const reader = new FileReader();
  reader.onload = () => {
    const img = new Image();
    img.onload = () => {
      sourceImage = img;
      fitImage();
      scheduleRender();
      setStatus("Image loaded");
    };
    img.src = reader.result;
  };
  reader.readAsDataURL(file);
});

zoomInput.addEventListener("input", () => {
  scale = Number(zoomInput.value);
  scheduleRender();
});

rotationInput.addEventListener("change", () => {
  userRotationDeg = Number(rotationInput.value);
  scheduleRender();
});

cropCanvas.addEventListener("pointerdown", (event) => {
  isDragging = true;
  dragStart = { x: event.clientX - offset.x, y: event.clientY - offset.y };
  cropCanvas.setPointerCapture(event.pointerId);
});

cropCanvas.addEventListener("pointermove", (event) => {
  if (!isDragging) return;
  offset.x = event.clientX - dragStart.x;
  offset.y = event.clientY - dragStart.y;
  scheduleRender();
});

cropCanvas.addEventListener("pointerup", (event) => {
  isDragging = false;
  cropCanvas.releasePointerCapture(event.pointerId);
});

cropCanvas.addEventListener("pointerleave", () => {
  isDragging = false;
});

convertBtn.addEventListener("click", () => {
  if (!sourceImage) {
    setStatus("Load an image first");
    return;
  }
  renderPreview();
  lastRawBytes = packSp6();
  setStatus("Converted to sp6");
});

uploadBtn.addEventListener("click", async () => {
  if (!lastRawBytes) {
    renderPreview();
    lastRawBytes = packSp6();
  }
  const url = deviceUrlInput.value || "/image";
  setStatus("Uploading...");
  try {
    const response = await fetch(url, {
      method: "POST",
      headers: { "Content-Type": "application/octet-stream" },
      body: lastRawBytes,
    });
    if (!response.ok) {
      throw new Error(`Upload failed: ${response.status}`);
    }
    setStatus("Upload complete");
  } catch (err) {
    setStatus(`Upload failed: ${err.message}`);
  }
});

downloadBtn.addEventListener("click", () => {
  if (!lastRawBytes) {
    setStatus("Convert first");
    return;
  }
  const blob = new Blob([lastRawBytes], { type: "application/octet-stream" });
  const url = URL.createObjectURL(blob);
  const link = document.createElement("a");
  link.href = url;
  link.download = "image.sp6";
  document.body.appendChild(link);
  link.click();
  document.body.removeChild(link);
  URL.revokeObjectURL(url);
});

scheduleRender();
