"use strict";

const $ = (id) => document.getElementById(id);
const expand =
  '<svg viewBox="0 0 24 24"><path d="M4 9V4h5M15 4h5v5M20 15v5h-5M9 20H4v-5"/></svg>';
const kinds = ["original", "h264", "semantic"];
const labels = { original: "原图", h264: "H.264", semantic: "语义压缩" };
const players = new Map();
const histories = new Map();
let config,
  paused = false,
  selected = null,
  filter = "all",
  refreshing = false;
let draft,
  editIndex = 0,
  toastTimer;

const escapeHtml = (value) =>
  String(value).replace(
    /[&<>"']/g,
    (c) =>
      ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[
        c
      ],
  );
const number = (value, digits = 1) =>
  Number.isFinite(value)
    ? value.toLocaleString("en-US", {
        minimumFractionDigits: digits,
        maximumFractionDigits: digits,
      })
    : "—";
const keyFor = (channel, kind) => `${channel.id}-${kind}`;

function toast(message, error = false) {
  clearTimeout(toastTimer);
  $("toast").textContent = message;
  $("toast").classList.toggle("error", error);
  $("toast").hidden = false;
  toastTimer = setTimeout(
    () => {
      $("toast").hidden = true;
    },
    error ? 6500 : 3000,
  );
}

async function api(path, body) {
  const response = await fetch(path, {
    method: body === undefined ? "GET" : "POST",
    headers: { "Content-Type": "application/json" },
    body: body === undefined ? undefined : JSON.stringify(body),
    signal: AbortSignal.timeout(10000),
  });
  const result = await response.json();
  if (!response.ok)
    throw new Error(result.error || `请求失败（${response.status}）`);
  return result;
}

class FrameDelay {
  constructor(video, milliseconds) {
    this.video = video;
    this.milliseconds = milliseconds;
    this.queue = [];
    this.epoch = 0;
    this.active = false;
    this.pending = 0;
    this.maxFrames = Math.max(12, Math.ceil((milliseconds * 60) / 1000) + 8);
    this.canvas = document.createElement("canvas");
    this.canvas.className = "delayed-video";
    this.canvas.setAttribute("aria-hidden", "true");
    video.after(this.canvas);
    this.context = this.canvas.getContext("2d", { alpha: false });
  }
  start() {
    if (this.active) return;
    this.active = true;
    this.canvas.hidden = false;
    const epoch = ++this.epoch;
    const capture = () => {
      if (!this.active || epoch !== this.epoch) return;
      this.captureToken = this.video.requestVideoFrameCallback(capture);
      if (this.video.readyState < 2 || this.pending >= 2) return;
      // Use one clock for both queues. Callback timestamps can be stale when
      // a background tab resumes, and expectedDisplayTime is only a prediction.
      const capturedAt = performance.now();
      this.pending++;
      let image;
      try {
        image =
          typeof VideoFrame === "function"
            ? new VideoFrame(this.video, {
                timestamp: Math.round(capturedAt * 1000),
              })
            : createImageBitmap(this.video);
      } catch (error) {
        image = Promise.reject(error);
      }
      Promise.resolve(image)
        .then((frame) => {
          if (!this.active || epoch !== this.epoch) {
            frame.close();
            return;
          }
          this.queue.push({ frame, capturedAt });
          this.queue.sort((a, b) => a.capturedAt - b.capturedAt);
          while (this.queue.length > this.maxFrames)
            this.queue.shift().frame.close();
        })
        .catch(() => {
          if (epoch === this.epoch) {
            this.stop();
            this.canvas.hidden = true;
          }
        })
        .finally(() => {
          if (epoch === this.epoch) this.pending--;
        });
    };
    const paint = () => {
      if (!this.active || epoch !== this.epoch) return;
      const now = performance.now();
      let next;
      while (
        this.queue.length &&
        now - this.queue[0].capturedAt >= this.milliseconds
      ) {
        next?.frame.close();
        next = this.queue.shift();
      }
      // Under unusually high source FPS, reduce compensation rather than
      // keeping a permanently black picture behind an undersized queue.
      if (!next && this.queue.length >= this.maxFrames)
        next = this.queue.shift();
      if (next) {
        const width = next.frame.displayWidth || next.frame.width;
        const height = next.frame.displayHeight || next.frame.height;
        if (this.canvas.width !== width || this.canvas.height !== height) {
          this.canvas.width = width;
          this.canvas.height = height;
        }
        this.context.drawImage(next.frame, 0, 0, width, height);
        this.canvas.dataset.delayMs = String(now - next.capturedAt);
        next.frame.close();
      }
      this.paintToken = requestAnimationFrame(paint);
    };
    if (typeof this.video.requestVideoFrameCallback !== "function") {
      this.canvas.hidden = true;
      this.active = false;
      return;
    }
    this.captureToken = this.video.requestVideoFrameCallback(capture);
    this.paintToken = requestAnimationFrame(paint);
  }
  stop() {
    this.active = false;
    this.epoch++;
    if (this.captureToken !== undefined)
      this.video.cancelVideoFrameCallback(this.captureToken);
    cancelAnimationFrame(this.paintToken);
    for (const item of this.queue) item.frame.close();
    this.queue = [];
    this.pending = 0;
    delete this.canvas.dataset.delayMs;
    this.context.clearRect(0, 0, this.canvas.width, this.canvas.height);
  }
  destroy() {
    this.stop();
    this.canvas.remove();
  }
}

function gatherIce(peer, signal) {
  if (peer.iceGatheringState === "complete") return Promise.resolve();
  return new Promise((resolve) => {
    const finish = () => {
      clearTimeout(timer);
      peer.removeEventListener("icegatheringstatechange", change);
      signal.removeEventListener("abort", finish);
      resolve();
    };
    const change = () => {
      if (peer.iceGatheringState === "complete") finish();
    };
    const timer = setTimeout(finish, 1500);
    peer.addEventListener("icegatheringstatechange", change);
    signal.addEventListener("abort", finish, { once: true });
  });
}

class Player {
  constructor(channel, kind) {
    this.channel = channel;
    this.kind = kind;
    this.key = keyFor(channel, kind);
    this.video = $(this.key + "-video");
    this.generation = 0;
    this.live = false;
    const delay = kind === "original" ? config.original_delay_ms : kind === "h264" ? config.h264_delay_ms : 0;
    this.delay =
      delay > 0
        ? new FrameDelay(this.video, delay)
        : null;
    this.video.addEventListener("playing", () => {
      if (!this.peer) return;
      this.receivedFrame();
      this.delay?.start();
    });
  }
  receivedFrame() {
    this.hasFrames = true;
    this.lastFrameAt = performance.now();
    clearTimeout(this.retry);
    this.retry = null;
    this.state("播放中", true);
  }
  state(text, live = false) {
    this.live = live;
    $(this.key).classList.toggle("live", live);
    $(this.key + "-empty").textContent = text;
    updateLive();
  }
  release(session) {
    if (session)
      fetch(session, { method: "DELETE", keepalive: true }).catch(() => {});
  }
  close(text = "已暂停") {
    this.generation++;
    clearTimeout(this.retry);
    this.retry = null;
    this.abort?.abort();
    this.abort = null;
    this.delay?.stop();
    this.release(this.session);
    this.session = null;
    if (this.peer) {
      this.peer.ontrack = this.peer.onconnectionstatechange = null;
      this.peer.close();
      this.peer = null;
    }
    this.video.srcObject = null;
    this.sample = null;
    this.metrics = null;
    this.hasFrames = false;
    this.lastFrameAt = performance.now();
    $(this.key + "-caption").textContent = "— × — · — FPS";
    this.state(text);
  }
  schedule() {
    if (paused || this.retry) return;
    this.retry = setTimeout(() => {
      this.retry = null;
      this.connect();
    }, 3500);
  }
  async connect() {
    this.close("连接中");
    if (paused) return;
    const generation = this.generation;
    const abort = new AbortController();
    this.abort = abort;
    const timeout = setTimeout(() => abort.abort(), 12000);
    try {
      const peer = new RTCPeerConnection({ iceServers: [] });
      this.peer = peer;
      peer.addTransceiver("video", { direction: "recvonly" });
      peer.ontrack = (event) => {
        if (generation !== this.generation) return;
        this.video.srcObject =
          event.streams[0] || new MediaStream([event.track]);
        this.video.play().catch(() => {
          if (generation === this.generation)
            this.state("点击重连以播放");
        });
      };
      peer.onconnectionstatechange = () => {
        if (generation !== this.generation) return;
        if (
          ["failed", "disconnected", "closed"].includes(peer.connectionState)
        ) {
          this.state("连接断开");
          this.schedule();
        }
      };
      await peer.setLocalDescription(await peer.createOffer());
      await gatherIce(peer, abort.signal);
      if (generation !== this.generation) return;
      const response = await fetch(
        `/api/whep/${this.channel.id}/${this.kind}`,
        {
          method: "POST",
          headers: { "Content-Type": "application/sdp" },
          body: peer.localDescription.sdp,
          signal: abort.signal,
        },
      );
      if (!response.ok) throw new Error("视频源暂不可用");
      const session = response.headers.get("Location");
      if (generation !== this.generation) {
        this.release(session);
        return;
      }
      this.session = session;
      const answer = await response.text();
      if (generation !== this.generation) return;
      await peer.setRemoteDescription({ type: "answer", sdp: answer });
    } catch (error) {
      if (generation !== this.generation) return;
      this.close("无信号");
      this.schedule();
    } finally {
      clearTimeout(timeout);
    }
  }
  async stats() {
    const peer = this.peer;
    if (!peer || peer.connectionState !== "connected") return;
    try {
      const reports = await peer.getStats();
      if (peer !== this.peer) return;
      for (const report of reports.values()) {
        if (
          report.type !== "inbound-rtp" ||
          (report.kind !== "video" && report.mediaType !== "video")
        )
          continue;
        let fps = report.framesPerSecond;
        if (
          this.sample &&
          report.timestamp > this.sample.time &&
          !Number.isFinite(fps)
        ) {
          fps = Math.max(
            0,
            ((report.framesDecoded - this.sample.frames) * 1000) /
              (report.timestamp - this.sample.time),
          );
        }
        if (report.framesDecoded > (this.sample?.frames || 0))
          this.receivedFrame();
        if (this.sample && report.timestamp > this.sample.time && report.framesDecoded > this.sample.frames) {
          const bytes = report.bytesReceived - this.sample.bytes;
          const frames = report.framesDecoded - this.sample.frames;
          const pixels = this.video.videoWidth * this.video.videoHeight;
          if (bytes >= 0 && pixels > 0) {
            const bpp = bytes * 8 / (frames * pixels);
            this.metrics = { kbs: bytes / (report.timestamp - this.sample.time), bpp, ratio: bpp > 0 ? 24 / bpp : null };
          } else {
            this.metrics = null;
          }
        }
        this.sample = { time: report.timestamp, frames: report.framesDecoded, bytes: report.bytesReceived };
        $(this.key + "-caption").hidden = !this.video.videoWidth;
        $(this.key + "-caption").innerHTML = this.video.videoWidth
          ? `<span class="resolution">${this.video.videoWidth} × ${this.video.videoHeight} · </span>${number(fps, 0)} FPS`
          : "";
      }
      // A new session may need to wait for its first keyframe. Also watch
      // connected peers without any inbound report, rather than waiting forever.
      if (performance.now() - this.lastFrameAt > (this.hasFrames ? 6000 : 15000)) {
        this.state("无信号");
        this.schedule();
      }
    } catch (_) {}
  }
}

function updateLive() {
  if (!config) return;
  let count = 0;
  const visible = [];
  for (const channel of config.channels) {
    const ready = kinds.filter(
      (kind) => players.get(keyFor(channel, kind))?.live,
    ).length;
    const card = $(channel.id + "-card");
    if (!card) continue;
    card.classList.toggle("live", ready === 3);
    $(channel.id + "-state").textContent =
      ready === 3
        ? "播放中"
        : ready > 0
          ? `${ready} / 3 已连接`
          : paused
            ? "已暂停"
            : "未连接";
    $(channel.id + "-nav").classList.toggle("live", ready > 0);
    if (ready) count++;
    card.hidden =
      (selected !== null && selected !== channel.id) ||
      (filter === "live" && !ready);
    if (!card.hidden) visible.push(card);
  }
  visible.forEach((card, index) => {
    card.classList.toggle("solo", visible.length === 1);
    card.classList.toggle(
      "wide",
      visible.length === 2 ||
        visible.length === 4 ||
        (visible.length === 5 && index >= 3),
    );
  });
  $("live-count").textContent = count;

  $("empty-filter").hidden = !(filter === "live" && count === 0);
}

function render() {
  for (const player of players.values()) {
    player.close();
    player.delay?.destroy();
  }
  players.clear();
  histories.clear();
  selected = null;
  $("channel-nav").innerHTML = "";
  $("channel-grid").innerHTML = "";
  for (const [index, channel] of config.channels.entries()) {
    $("channel-nav").insertAdjacentHTML(
      "beforeend",
      `<button class="channel-link" id="${channel.id}-nav" data-channel="${channel.id}"><span>${escapeHtml(channel.name)}</span><i></i></button>`,
    );
    const feeds = kinds
      .map((kind) => {
        const key = keyFor(channel, kind);
        return `<div class="feed" id="${key}"><header class="feed-heading"><span class="feed-label ${kind}">${labels[kind]}</span><span class="feed-caption" id="${key}-caption" hidden></span></header><div class="picture"><video id="${key}-video" autoplay muted playsinline aria-label="${escapeHtml(channel.name)}${labels[kind]}"></video><div class="video-empty"><span id="${key}-empty">无信号</span></div><button class="expand-button" data-expand="${key}" title="全屏" aria-label="全屏">${expand}</button></div><div class="feed-metrics"><div class="metric"><span class="metric-label">bpp</span><span class="metric-value" id="${key}-bpp">—</span></div><div class="metric"><span class="metric-label">带宽</span><span class="metric-value"><span id="${key}-kbs">—</span><small>kB/s</small></span></div><div class="metric"><span class="metric-label">压缩比</span><span class="metric-value"><span id="${key}-ratio">—</span><small>: 1</small></span></div></div></div>`;
      })
      .join("");
    $("channel-grid").insertAdjacentHTML(
      "beforeend",
      `<article class="channel-card" id="${channel.id}-card"><header class="card-heading"><h2>${escapeHtml(channel.name)}</h2><span class="pair-status"><i></i><span id="${channel.id}-state">未连接</span></span><button class="card-options" data-edit="${index}" aria-label="配置${escapeHtml(channel.name)}" title="通道参数">···</button></header><div class="pair">${feeds}</div><footer class="comparison-history"><span>带宽 <small>kB/s</small></span><svg viewBox="0 0 100 28" preserveAspectRatio="none" aria-label="H.264 与语义压缩带宽对比"><path class="h264-line" id="${channel.id}-h264-plot" d=""/><path class="semantic-line" id="${channel.id}-plot" d=""/></svg><span class="plot-legend"><i class="h264-swatch"></i>H.264<i class="semantic-swatch"></i>语义压缩</span></footer></article>`,
    );
    for (const kind of kinds)
      players.set(keyFor(channel, kind), new Player(channel, kind));
    histories.set(channel.id, []);
  }
  updateLive();
  if (!paused) for (const player of players.values()) player.connect();
}

async function refresh() {
  if (refreshing || !config) return;
  refreshing = true;
  try {
    const [status] = await Promise.all([
      api("/api/status"),
      ...Array.from(players.values(), (player) => player.stats()),
    ]);
    const now = performance.now();
    const byId = new Map(
      status.channels.map((channel) => [channel.id, channel.metrics]),
    );
    let total = 0,
      available = 0;
    for (const channel of config.channels) {
      const metrics = byId.get(channel.id);
      const h264 = players.get(keyFor(channel, "h264"));
      const h264Metrics = h264?.live ? h264.metrics : null;
      for (const kind of kinds) {
        const value = kind === "semantic" ? metrics : kind === "h264" ? h264Metrics : { bpp: 24, kbs: channel.width * channel.height * channel.fps * 3 / 1000, ratio: 1 };
        const key = keyFor(channel, kind);
        $(key + "-bpp").textContent = number(value?.bpp, 4);
        $(key + "-kbs").textContent = number(value?.kbs, 1);
        $(key + "-ratio").textContent = number(value?.ratio, kind === "original" ? 1 : 0);
      }
      const history = histories.get(channel.id);
      if (metrics) {
        total += metrics.kbs;
        available++;
      }
      if (metrics || h264Metrics) history.push({ time: now, semantic: metrics?.kbs, h264: h264Metrics?.kbs });
      while (history.length && history[0].time < now - 30000) history.shift();
      const peak = Math.max(1, ...history.flatMap((point) => [point.semantic || 0, point.h264 || 0]));
      for (const kind of ["semantic", "h264"]) {
        $(channel.id + (kind === "h264" ? "-h264-plot" : "-plot")).setAttribute(
        "d",
        history.filter(point => Number.isFinite(point[kind]))
          .map(
            (point, index) =>
              `${index ? "L" : "M"}${100 - (now - point.time) / 300},${25 - (point[kind] / peak) * 21}`,
          )
          .join(" "),
      );
      }
    }
    $("h264-error").textContent = status.h264?.error || "";
    $("h264-error").hidden = !status.h264?.error;
    $("bandwidth-total").textContent = available === 5 ? number(total, 1) : "—";
    $("bandwidth-total").title =
      available === 5
        ? "五路语义码流带宽合计"
        : `已收到 ${available} / 5 路编码统计`;
  } catch (_) {
    // A playback service outage is shown on each picture; keep controls usable.
  } finally {
    refreshing = false;
  }
}

function captureFields() {
  if (!draft) return;
  const channel = draft.channels[editIndex];
  for (const field of ["name", "original", "h264", "semantic"])
    channel[field] = $("field-" + field).value.trim();
  for (const field of ["width", "height", "fps", "stats_port"])
    channel[field] = Number($("field-" + field).value);
}

function channelFields() {
  const channel = draft.channels[editIndex];
  $("settings-tabs").innerHTML = draft.channels
    .map(
      (_, index) =>
        `<button type="button" class="${index === editIndex ? "active" : ""}" data-tab="${index}">${String(index + 1).padStart(2, "0")}</button>`,
    )
    .join("");
  $("channel-fields").innerHTML =
    '<label>通道名称<input id="field-name" maxlength="32" required></label><label>原图 RTSP<textarea id="field-original" rows="2" required spellcheck="false"></textarea></label><label>H.264 RTSP<textarea id="field-h264" rows="2" required spellcheck="false"></textarea></label><label>语义压缩 RTSP<textarea id="field-semantic" rows="2" required spellcheck="false"></textarea></label><div class="form-row"><label>宽度<input id="field-width" type="number" min="16" max="8192" required></label><label>高度<input id="field-height" type="number" min="16" max="8192" required></label><label title="仅作为编码源参数记录；板端帧率需在编码配置中修改">编码 FPS<input id="field-fps" type="number" min="1" max="240" step="any" required></label></div><label>编码统计 UDP 端口<input id="field-stats_port" type="number" min="1024" max="65535" required></label>';
  for (const field of [
    "name",
    "original",
    "h264",
    "semantic",
    "width",
    "height",
    "fps",
    "stats_port",
  ])
    $("field-" + field).value = channel[field];
}

function openSettings(index = 0) {
  if (!config) return;
  draft = structuredClone(config);
  editIndex = index;
  for (const field of ["webrtc", "media_api", "original_delay_ms", "h264_qp", "h264_delay_ms", "h264_ffmpeg", "h264_encoder"])
    $(field).value = draft[field];
  $("h264_auto_start").checked = draft.h264_auto_start;
  $("settings-error").hidden = true;
  channelFields();
  if (!$("settings-dialog").open) $("settings-dialog").showModal();
}

document.addEventListener("click", (event) => {
  const expandButton = event.target.closest("[data-expand]");
  if (expandButton) {
    const element = $(expandButton.dataset.expand);
    (document.fullscreenElement
      ? document.exitFullscreen()
      : element.requestFullscreen()
    ).catch(() => toast("无法进入全屏", true));
  }
  const settingsButton = event.target.closest("[data-settings],[data-edit]");
  if (settingsButton) openSettings(Number(settingsButton.dataset.edit || 0));
  const tab = event.target.closest("[data-tab]");
  if (tab) {
    captureFields();
    editIndex = Number(tab.dataset.tab);
    channelFields();
  }
  const nav = event.target.closest("[data-channel]");
  if (nav) {
    selected = selected === nav.dataset.channel ? null : nav.dataset.channel;
    filter = "all";
    for (const button of document.querySelectorAll("[data-filter]"))
      button.classList.toggle("active", button.dataset.filter === filter);
    for (const button of document.querySelectorAll("[data-channel]"))
      button.classList.toggle("selected", button.dataset.channel === selected);
    $("overview").classList.toggle("active", selected === null);
    updateLive();
  }
  const filterButton = event.target.closest("[data-filter]");
  if (filterButton) {
    filter = filterButton.dataset.filter;
    selected = null;
    $("overview").classList.add("active");
    for (const button of document.querySelectorAll("[data-channel]"))
      button.classList.remove("selected");
    for (const button of document.querySelectorAll("[data-filter]"))
      button.classList.toggle("active", button === filterButton);
    updateLive();
  }
});
$("overview").addEventListener("click", () => {
  filter = "all";
  for (const button of document.querySelectorAll("[data-filter]"))
    button.classList.toggle("active", button.dataset.filter === "all");
  selected = null;
  $("overview").classList.add("active");
  for (const nav of document.querySelectorAll("[data-channel]"))
    nav.classList.remove("selected");
  updateLive();
});
$("pause-button").addEventListener("click", () => {
  paused = !paused;
  $("pause-label").textContent = paused ? "继续预览" : "暂停预览";
  $("pause-icon").textContent = paused ? "▷" : "Ⅱ";
  for (const player of players.values())
    paused ? player.close() : player.connect();
});
$("reconnect-button").addEventListener("click", () => {
  if (paused) {
    toast("请先继续预览");
    return;
  }
  for (const player of players.values()) player.connect();
  toast("正在重新连接视频");
});
for (const id of ["close-settings", "cancel-settings"])
  $(id).addEventListener("click", () => $("settings-dialog").close());
$("settings-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  captureFields();
  draft.webrtc = $("webrtc").value.trim();
  draft.media_api = $("media_api").value.trim();
  draft.original_delay_ms = Number($("original_delay_ms").value);
  draft.h264_qp = Number($("h264_qp").value);
  draft.h264_delay_ms = Number($("h264_delay_ms").value);
  draft.h264_ffmpeg = $("h264_ffmpeg").value.trim();
  draft.h264_encoder = $("h264_encoder").value;
  draft.h264_auto_start = $("h264_auto_start").checked;
  $("save-settings").disabled = true;
  try {
    config = await api("/api/config", draft);
    paused = false;
    $("pause-label").textContent = "暂停预览";
    $("pause-icon").textContent = "Ⅱ";
    $("settings-dialog").close();
    render();
    await refresh();
    toast("参数已保存");
  } catch (error) {
    $("settings-error").textContent = error.message;
    $("settings-error").hidden = false;
  } finally {
    $("save-settings").disabled = false;
  }
});
window.addEventListener("pagehide", () => {
  for (const player of players.values()) player.close();
});
window.addEventListener("pageshow", (event) => {
  if (event.persisted && !paused)
    for (const player of players.values()) player.connect();
});
document.addEventListener("visibilitychange", () => {
  if (document.visibilityState !== "visible" || paused) return;
  for (const player of players.values()) {
    if (!player.peer || performance.now() - player.lastFrameAt > 15000)
      player.connect();
  }
  refresh();
});
for (const action of ["start", "stop"]) {
  $("h264-" + action).addEventListener("click", async () => {
    try {
      await api("/api/h264/" + action, {});
      toast(action === "start" ? "正在启动 H.264" : "H.264 已停止");
    } catch (error) { toast(error.message, true); }
  });
}
function clock() {
  $("clock").textContent = new Intl.DateTimeFormat("zh-CN", {
    year: "numeric",
    month: "2-digit",
    day: "2-digit",
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
    hour12: false,
  })
    .format(new Date())
    .replaceAll("/", ".");
}
clock();
setInterval(clock, 1000);
setInterval(refresh, 2000);
(async () => {
  try {
    config = await api("/api/config");
    render();
    await refresh();
  } catch (error) {
    toast("读取参数失败：" + error.message, true);
  }
})();
