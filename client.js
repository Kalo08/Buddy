/* ─── Buddy Hub — client.js ─────────────────────────────────────────────────
   Modules: connection · camera · audio · controls
   Protocol:
     Text (JSON) — commands, status events
     Binary      — 0x01 prefix = JPEG frame, 0x02 prefix = PCM16 audio chunk
────────────────────────────────────────────────────────────────────────────── */

'use strict';

// ── DOM refs ──────────────────────────────────────────────────────────────────

const $  = id => document.getElementById(id);
const el = {
  input:          $('buddy-id-input'),
  connectBtn:     $('connect-btn'),
  connectHint:    $('connect-hint'),
  hubLive:        $('hub-live'),
  liveDot:        $('live-dot'),
  hubBuddyId:     $('hub-buddy-id'),
  cameraArea:     $('camera-area'),
  cameraOffline:  $('camera-offline'),
  camCanvas:      $('cam-canvas'),
  micToggle:      $('mic-toggle'),
  speakerToggle:  $('speaker-toggle'),
  speakBtn:       $('speak-btn'),
  disconnectBtn:  $('disconnect-btn'),
};

// ── State ─────────────────────────────────────────────────────────────────────

let ws            = null;
let connected     = false;
let audioCtx      = null;
let micStream     = null;
let mediaRecorder = null;
let speakerMuted  = false;
let micMuted      = false;
const heldDirs    = new Set();  // directions currently held (keys + buttons)

// ── Helpers ───────────────────────────────────────────────────────────────────

function setHint(text, isError = false) {
  el.connectHint.textContent = text;
  el.connectHint.style.color = isError
    ? 'var(--accent)'
    : 'var(--text-dim)';
}

function setLiveState(state) {
  // state: 'offline' | 'connecting' | 'waiting' | 'connected' | 'lost'
  const dot  = el.liveDot;
  const live = el.hubLive;

  dot.classList.remove('active');
  live.style.color = '';

  switch (state) {
    case 'connected':
      live.childNodes[live.childNodes.length - 1].textContent = ' connected';
      dot.classList.add('active');
      live.style.color = 'var(--live)';
      break;
    case 'connecting':
      live.childNodes[live.childNodes.length - 1].textContent = ' connecting...';
      live.style.color = 'var(--text-mid)';
      break;
    case 'waiting':
      live.childNodes[live.childNodes.length - 1].textContent = ' waiting for device...';
      live.style.color = 'var(--text-mid)';
      break;
    case 'lost':
      live.childNodes[live.childNodes.length - 1].textContent = ' lost connection';
      live.style.color = 'var(--accent)';
      break;
    default:
      live.childNodes[live.childNodes.length - 1].textContent = ' not connected';
      live.style.color = 'var(--text-dim)';
  }
}

function resetUI() {
  connected = false;
  setLiveState('offline');
  el.cameraOffline.style.display = '';
  el.camCanvas.style.display = 'none';
  el.connectBtn.disabled = false;
  el.connectBtn.querySelector('span').textContent = 'Establish connection';
}

// ── Module: Connection ────────────────────────────────────────────────────────

function connect() {
  const raw = el.input.value.trim().toUpperCase().replace(/^BDY-/, '');
  if (!raw) {
    setHint('enter a buddy ID first.', true);
    return;
  }

  const id = `BDY-${raw}`;

  if (ws) {
    ws.close();
    ws = null;
  }

  setHint('opening connection...');
  setLiveState('connecting');
  el.connectBtn.disabled = true;
  el.connectBtn.querySelector('span').textContent = 'connecting...';

  const wsProto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  ws = new WebSocket(`${wsProto}//${location.host}/ws?id=${id}&role=client`);
  ws.binaryType = 'arraybuffer';

  ws.onopen = () => {
    setHint('connected to server — waiting for buddy...');
  };

  ws.onmessage = evt => {
    if (typeof evt.data === 'string') {
      handleTextMessage(JSON.parse(evt.data), id);
    } else {
      handleBinaryMessage(evt.data);
    }
  };

  ws.onclose = () => {
    if (connected) {
      setLiveState('lost');
      setHint('connection closed. re-enter ID to reconnect.', true);
    }
    stopMic();
    resetUI();
  };

  ws.onerror = () => {
    setHint('could not reach server.', true);
    resetUI();
  };
}

function handleTextMessage(msg, id) {
  switch (msg.type) {

    case 'paired':
      connected = true;
      el.hubBuddyId.textContent = msg.id || id;
      setLiveState('connected');
      setHint('buddy connected. controls are live.');
      el.connectBtn.disabled = false;
      el.connectBtn.querySelector('span').textContent = 'Establish connection';
      document.getElementById('how-it-works').scrollIntoView({ behavior: 'smooth' });
      showCamera();
      break;

    case 'waiting':
      setLiveState('waiting');
      setHint(msg.msg || 'buddy not online — waiting...');
      break;

    case 'buddy_disconnected':
      connected = false;
      setLiveState('lost');
      setHint('buddy went offline.', true);
      hideCamera();
      stopMic();
      break;

    default:
      break;
  }
}

