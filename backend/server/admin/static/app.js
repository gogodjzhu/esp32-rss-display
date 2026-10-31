'use strict';

// 管理页前端：无构建链、无 CDN 依赖，只用一个文件。
// 数据全部来自后端接口：GET /v1/feeds 拿配置，GET /admin/status 拿健康度，
// 两个接口按 feed id 在前端合并，避免再加一个重复的管理接口。

const state = { feeds: [], status: null, editing: null, adding: false };

const $ = (sel, root = document) => root.querySelector(sel);

function esc(v) {
  return String(v == null ? '' : v).replace(/[&<>"']/g, (c) => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
  }[c]));
}

function truncate(text, n) {
  const s = String(text || '');
  return s.length > n ? s.slice(0, n) + '…' : s;
}

// api 统一处理 JSON 与错误：后端非 2xx 一律回 {error}，这里直接抛出可展示的文案。
async function api(path, { method = 'GET', body } = {}) {
  const res = await fetch(path, {
    method,
    headers: body ? { 'Content-Type': 'application/json' } : undefined,
    body: body ? JSON.stringify(body) : undefined,
  });
  if (res.status === 204) return null;
  const text = await res.text();
  let data = null;
  if (text) {
    try { data = JSON.parse(text); } catch { data = null; }
  }
  if (!res.ok) throw new Error((data && data.error) || `请求失败（HTTP ${res.status}）`);
  return data;
}

let toastTimer = 0;
function toast(message, type = 'ok') {
  const box = $('#toast');
  box.textContent = message;
  box.className = `toast ${type}`;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => box.classList.add('hidden'), 3200);
}

function fmtTime(value) {
  if (!value) return '—';
  const d = new Date(value);
  if (Number.isNaN(d.getTime())) return '—';
  return d.toLocaleString('zh-CN', { hour12: false });
}

function fmtBytes(n) {
  if (!n) return '0 B';
  const units = ['B', 'KB', 'MB', 'GB'];
  let i = 0;
  let v = n;
  while (v >= 1024 && i < units.length - 1) { v /= 1024; i++; }
  return `${v.toFixed(i === 0 ? 0 : 1)} ${units[i]}`;
}

// 与后端 validateFeedURL 同源，先在前端拦一次，后端仍然兜底。
function checkURL(raw) {
  const value = String(raw || '').trim();
  if (!value) return 'URL 必填';
  let u;
  try { u = new URL(value); } catch { return 'URL 不是合法的链接'; }
  if (u.protocol !== 'http:' && u.protocol !== 'https:') return 'URL 只支持 http/https';
  if (!u.hostname) return 'URL 缺少主机名';
  return '';
}

function statusByID() {
  const map = new Map();
  const feeds = (state.status && state.status.feeds) || [];
  for (const f of feeds) map.set(f.id, f);
  return map;
}

function badge(enabled) {
  return enabled
    ? '<span class="badge ok">启用</span>'
    : '<span class="badge off">停用</span>';
}

function editorRow(feed) {
  const isNew = !feed.id;
  return `<tr class="editor"><td colspan="8">
    <form class="edit-form" data-id="${isNew ? '' : feed.id}">
      <label class="field">
        <span>名称</span>
        <input name="name" value="${esc(isNew ? '' : feed.name)}" placeholder="例如 HackerNews" required>
      </label>
      <label class="field grow">
        <span>URL</span>
        <input name="url" value="${esc(isNew ? '' : feed.url)}" placeholder="https://example.com/rss.xml" required>
      </label>
      <label class="field check">
        <span>状态</span>
        <input type="checkbox" name="enabled" ${isNew || feed.enabled ? 'checked' : ''}> 启用
      </label>
      <div class="form-actions">
        <button class="btn primary" type="submit">保存</button>
        <button class="btn ghost" type="button" data-act="cancel">取消</button>
      </div>
      <p class="field-error" hidden></p>
    </form>
  </td></tr>`;
}

function feedRow(feed, status) {
  const err = status && status.last_error
    ? `<span class="err" title="${esc(status.last_error)}">${esc(truncate(status.last_error, 36))}</span>`
    : '—';
  return `<tr data-id="${feed.id}">
    <td class="name">${esc(feed.name)}</td>
    <td class="url"><a href="${esc(feed.url)}" target="_blank" rel="noreferrer">${esc(feed.url)}</a></td>
    <td>${badge(feed.enabled)}</td>
    <td>${fmtTime(status && status.last_success_at)}</td>
    <td class="num">${status ? status.items_today : '—'}</td>
    <td class="num">${status ? status.items_total : '—'}</td>
    <td class="err-cell">${err}</td>
    <td class="actions">
      <button class="btn tiny" data-act="edit">编辑</button>
      <button class="btn tiny" data-act="toggle">${feed.enabled ? '停用' : '启用'}</button>
      <button class="btn tiny danger" data-act="delete">删除</button>
    </td>
  </tr>`;
}

function renderFeeds() {
  const body = $('#feeds-body');
  const statuses = statusByID();
  const parts = [];

  if (state.adding) parts.push(editorRow({}));

  if (!state.feeds.length && !state.adding) {
    parts.push('<tr><td colspan="8" class="empty">还没有订阅源，点「新增订阅源」添加。</td></tr>');
  }

  for (const feed of state.feeds) {
    parts.push(feedRow(feed, statuses.get(feed.id)));
    if (state.editing === feed.id) parts.push(editorRow(feed));
  }

  body.innerHTML = parts.join('');
  $('#feed-count').textContent = state.feeds.length
    ? `共 ${state.feeds.length} 个源，启用 ${state.feeds.filter((f) => f.enabled).length} 个`
    : '';

  if (state.adding) $('input[name="name"]', body).focus();
  else if (state.editing) {
    const input = $(`.editor input[name="name"]`, body);
    if (input) input.focus();
  }
}

