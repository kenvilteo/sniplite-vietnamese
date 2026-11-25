// sniplite upload server: Cloudflare Worker + R2.
//   POST   /upload   body = PNG/JPEG, Authorization: Bearer <TOKEN>  -> {"id","url"}
//   GET    /list     Authorization                                    -> [{id,size,uploaded}]
//   DELETE /<id>     Authorization                                    -> 204
//   GET    /<id>     public: the image itself
//   GET    /         the gallery page (asks for the token, keeps it in this browser)
// Secret: wrangler secret put TOKEN

const MAX_BYTES = 10 * 1024 * 1024;
const TYPES = { "image/png": "png", "image/jpeg": "jpg" };
const ID_RE = /^[A-Za-z0-9]{10}\.(png|jpg)$/;

function authed(req, env) {
  const got = req.headers.get("Authorization") || "";
  const want = "Bearer " + (env.TOKEN || "");
  if (!env.TOKEN || got.length !== want.length) return false;
  let diff = 0;
  for (let i = 0; i < want.length; i++) diff |= got.charCodeAt(i) ^ want.charCodeAt(i);
  return diff === 0;
}

function newId(ext) {
  const abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  const bytes = crypto.getRandomValues(new Uint8Array(10));
  let s = "";
  for (const b of bytes) s += abc[b % abc.length];
  return s + "." + ext;
}

const json = (v, status = 200) =>
  new Response(JSON.stringify(v), { status, headers: { "Content-Type": "application/json; charset=utf-8" } });

export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    const path = url.pathname.replace(/^\/+/, "");

    if (req.method === "GET" && path === "") {
      return new Response(GALLERY, {
        headers: {
          "Content-Type": "text/html; charset=utf-8",
          "Content-Security-Policy": "default-src 'self'; img-src 'self'; style-src 'unsafe-inline'; script-src 'unsafe-inline'",
        },
      });
    }

    if (req.method === "GET" && ID_RE.test(path)) {
      const obj = await env.IMG.get(path);
      if (!obj) return new Response("Not found", { status: 404 });
      return new Response(obj.body, {
        headers: {
          "Content-Type": obj.httpMetadata?.contentType || "image/png",
          "Cache-Control": "public, max-age=31536000, immutable",
          "ETag": obj.httpEtag,
        },
      });
    }

    if (!authed(req, env)) return new Response("Unauthorized", { status: 401 });

    if ((req.method === "POST" || req.method === "PUT") && path === "upload") {
      const type = (req.headers.get("Content-Type") || "").split(";")[0].trim().toLowerCase();
      const ext = TYPES[type];
      if (!ext) return new Response("Only image/png or image/jpeg", { status: 415 });
      const len = Number(req.headers.get("Content-Length") || 0);
      if (len > MAX_BYTES) return new Response("Too large", { status: 413 });
      const body = await req.arrayBuffer();
      if (body.byteLength === 0 || body.byteLength > MAX_BYTES) return new Response("Bad size", { status: 413 });
      const id = newId(ext);
      await env.IMG.put(id, body, { httpMetadata: { contentType: type } });
      return json({ id, url: url.origin + "/" + id });
    }

    if (req.method === "GET" && path === "list") {
      const out = [];
      let cursor;
      do {
        const r = await env.IMG.list({ limit: 1000, cursor });
        for (const o of r.objects) out.push({ id: o.key, size: o.size, uploaded: o.uploaded });
        cursor = r.truncated ? r.cursor : undefined;
      } while (cursor);
      out.sort((a, b) => (a.uploaded < b.uploaded ? 1 : -1));
      return json(out);
    }

    if (req.method === "DELETE" && ID_RE.test(path)) {
      await env.IMG.delete(path);
      return new Response(null, { status: 204 });
    }

    return new Response("Bad request", { status: 400 });
  },
};

const GALLERY = `<!doctype html>
<html lang="vi"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>sniplite – ảnh đã tải lên</title>
<style>
 body{font:14px system-ui,Segoe UI,sans-serif;margin:0;background:#f4f4f5;color:#18181b}
 header{display:flex;gap:12px;align-items:center;padding:12px 20px;background:#fff;border-bottom:1px solid #e4e4e7;position:sticky;top:0}
 header h1{font-size:16px;margin:0;flex:1}
 button{font:inherit;padding:5px 10px;border:1px solid #d4d4d8;border-radius:6px;background:#fff;cursor:pointer}
 button.del{color:#b91c1c;border-color:#fca5a5}
 #grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(220px,1fr));gap:14px;padding:20px}
 .card{background:#fff;border:1px solid #e4e4e7;border-radius:8px;overflow:hidden;display:flex;flex-direction:column}
 .card a{display:block;background:#fafafa;height:160px}
 .card img{width:100%;height:100%;object-fit:contain}
 .meta{padding:8px 10px;font-size:12px;color:#52525b;display:flex;flex-direction:column;gap:6px}
 .row{display:flex;gap:6px}
 #msg{padding:20px;color:#52525b}
</style></head><body>
<header><h1>Ảnh đã tải lên <span id="count"></span></h1><button id="reload">Tải lại</button><button id="logout">Đăng xuất</button></header>
<div id="msg"></div><div id="grid"></div>
<script>
const KEY = "sniplite_token";
const $ = id => document.getElementById(id);
function token() {
  let t = localStorage.getItem(KEY);
  if (!t) { t = prompt("Nhập upload_token:") || ""; if (t) localStorage.setItem(KEY, t); }
  return t;
}
async function api(method, path) {
  const r = await fetch(path, { method, headers: { Authorization: "Bearer " + token() } });
  if (r.status === 401) { localStorage.removeItem(KEY); throw new Error("Sai token"); }
  if (!r.ok) throw new Error("HTTP " + r.status);
  return r.status === 204 ? null : r.json();
}
function card(it) {
  const link = location.origin + "/" + it.id;
  const c = document.createElement("div"); c.className = "card";
  const a = document.createElement("a"); a.href = link; a.target = "_blank"; a.rel = "noopener";
  const img = document.createElement("img"); img.loading = "lazy"; img.src = "/" + it.id; img.alt = it.id;
  a.append(img);
  const m = document.createElement("div"); m.className = "meta";
  const info = document.createElement("div");
  info.textContent = new Date(it.uploaded).toLocaleString("vi-VN") + " · " + Math.round(it.size / 1024) + " KB";
  const row = document.createElement("div"); row.className = "row";
  const copy = document.createElement("button"); copy.textContent = "Chép link";
  copy.onclick = async () => { await navigator.clipboard.writeText(link); copy.textContent = "Đã chép"; };
  const del = document.createElement("button"); del.textContent = "Xóa"; del.className = "del";
  del.onclick = async () => {
    if (!confirm("Xóa " + it.id + "?")) return;
    try { await api("DELETE", "/" + it.id); c.remove(); count(-1); } catch (e) { alert(e.message); }
  };
  row.append(copy, del); m.append(info, row); c.append(a, m);
  return c;
}
let total = 0;
function count(d) { total += d; $("count").textContent = "(" + total + ")"; }
async function load() {
  $("grid").replaceChildren(); $("msg").textContent = "Đang tải...";
  try {
    const items = await api("GET", "/list");
    total = 0; count(items.length);
    $("msg").textContent = items.length ? "" : "Chưa có ảnh nào.";
    $("grid").append(...items.map(card));
  } catch (e) { $("msg").textContent = e.message; }
}
$("reload").onclick = load;
$("logout").onclick = () => { localStorage.removeItem(KEY); location.reload(); };
load();
</script></body></html>`;
