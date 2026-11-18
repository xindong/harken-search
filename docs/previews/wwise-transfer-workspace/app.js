(() => {
  'use strict';

  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));

  const ctx = new (window.AudioContext || window.webkitAudioContext)();
  const state = {
    sources: new Map(),
    queue: [],
    selectedId: null,
    activeSourceId: null,
    playhead: 0,
    playing: false,
    container: 'random',
    undo: [],
    results: null,
    params: { thresholdDb: -42, minSilence: 0.22, padding: 0.02 },
  };

  let player = null;
  let rafId = 0;
  let seq = 0;

  const SAMPLE_META = [
    { file: 'AC_Footsteps_Concrete_01.wav', type: '脚步', short: '水泥脚步 01' },
    { file: 'METAL_Door_Impact_02.wav', type: '金属', short: '金属门撞击 02' },
    { file: 'UI_Click_Soft_03.wav', type: 'UI', short: '轻点击 03' },
    { file: 'AC_Footsteps_Quiet_Take_04.wav', type: '脚步', short: '低音量脚步 04' },
  ];

  const fmt = (t) => {
    const total = Math.max(0, t);
    const m = Math.floor(total / 60);
    const s = Math.floor(total % 60);
    const ms = Math.floor((total % 1) * 1000);
    return String(m).padStart(2, '0') + ':' + String(s).padStart(2, '0') + '.' + String(ms).padStart(3, '0');
  };
  const fmtShort = (t) => (t >= 10 ? t.toFixed(1) : t.toFixed(2)) + 's';
  const baseName = (name) => name.replace(/\.[^.]+$/, '');

  function decodeBase64(b64) {
    const bin = atob(b64);
    const bytes = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i += 1) bytes[i] = bin.charCodeAt(i);
    return bytes.buffer;
  }

  async function loadSamples() {
    const payload = window.HARKEN_PREVIEW_SAMPLES || {};
    for (const meta of SAMPLE_META) {
      const b64 = payload[meta.file];
      if (!b64) continue;
      const buffer = await ctx.decodeAudioData(decodeBase64(b64));
      state.sources.set(meta.file, {
        id: meta.file,
        file: meta.file,
        name: baseName(meta.file),
        short: meta.short,
        type: meta.type,
        buffer,
        duration: buffer.duration,
      });
    }
  }

  function renderResults() {
    const list = $('#results');
    list.innerHTML = '';
    for (const source of state.sources.values()) {
      const li = document.createElement('li');
      li.className = 'result-item';
      li.draggable = true;
      li.dataset.source = source.id;
      li.innerHTML =
        '<span class="type">' + source.type + '</span>' +
        '<span class="name" title="' + source.short + '">' + source.short + '</span>' +
        '<span class="dur">' + fmtShort(source.duration) + '</span>';
      li.addEventListener('dragstart', (event) => {
        event.dataTransfer.setData('text/plain', source.id);
        event.dataTransfer.effectAllowed = 'copy';
      });
      li.addEventListener('dblclick', () => addSource(source.id));
      list.appendChild(li);
    }
  }

  function snapshot() {
    state.undo.push(JSON.stringify(state.queue));
    if (state.undo.length > 40) state.undo.shift();
  }

  function undo() {
    const prev = state.undo.pop();
    if (!prev) return;
    state.queue = JSON.parse(prev);
    if (!state.queue.some((s) => s.id === state.selectedId)) state.selectedId = state.queue.length ? state.queue[0].id : null;
    syncActive();
    refresh();
  }

  function addSource(sourceId) {
    const source = state.sources.get(sourceId);
    if (!source) return;
    snapshot();
    seq += 1;
    const sameSource = state.queue.filter((s) => s.sourceId === sourceId).length;
    const segment = {
      id: 'seg-' + seq,
      sourceId,
      name: source.name + ' ' + String(sameSource + 1).padStart(2, '0'),
      start: 0,
      end: source.duration,
      included: true,
    };
    state.queue.push(segment);
    state.selectedId = segment.id;
    state.activeSourceId = sourceId;
    state.playhead = 0;
    state.results = null;
    refresh();
  }

  function selected() {
    return state.queue.find((s) => s.id === state.selectedId) || null;
  }

  function syncActive() {
    const seg = selected();
    if (seg) state.activeSourceId = seg.sourceId;
    if (state.activeSourceId) {
      const source = state.sources.get(state.activeSourceId);
      if (source) state.playhead = Math.min(state.playhead, source.duration);
    }
  }

  function selectSegment(id, seek) {
    const seg = state.queue.find((s) => s.id === id);
    if (!seg) return;
    state.selectedId = id;
    state.activeSourceId = seg.sourceId;
    if (seek) state.playhead = seg.start;
    refresh();
  }

  function analyzeSegments(source) {
    const data = source.buffer.getChannelData(0);
    const rate = source.buffer.sampleRate;
    const win = Math.max(1, Math.round(rate * 0.01));
    const frames = Math.ceil(data.length / win);
    const levels = new Float32Array(frames);
    for (let i = 0; i < frames; i += 1) {
      const from = i * win;
      const to = Math.min(from + win, data.length);
      let sum = 0;
      for (let j = from; j < to; j += 1) sum += data[j] * data[j];
      levels[i] = Math.sqrt(sum / Math.max(1, to - from));
    }
    const threshold = Math.pow(10, state.params.thresholdDb / 20);
    const minFrames = Math.max(1, Math.round(state.params.minSilence / 0.01));
    const runs = [];
    let runStart = -1;
    for (let i = 0; i < frames; i += 1) {
      if (levels[i] <= threshold) {
        if (runStart < 0) runStart = i;
      } else if (runStart >= 0) {
        runs.push([runStart, i]);
        runStart = -1;
      }
    }
    if (runStart >= 0) runs.push([runStart, frames]);
    const duration = source.duration;
    const pad = state.params.padding;
    const ranges = [];
    let cursor = 0;
    for (const [a, b] of runs) {
      if (b - a < minFrames) continue;
      const silenceStart = a * 0.01;
      const silenceEnd = b * 0.01;
      if (silenceStart < 0.03 || silenceEnd > duration - 0.03) continue;
      const prevEnd = Math.min(duration, silenceStart + pad);
      const nextStart = Math.max(prevEnd + 0.03, silenceEnd - pad);
      if (prevEnd - cursor >= 0.05) ranges.push([cursor, prevEnd]);
      cursor = nextStart;
    }
    if (duration - cursor >= 0.05) ranges.push([cursor, duration]);
    return ranges;
  }

  function splitSource(sourceId) {
    const source = state.sources.get(sourceId);
    if (!source) return { created: 0, reason: '找不到音效' };
    const ranges = analyzeSegments(source);
    if (ranges.length <= 1) {
      return { created: 0, reason: '没有找到符合当前灵敏度的静音间隔' };
    }
    snapshot();
    const firstIndex = state.queue.findIndex((s) => s.sourceId === sourceId);
    const keepNames = state.queue.filter((s) => s.sourceId === sourceId).map((s) => s.name);
    state.queue = state.queue.filter((s) => s.sourceId !== sourceId);
    const created = ranges.map((range, index) => {
      seq += 1;
      return {
        id: 'seg-' + seq,
        sourceId,
        name: keepNames[index] || source.name + ' ' + String(index + 1).padStart(2, '0'),
        start: range[0],
        end: range[1],
        included: true,
      };
    });
    const insertAt = firstIndex < 0 ? state.queue.length : firstIndex;
    state.queue.splice(insertAt, 0, ...created);
    state.selectedId = created[0].id;
    state.activeSourceId = sourceId;
    state.playhead = created[0].start;
    state.results = null;
    refresh();
    return { created: created.length, reason: '' };
  }

  function splitAtPlayhead() {
    const source = state.sources.get(state.activeSourceId);
    const seg = state.queue.find(
      (s) => s.sourceId === state.activeSourceId && state.playhead > s.start + 0.03 && state.playhead < s.end - 0.03
    );
    if (!source || !seg) return '把播放头放在片段内部，再切开';
    snapshot();
    const cut = state.playhead;
    const right = {
      id: 'seg-' + (++seq),
      sourceId: seg.sourceId,
      name: seg.name + ' B',
      start: cut,
      end: seg.end,
      included: seg.included,
    };
    seg.end = cut;
    seg.name = seg.name.replace(/ B$/, '') + ' A';
    const index = state.queue.indexOf(seg);
    state.queue.splice(index + 1, 0, right);
    state.selectedId = right.id;
    refresh();
    return '已在播放头切开';
  }

  function mergeNext() {
    const seg = selected();
    if (!seg) return '先选一个片段';
    const index = state.queue.indexOf(seg);
    const next = state.queue[index + 1];
    if (!next || next.sourceId !== seg.sourceId || Math.abs(next.start - seg.end) > 0.01) {
      return '下一段不是同一条录音的相邻片段';
    }
    snapshot();
    seg.end = next.end;
    seg.name = seg.name.replace(/ A$/, '');
    state.queue.splice(index + 1, 1);
    refresh();
    return '已合并相邻片段';
  }

  function removeSelected() {
    const seg = selected();
    if (!seg) return '先选一个片段';
    snapshot();
    const index = state.queue.indexOf(seg);
    state.queue.splice(index, 1);
    const next = state.queue[Math.min(index, state.queue.length - 1)];
    state.selectedId = next ? next.id : null;
    state.activeSourceId = next ? next.sourceId : null;
    refresh();
    return '已移出队列';
  }

  function toggleInclude(id) {
    const seg = state.queue.find((s) => s.id === id);
    if (!seg) return;
    seg.included = !seg.included;
    refresh();
  }

  function play() {
    const source = state.sources.get(state.activeSourceId);
    if (!source) return;
    stop();
    const seg = selected();
    let startAt = state.playhead;
    let endAt = source.duration;
    if (seg && seg.sourceId === source.id && (state.playhead < seg.start || state.playhead >= seg.end)) {
      startAt = seg.start;
    }
    if (seg && seg.sourceId === source.id && startAt >= seg.start && startAt < seg.end) {
      endAt = seg.end;
    }
    if (ctx.state === 'suspended') ctx.resume();
    const node = ctx.createBufferSource();
    node.buffer = source.buffer;
    node.connect(ctx.destination);
    node.start(0, startAt, Math.max(0.02, endAt - startAt));
    const startedAt = ctx.currentTime;
    player = { node, startedAt, startAt, endAt };
    state.playing = true;
    state.playhead = startAt;
    node.onended = () => {
      if (!player || player.node !== node) return;
      state.playing = false;
      state.playhead = endAt;
      player = null;
      renderTransport();
      drawWave();
    };
    renderTransport();
    tick();
  }

  function stop() {
    if (player) {
      const node = player.node;
      player = null;
      try { node.onended = null; node.stop(); } catch (_) { /* already stopped */ }
    }
    state.playing = false;
    cancelAnimationFrame(rafId);
    renderTransport();
    drawWave();
  }

  function tick() {
    if (!state.playing || !player) return;
    const elapsed = ctx.currentTime - player.startedAt;
    state.playhead = Math.min(player.endAt, player.startAt + elapsed);
    renderTransport();
    drawWave();
    if (state.playhead >= player.endAt) return;
    rafId = requestAnimationFrame(tick);
  }

  function renderQueue() {
    const list = $('#queue');
    const count = state.queue.length;
    $('#queue-count').textContent = count + ' 段';
    if (!count) {
      list.innerHTML = '<li class="empty">把左侧音效拖进来，或双击示例音效</li>';
      return;
    }
    list.innerHTML = '';
    let lastSource = null;
    for (const seg of state.queue) {
      if (seg.sourceId !== lastSource) {
        lastSource = seg.sourceId;
        const source = state.sources.get(seg.sourceId);
        const total = state.queue.filter((s) => s.sourceId === seg.sourceId).length;
        const title = document.createElement('li');
        title.className = 'queue-group-title';
        title.innerHTML = '<b>' + (source ? source.name : seg.sourceId) + '</b><span>' + total + ' 段</span>';
        list.appendChild(title);
      }
      const index = state.queue.filter((s) => s.sourceId === seg.sourceId).indexOf(seg) + 1;
      const li = document.createElement('li');
      li.className = 'seg-item' + (seg.id === state.selectedId ? ' active' : '') + (seg.included ? '' : ' excluded');
      li.dataset.id = seg.id;
      li.innerHTML =
        '<span class="idx">' + index + '</span>' +
        '<input class="nm" value="' + seg.name.replace(/"/g, '&quot;') + '" spellcheck="false">' +
        '<span class="len">' + fmtShort(seg.end - seg.start) + '</span>' +
        '<button class="eye" title="' + (seg.included ? '排除本段' : '恢复本段') + '">' + (seg.included ? '●' : '○') + '</button>';
      li.addEventListener('click', (event) => {
        if (event.target.classList.contains('nm') || event.target.classList.contains('eye')) return;
        selectSegment(seg.id, true);
      });
      const input = $('.nm', li);
      input.addEventListener('change', () => {
        seg.name = input.value.trim() || seg.name;
        refresh();
      });
      input.addEventListener('keydown', (event) => {
        if (event.key === 'Enter') input.blur();
      });
      $('.eye', li).addEventListener('click', (event) => {
        event.stopPropagation();
        toggleInclude(seg.id);
      });
      list.appendChild(li);
    }
  }

  function renderStrip() {
    const strip = $('#segment-strip');
    strip.innerHTML = '';
    const source = state.sources.get(state.activeSourceId);
    if (!source) {
      strip.innerHTML = '<div class="strip-empty"></div>';
      return;
    }
    const segs = state.queue.filter((s) => s.sourceId === source.id);
    if (!segs.length) {
      strip.innerHTML = '<div class="strip-empty"></div>';
      return;
    }
    const total = segs.reduce((sum, s) => sum + (s.end - s.start), 0);
    let index = 0;
    for (const seg of segs) {
      index += 1;
      const cell = document.createElement('div');
      cell.className = 'strip-cell' + (seg.included ? '' : ' excluded') + (seg.id === state.selectedId ? ' active' : '');
      cell.style.flex = String(Math.max(0.06, seg.end - seg.start)) + ' 1 0';
      cell.textContent = index;
      cell.title = seg.name + ' · ' + fmtShort(seg.end - seg.start);
      cell.addEventListener('click', () => selectSegment(seg.id, true));
      strip.appendChild(cell);
    }
    strip.title = '共 ' + fmtShort(total);
  }

  function renderTransport() {
    $('#time-head').textContent = fmt(state.playhead);
    const source = state.sources.get(state.activeSourceId);
    $('#time-total').textContent = fmt(source ? source.duration : 0);
    const seg = selected();
    $('#active-name').textContent = seg ? seg.name + ' · ' + fmtShort(seg.end - seg.start) : '未选择片段';
    $('#btn-play').textContent = state.playing ? '■ 停止' : '▶ 试听';
    $('#btn-split-head').disabled = !source;
    $('#btn-merge').disabled = !seg;
    $('#btn-remove').disabled = !seg;
    $('#btn-undo').disabled = state.undo.length === 0;
    $('#btn-split-silence').disabled = !source;
  }

  function renderTarget() {
    const included = state.queue.filter((s) => s.included);
    const duration = included.reduce((sum, s) => sum + (s.end - s.start), 0);
    const containers = new Set(included.map((s) => s.sourceId)).size;
    $('#sum-segments').textContent = String(included.length);
    $('#sum-duration').textContent = duration.toFixed(2) + 's';
    $('#sum-containers').textContent = String(containers);
    $('#btn-send').textContent = '发送 ' + included.length + ' 段';
    $('#btn-send').disabled = included.length === 0;
    $('#t-project').textContent = $('#project').value;
    const parent = $$('.target-panel select')[0].value;
    const leaf = $$('.target-panel select')[1].value;
    $('#t-path').textContent = 'Actor-Mixer/' + parent.replace('Actor-Mixer/', '') + '/' + leaf;
    $('#container-note').textContent =
      state.container === 'random'
        ? '同一条源录音的片段进同一个容器，随机播放'
        : '序列容器：按原录音时间顺序播放，可在发送前调整';
    for (const btn of $$('#container-seg .seg-btn')) {
      btn.classList.toggle('active', btn.dataset.kind === state.container);
    }
    renderSendResult();
  }

  function renderSendResult() {
    const box = $('#send-result');
    if (!state.results) {
      box.classList.add('hidden');
      box.innerHTML = '';
      return;
    }
    const ok = state.results.filter((r) => r.status === 'Imported').length;
    const skipped = state.results.length - ok;
    box.classList.remove('hidden');
    box.innerHTML =
      '<div class="head">预览模拟结果 · 已导入 ' + ok + ' 段' + (skipped ? ' · 跳过 ' + skipped + ' 段（重名不覆盖）' : '') + '</div>' +
      '<ul>' +
      state.results
        .map(
          (r) =>
            '<li class="' + (r.status === 'Imported' ? '' : 'skip') + '"><span>' + r.name +
            '</span><span class="p" title="' + r.path + '">' + r.path + '</span><span class="st">' + r.status + '</span></li>'
        )
        .join('') +
      '</ul>';
  }

  function drawWave() {
    const canvas = $('#wave');
    const wrap = canvas.parentElement;
    const dpr = window.devicePixelRatio || 1;
    const width = Math.max(320, Math.round(wrap.clientWidth));
    const height = 260;
    if (canvas.width !== Math.round(width * dpr) || canvas.height !== Math.round(height * dpr)) {
      canvas.width = Math.round(width * dpr);
      canvas.height = Math.round(height * dpr);
    }
    const g = canvas.getContext('2d');
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, width, height);

    const source = state.sources.get(state.activeSourceId);
    const empty = $('#wave-empty');
    if (!source) {
      g.fillStyle = '#12151a';
      g.fillRect(0, 0, width, height);
      empty.style.display = 'flex';
      return;
    }
    empty.style.display = 'none';

    g.fillStyle = '#12151a';
    g.fillRect(0, 0, width, height);

    const duration = source.duration;
    const xOf = (t) => (t / duration) * width;
    const mid = height / 2;
    const amp = height / 2 - 16;

    const segs = state.queue.filter((s) => s.sourceId === source.id);
    segs.forEach((seg, index) => {
      if (!seg.included) {
        g.fillStyle = 'rgba(20,23,28,.62)';
        g.fillRect(xOf(seg.start), 0, Math.max(1, xOf(seg.end) - xOf(seg.start)), height);
      } else {
        g.fillStyle = index % 2 === 0 ? 'rgba(111,208,140,.045)' : 'rgba(127,168,216,.045)';
        g.fillRect(xOf(seg.start), 0, Math.max(1, xOf(seg.end) - xOf(seg.start)), height);
      }
    });

    g.strokeStyle = '#242a33';
    g.lineWidth = 1;
    g.beginPath();
    g.moveTo(0, mid);
    g.lineTo(width, mid);
    g.stroke();

    const data = source.buffer.getChannelData(0);
    const step = data.length / width;
    g.fillStyle = '#69809a';
    for (let x = 0; x < width; x += 1) {
      const from = Math.floor(x * step);
      const to = Math.min(data.length, Math.floor((x + 1) * step));
      let min = 1;
      let max = -1;
      for (let i = from; i < to; i += 1) {
        const v = data[i];
        if (v < min) min = v;
        if (v > max) max = v;
      }
      if (min > max) continue;
      const y1 = mid - max * amp;
      const y2 = mid - min * amp;
      g.fillRect(x, y1, 1, Math.max(1, y2 - y1));
    }

    for (const seg of segs) {
      const x = xOf(seg.start);
      g.strokeStyle = seg.included ? '#6fd08c' : '#5a6472';
      g.lineWidth = 1;
      g.beginPath();
      g.moveTo(x + 0.5, 18);
      g.lineTo(x + 0.5, height);
      g.stroke();
      g.fillStyle = seg.included ? '#6fd08c' : '#5a6472';
      g.fillRect(x, 18, 3, 6);
    }

    const px = xOf(state.playhead);
    g.strokeStyle = '#ffb84d';
    g.lineWidth = 1.5;
    g.beginPath();
    g.moveTo(px, 10);
    g.lineTo(px, height);
    g.stroke();
    g.fillStyle = '#ffb84d';
    g.beginPath();
    g.moveTo(px - 5, 10);
    g.lineTo(px + 5, 10);
    g.lineTo(px, 18);
    g.closePath();
    g.fill();

    g.fillStyle = '#6f7a89';
    g.font = '10px "Segoe UI", sans-serif';
    const tickCount = Math.min(12, Math.max(3, Math.floor(duration)));
    for (let i = 0; i <= tickCount; i += 1) {
      const t = (duration * i) / tickCount;
      const x = xOf(t);
      g.fillRect(x, height - 12, 1, 6);
      g.fillText(t.toFixed(2) + 's', Math.min(width - 34, Math.max(2, x + 3)), height - 15);
    }
  }

  let toastTimer = 0;
  function toast(message) {
    const tip = $('.tip');
    tip.textContent = message;
    tip.style.color = '#b8f0c9';
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => {
      tip.textContent = '空格 播放/停止 · ←/→ 移动播放头 · 点波形定位 · 片段可改名后直接发送';
      tip.style.color = '';
    }, 2600);
  }

  function refresh() {
    renderQueue();
    renderStrip();
    renderTransport();
    renderTarget();
    drawWave();
  }

  function bind() {
    $('#btn-split-silence').addEventListener('click', () => {
      const sourceId = state.activeSourceId;
      if (!sourceId) return toast('先把一条音效加进队列');
      const result = splitSource(sourceId);
      toast(result.created ? '按静音分成 ' + result.created + ' 段，边界可再拖或合并' : result.reason);
    });
    $('#btn-split-head').addEventListener('click', () => toast(splitAtPlayhead()));
    $('#btn-merge').addEventListener('click', () => toast(mergeNext()));
    $('#btn-remove').addEventListener('click', () => toast(removeSelected()));
    $('#btn-undo').addEventListener('click', () => undo());
    $('#btn-params').addEventListener('click', (event) => {
      const panel = $('#params');
      const hidden = panel.classList.toggle('hidden');
      event.currentTarget.setAttribute('aria-expanded', String(!hidden));
    });
    $('#btn-play').addEventListener('click', () => (state.playing ? stop() : play()));

    $('#p-threshold').addEventListener('input', (event) => {
      state.params.thresholdDb = Number(event.target.value);
      $('#p-threshold-val').textContent = event.target.value + ' dB';
    });
    $('#p-silence').addEventListener('input', (event) => {
      state.params.minSilence = Number(event.target.value);
      $('#p-silence-val').textContent = Math.round(state.params.minSilence * 1000) + ' ms';
    });
    $('#p-padding').addEventListener('input', (event) => {
      state.params.padding = Number(event.target.value);
      $('#p-padding-val').textContent = Math.round(state.params.padding * 1000) + ' ms';
    });

    for (const btn of $$('#container-seg .seg-btn')) {
      btn.addEventListener('click', () => {
        state.container = btn.dataset.kind;
        renderTarget();
      });
    }

    $('#project').addEventListener('change', renderTarget);
    for (const select of $$('.target-panel select')) select.addEventListener('change', renderTarget);

    $('#btn-send').addEventListener('click', () => {
      const included = state.queue.filter((s) => s.included);
      if (!included.length) return;
      const results = [];
      const seen = new Set();
      const counters = new Map();
      for (const seg of included) {
        const source = state.sources.get(seg.sourceId);
        const key = seg.sourceId;
        counters.set(key, (counters.get(key) || 0) + 1);
        const leaf = $$('.target-panel select')[1].value;
        const parent = $$('.target-panel select')[0].value.replace('Actor-Mixer/', '');
        const path = 'Actor-Mixer/' + parent + '/' + leaf + '/' + seg.name;
        let status = 'Imported';
        if (seen.has(path)) status = 'Skipped';
        seen.add(path);
        results.push({ name: seg.name, path, status, duration: seg.end - seg.start });
      }
      state.results = results;
      renderSendResult();
      toast('预览模拟：不会写入 Wwise');
    });

    const canvas = $('#wave');
    const scrub = (event) => {
      const source = state.sources.get(state.activeSourceId);
      if (!source) return;
      const rect = canvas.getBoundingClientRect();
      const ratio = Math.min(1, Math.max(0, (event.clientX - rect.left) / rect.width));
      state.playhead = ratio * source.duration;
      if (state.playing) play();
      renderTransport();
      drawWave();
    };
    let scrubbing = false;
    canvas.addEventListener('pointerdown', (event) => {
      scrubbing = true;
      canvas.setPointerCapture(event.pointerId);
      scrub(event);
    });
    canvas.addEventListener('pointermove', (event) => {
      if (scrubbing) scrub(event);
    });
    canvas.addEventListener('pointerup', () => {
      scrubbing = false;
    });

    const wrap = canvas.parentElement;
    wrap.addEventListener('dragover', (event) => {
      event.preventDefault();
      wrap.classList.add('drop');
    });
    wrap.addEventListener('dragleave', () => wrap.classList.remove('drop'));
    wrap.addEventListener('drop', (event) => {
      event.preventDefault();
      wrap.classList.remove('drop');
      const sourceId = event.dataTransfer.getData('text/plain');
      if (sourceId) addSource(sourceId);
    });

    window.addEventListener('keydown', (event) => {
      const tag = (event.target.tagName || '').toLowerCase();
      if (tag === 'input' || tag === 'select') return;
      const source = state.sources.get(state.activeSourceId);
      if (event.code === 'Space') {
        event.preventDefault();
        if (state.playing) stop(); else play();
      } else if (event.key === 'ArrowLeft' && source) {
        state.playhead = Math.max(0, state.playhead - 0.02);
        renderTransport();
        drawWave();
      } else if (event.key === 'ArrowRight' && source) {
        state.playhead = Math.min(source.duration, state.playhead + 0.02);
        renderTransport();
        drawWave();
      }
    });

    window.addEventListener('resize', drawWave);
  }

  function handleDemoQuery() {
    const query = new URLSearchParams(location.search);
    if (!query.has('demo')) return;
    seedDemo();
    refresh();
    if (query.has('sent')) document.querySelector('#btn-send').click();
    if (query.has('sensitivity')) document.querySelector('#btn-params').click();
  }
  function seedDemo() {
    const first = SAMPLE_META[0].file;
    const second = SAMPLE_META[1].file;
    if (!state.sources.has(first)) return;
    addSource(first);
    splitSource(first);
    if (state.sources.has(second)) addSource(second);
    if (state.queue.length) selectSegment(state.queue[0].id, true);
  }

  (async () => {
    await loadSamples();
    renderResults();
    bind();
    handleDemoQuery();
    refresh();
  })().catch((error) => {
    const tip = $('.tip');
    if (tip) tip.textContent = '示例音效加载失败：' + error.message;
    console.error(error);
  });
})();