el.connectBtn.addEventListener('click', connect);

el.disconnectBtn.addEventListener('click', () => {
  if (!ws) {
    setHint('not connected.');
    return;
  }
  // Stop anything in flight, then close cleanly. Setting connected=false
  // first keeps the onclose handler from reporting it as a lost connection.
  releaseDpad();
  stopMic();
  connected = false;
  ws.close();
  ws = null;
  resetUI();
  setHint('disconnected from buddy.');
});

el.input.addEventListener('keydown', e => {
  if (e.key === 'Enter') connect();
});

// ── Module: Camera ────────────────────────────────────────────────────────────

function showCamera() {
  el.cameraOffline.style.display = 'none';
  el.camCanvas.style.display = 'block';
}

function hideCamera() {
  el.cameraOffline.style.display = '';
  el.camCanvas.style.display = 'none';
}

// Camera rendering: decode incoming JPEG into an ImageBitmap, then draw it on
// the next animation frame.  Keeping only the latest pending bitmap means the
// display never falls further behind than one frame — stale bitmaps are closed
// immediately rather than queued up, which was the main source of visible lag.

let _pendingBitmap = null;
let _rafPending    = false;

function _rafDraw() {
  _rafPending = false;
  const bitmap = _pendingBitmap;
  _pendingBitmap = null;
  if (!bitmap) return;

  const canvas = el.camCanvas;
  const ctx    = canvas.getContext('2d');

  if (canvas.width !== bitmap.width || canvas.height !== bitmap.height) {
    canvas.width  = bitmap.width;
    canvas.height = bitmap.height;
  }

  ctx.drawImage(bitmap, 0, 0);
  bitmap.close();
}

function renderFrame(arrayBuffer) {
  const jpegBytes = arrayBuffer.slice(1);
  const blob = new Blob([jpegBytes], { type: 'image/jpeg' });

  createImageBitmap(blob).then(bitmap => {
    // Drop previous pending bitmap so we always show the freshest frame
    if (_pendingBitmap) {
      _pendingBitmap.close();
    }
    _pendingBitmap = bitmap;

    if (!_rafPending) {
      _rafPending = true;
      requestAnimationFrame(_rafDraw);
    }
  }).catch(() => {
    // Silently drop malformed frames
  });
}

// ── Module: Audio ─────────────────────────────────────────────────────────────

function getAudioCtx() {
  if (!audioCtx) audioCtx = new (window.AudioContext || window.webkitAudioContext)();
  if (audioCtx.state === 'suspended') audioCtx.resume();
  return audioCtx;
}

function playPCM16(arrayBuffer) {
  if (speakerMuted) return;

  // Strip the 0x02 type byte
  const raw    = arrayBuffer.slice(1);
  const pcm16  = new Int16Array(raw);
  const float32 = new Float32Array(pcm16.length);
  for (let i = 0; i < pcm16.length; i++) float32[i] = pcm16[i] / 32768;

  const ctx    = getAudioCtx();
  const buffer = ctx.createBuffer(1, float32.length, 16000);
  buffer.copyToChannel(float32, 0);

  const source = ctx.createBufferSource();
  source.buffer = buffer;
  source.connect(ctx.destination);
  source.start();
}

// Speak button — hold to talk

el.speakBtn.addEventListener('mousedown',   startSpeaking);
el.speakBtn.addEventListener('touchstart',  e => { e.preventDefault(); startSpeaking(); });
el.speakBtn.addEventListener('mouseup',     stopSpeaking);
el.speakBtn.addEventListener('mouseleave',  stopSpeaking);
el.speakBtn.addEventListener('touchend',    stopSpeaking);
el.speakBtn.addEventListener('touchcancel', stopSpeaking);

let speakSession = 0;
let sentBytes    = 0;

async function startSpeaking() {
  if (!connected) return;
  if (micMuted) {
    setHint('mic is muted — tap the MIC toggle first.', true);
    return;
  }
  if (mediaRecorder) return; // already recording (e.g. ghost mousedown after touchstart)

  const session = ++speakSession;
  el.speakBtn.classList.add('speaking');
  sentBytes = 0;

  try {
    const stream = await navigator.mediaDevices.getUserMedia({ audio: true, video: false });

    // Button released (or pressed again) while the permission prompt was up —
    // don't start a recorder nobody will ever stop.
    if (session !== speakSession || !el.speakBtn.classList.contains('speaking')) {
      stream.getTracks().forEach(t => t.stop());
      return;
    }

    micStream = stream;
    mediaRecorder = new MediaRecorder(micStream);

    mediaRecorder.ondataavailable = async e => {
      if (!ws || ws.readyState !== WebSocket.OPEN) return;
      if (e.data.size === 0) return;

      // Prepend 0x02 type byte and send as binary
      const raw    = await e.data.arrayBuffer();
      const typed  = new Uint8Array(raw.byteLength + 1);
      typed[0]     = 0x02;
      typed.set(new Uint8Array(raw), 1);
      ws.send(typed.buffer);

      // Live meter: real voice sends multiple KB/s; a dead or muted mic
      // sends ~0.1 KB chunks. Lets you diagnose without server logs.
      sentBytes += raw.byteLength;
      setHint(`sending audio... ${(sentBytes / 1024).toFixed(1)} KB`);
    };

    mediaRecorder.onerror = e => {
      setHint(`mic recorder error: ${(e.error && e.error.name) || 'unknown'}`, true);
    };

    mediaRecorder.start(250); // 250ms chunks
  } catch (err) {
    setHint(`mic access failed: ${err.name || err}`, true);
    el.speakBtn.classList.remove('speaking');
  }
}