function renderStatus() {
  const s = state.status;
  if (!s) return;

  const cards = [
    ['条目总数', s.items.total, `近 24 小时 ${s.items.last_24h} 条`],
    ['设备', s.devices.total, `近 1 小时活跃 ${s.devices.active_1h} 台`],
    ['图片', s.images.files, fmtBytes(s.images.bytes)],
    ['数据库', s.database.file || '—', `${fmtBytes(s.database.bytes)} · 条目最长 ${s.max_age_hours} 小时`],
  ];

  $('#status-cards').innerHTML = cards.map(([label, value, sub]) => `
    <div class="card">
      <div class="card-label">${esc(label)}</div>
      <div class="card-value">${esc(value)}</div>
      <div class="card-sub">${esc(sub)}</div>
    </div>`).join('');

  $('#status-feeds').innerHTML = (s.feeds || []).map((f) => `<tr>
    <td class="name">${esc(f.name)}</td>
    <td class="url">${esc(f.url)}</td>
    <td>${badge(f.enabled)}</td>
    <td>${fmtTime(f.last_success_at)}</td>
    <td>${f.last_error_at ? `${fmtTime(f.last_error_at)}<br><span class="err">${esc(truncate(f.last_error, 60))}</span>` : '—'}</td>
    <td class="num">${f.items_today}</td>
    <td class="num">${f.items_total}</td>
  </tr>`).join('') || '<tr><td colspan="7" class="empty">暂无数据</td></tr>';
}

async function loadAll() {
  try {
    const [feeds, status] = await Promise.all([api('/v1/feeds'), api('/admin/status')]);
    state.feeds = feeds || [];
    state.status = status;
    renderFeeds();
    renderStatus();
  } catch (err) {
    toast(`加载失败：${err.message}`, 'error');
  }
}

async function saveFeed(form) {
  const id = form.dataset.id;
  const name = form.elements.name.value.trim();
  const url = form.elements.url.value.trim();
  const enabled = form.elements.enabled.checked;
  const errorBox = $('.field-error', form);

  const urlError = checkURL(url);
  if (!name) return showFieldError(errorBox, '名称必填');
  if (urlError) return showFieldError(errorBox, urlError);

  setFormBusy(form, true);
  try {
    if (id) {
      await api(`/v1/feeds/${id}`, { method: 'PATCH', body: { name, url, enabled } });
      toast('已保存');
    } else {
      await api('/v1/feeds', { method: 'POST', body: { name, url, enabled } });
      toast('已新增，下一轮抓取生效');
    }
    state.editing = null;
    state.adding = false;
    await loadAll();
  } catch (err) {
    showFieldError(errorBox, err.message);
  } finally {
    setFormBusy(form, false);
  }
}

function showFieldError(box, message) {
  if (!box) return;
  box.hidden = false;
  box.textContent = message;
}

function setFormBusy(form, busy) {
  const submit = form.querySelector('button[type="submit"]');
  if (submit) submit.disabled = busy;
}

async function toggleFeed(feed, button) {
  button.disabled = true;
  try {
    await api(`/v1/feeds/${feed.id}`, { method: 'PATCH', body: { enabled: !feed.enabled } });
    toast(feed.enabled ? '已停用' : '已启用');
    await loadAll();
  } catch (err) {
    toast(`操作失败：${err.message}`, 'error');
    button.disabled = false;
  }
}

async function deleteFeed(feed) {
  if (!confirm(`删除「${feed.name}」？该源下已抓取的条目会一并删除，无法恢复。`)) return;
  try {
    await api(`/v1/feeds/${feed.id}`, { method: 'DELETE' });
    toast('已删除');
    await loadAll();
  } catch (err) {
    toast(`删除失败：${err.message}`, 'error');
  }
}

function bindEvents() {
  $('#feeds-body').addEventListener('click', (event) => {
    const button = event.target.closest('button[data-act]');
    if (!button) return;
    const row = button.closest('tr');
    const feed = state.feeds.find((f) => String(f.id) === row.dataset.id);

    if (button.dataset.act === 'cancel') {
      state.editing = null;
      state.adding = false;
      renderFeeds();
      return;
    }
    if (!feed) return;

    if (button.dataset.act === 'edit') {
      state.adding = false;
      state.editing = state.editing === feed.id ? null : feed.id;
      renderFeeds();
    } else if (button.dataset.act === 'toggle') {
      toggleFeed(feed, button);
    } else if (button.dataset.act === 'delete') {
      deleteFeed(feed);
    }
  });

  $('#feeds-body').addEventListener('submit', (event) => {
    const form = event.target.closest('form.edit-form');
    if (!form) return;
    event.preventDefault();
    saveFeed(form);
  });

  $('#add-feed').addEventListener('click', () => {
    state.editing = null;
    state.adding = !state.adding;
    renderFeeds();
  });

  $('#reload').addEventListener('click', async (event) => {
    event.target.disabled = true;
    await loadAll();
    event.target.disabled = false;
    toast('已刷新');
  });

  document.querySelectorAll('.tab').forEach((tab) => {
    tab.addEventListener('click', () => {
      document.querySelectorAll('.tab').forEach((t) => t.classList.toggle('active', t === tab));
      $('#tab-feeds').classList.toggle('hidden', tab.dataset.tab !== 'feeds');
      $('#tab-status').classList.toggle('hidden', tab.dataset.tab !== 'status');
    });
  });
}

bindEvents();
loadAll();
