// Shared helpers for the built-in pages.
const $ = id => document.getElementById(id);
const sleep = ms => new Promise(r => setTimeout(r, ms));

// /ui POSTs must carry X-Requested-With (see requireUiAuth in main.cpp).
async function req(url, method = 'GET', body) {
  const r = await fetch(url, {method, body, headers: {'X-Requested-With': 'fetch'}});
  const t = await r.text();
  let j = null; try { j = JSON.parse(t); } catch (e) {}
  if (!r.ok || !j || !j.ok) throw new Error((j && j.error) || ('HTTP ' + r.status));
  return j;
}

// Builds an element; user-supplied text goes in via textContent, never innerHTML.
function el(tag, props = {}, ...children) {
  const e = Object.assign(document.createElement(tag), props);
  e.append(...children);
  return e;
}

function say(text, ok) { const m = $('msg'); m.textContent = text; m.className = ok ? 'ok' : 'err'; }

function uptime(s) {
  const d = Math.floor(s/86400), h = Math.floor(s%86400/3600), m = Math.floor(s%3600/60);
  return (d ? d+'d ' : '') + (d||h ? h+'h ' : '') + m + 'm';
}

function renderInfo(st) {
  $('ver').textContent = 'v' + st.version;
  $('info').textContent = `${st.hostname} · ${st.ip} · RSSI ${st.rssi} dBm · up ${uptime(st.uptime)} · firmware v${st.version}, built ${st.build}`;
}

// Disables every button while fn runs and reports failures in #msg.
async function busy(fn) {
  const btns = document.querySelectorAll('button');
  btns.forEach(b => b.disabled = true);
  try { await fn(); }
  catch (e) { say('Failed: ' + e.message, false); }
  finally { btns.forEach(b => b.disabled = false); }
}