function stopSpeaking() {
  el.speakBtn.classList.remove('speaking');
  stopMic();
}

function stopMic() {
  if (mediaRecorder && mediaRecorder.state !== 'inactive') {
    mediaRecorder.stop();
  }
  if (micStream) {
    micStream.getTracks().forEach(t => t.stop());
    micStream = null;
  }
  mediaRecorder = null;
}

// Audio toggle handlers

el.micToggle.addEventListener('click', () => {
  micMuted = !micMuted;
  el.micToggle.classList.toggle('on', !micMuted);
  if (micMuted) stopMic();
});

el.speakerToggle.addEventListener('click', () => {
  speakerMuted = !speakerMuted;
  el.speakerToggle.classList.toggle('on', !speakerMuted);
});

// ── Module: Controls ──────────────────────────────────────────────────────────

const KEY_MAP = {
  ArrowUp:    'fwd',
  ArrowDown:  'back',
  ArrowLeft:  'left',
  ArrowRight: 'right',
  w: 'fwd', W: 'fwd',
  s: 'back', S: 'back',
  a: 'left', A: 'left',
  d: 'right', D: 'right',
};

function sendCmd(dir) {
  if (!ws || ws.readyState !== WebSocket.OPEN) return;
  ws.send(JSON.stringify({ type: 'cmd', dir }));
}

// Held directions combine into one drive vector, so W+A/W+D steers while
// driving. Opposite keys cancel out. All released → plain "stop" (the old
// message, so an un-updated device still stops).
function sendDrive() {
  if (!ws || ws.readyState !== WebSocket.OPEN) return;
  const vx = (heldDirs.has('fwd')   ? 1 : 0) - (heldDirs.has('back') ? 1 : 0);
  const vy = (heldDirs.has('right') ? 1 : 0) - (heldDirs.has('left') ? 1 : 0);
  if (vx === 0 && vy === 0) sendCmd('stop');
  else ws.send(JSON.stringify({ type: 'vec', vx, vy }));
}

function updateDpadUI() {
  document.querySelectorAll('.d-btn[data-dir]').forEach(btn => {
    btn.classList.toggle('d-btn-active', heldDirs.has(btn.dataset.dir));
  });
}

function pressDpad(dir) {
  if (heldDirs.has(dir)) return;
  heldDirs.add(dir);
  sendDrive();
  updateDpadUI();
}

// Call with a direction to release just that one (key/button up), or with
// no argument to release everything (safety stop).
function releaseDpad(dir) {
  if (dir === undefined) {
    if (!heldDirs.size) return;
    heldDirs.clear();
  } else {
    if (!heldDirs.delete(dir)) return;
  }
  sendDrive();
  updateDpadUI();
}

// D-pad button events
// touchstart/touchend call preventDefault() so the browser doesn't also fire
// synthetic mousedown/mouseup ~300ms after the touch — otherwise that ghost
// mousedown re-triggers pressDpad right after release sent "stop".
document.querySelectorAll('.d-btn[data-dir]').forEach(btn => {
  const dir = btn.dataset.dir;
  btn.addEventListener('mousedown',   () => pressDpad(dir));
  btn.addEventListener('touchstart',  e => { e.preventDefault(); pressDpad(dir); });
  btn.addEventListener('mouseup',     () => releaseDpad(dir));
  btn.addEventListener('mouseleave',  () => releaseDpad(dir));
  btn.addEventListener('touchend',    e => { e.preventDefault(); releaseDpad(dir); });
  btn.addEventListener('touchcancel', e => { e.preventDefault(); releaseDpad(dir); });
});

// Keyboard support
document.addEventListener('keydown', e => {
  // Don't capture when typing in the input
  if (document.activeElement === el.input) return;
  const dir = KEY_MAP[e.key];
  if (dir) { e.preventDefault(); pressDpad(dir); }
});

document.addEventListener('keyup', e => {
  const dir = KEY_MAP[e.key];
  if (dir) releaseDpad(dir);
});

// If the tab loses focus mid-drive, keyup never arrives — stop everything.
window.addEventListener('blur', () => releaseDpad());

// ── Binary message router ─────────────────────────────────────────────────────

function handleBinaryMessage(arrayBuffer) {
  const view = new DataView(arrayBuffer);
  const type = view.getUint8(0);

  if (type === 0x01) renderFrame(arrayBuffer);
  else if (type === 0x02) playPCM16(arrayBuffer);
}

// ── Init ──────────────────────────────────────────────────────────────────────

setLiveState('offline');
el.camCanvas.style.display = 'none';
el.micToggle.classList.add('on');    // mic on by default
