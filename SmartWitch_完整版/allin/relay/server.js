'use strict';
/* ============================================================================
 * SmartWitch 局域网中转服务（零依赖，只用 Node 内置模块）
 * ----------------------------------------------------------------------------
 * 作用：让「多人 / iPhone」也能看到手表数据。
 *
 *   电脑（主机）                     中转服务                手机 / iPad（跟随端）
 *   Chrome 打开 http://localhost ─┐                        ┌─ http://192.168.x.x
 *   连一次手表蓝牙（Web Bluetooth）├── WebSocket ──────────┤─ 普通浏览器即可
 *   把每帧 JSON 转发上来           │   (本文件)            │  不需要蓝牙、人数不限
 *                                  └──────────────────────┘
 *
 * 为什么必须由电脑做主机：Web Bluetooth 要求「安全上下文」，
 * 只有 https / localhost / file:// 三种。手机访问 http://192.168.x.x 不是安全
 * 上下文，navigator.bluetooth 会直接不存在 —— 所以手机端只能被动接收数据。
 * 而电脑访问 http://localhost:PORT 属于安全上下文，蓝牙照常可用。
 *
 * 用法：node server.js [端口]      默认 8099
 * ==========================================================================*/

const http   = require('http');
const fs     = require('fs');
const path   = require('path');
const crypto = require('crypto');
const os     = require('os');
const { exec } = require('child_process');

const PORT     = Number(process.env.PORT || process.argv[2] || 8099);
const NO_OPEN  = process.argv.includes('--no-open');
const WEB_ROOT = path.join(__dirname, '..', 'web');
const INDEX    = 'SmartWitch_BLE_Dashboard.html';
const WS_GUID  = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.htm':  'text/html; charset=utf-8',
  '.css':  'text/css; charset=utf-8',
  '.js':   'text/javascript; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.png':  'image/png',
  '.jpg':  'image/jpeg',
  '.jpeg': 'image/jpeg',
  '.gif':  'image/gif',
  '.svg':  'image/svg+xml',
  '.ico':  'image/x-icon',
  '.txt':  'text/plain; charset=utf-8'
};

/* ======================= 局域网地址 ======================= */
function lanIPs() {
  const out = [];
  const ifs = os.networkInterfaces();
  for (const name of Object.keys(ifs)) {
    for (const ni of ifs[name] || []) {
      if (ni.family === 'IPv4' && !ni.internal) out.push(ni.address);
    }
  }
  return out.length ? out : ['<本机IP>'];
}

/* ======================= WebSocket 编解码 ======================= */
// 服务端发出的帧不加掩码（RFC 6455 规定）
function wsEncodeText(str) {
  const p = Buffer.from(str, 'utf8');
  const n = p.length;
  let h;
  if (n < 126) {
    h = Buffer.from([0x81, n]);
  } else if (n < 65536) {
    h = Buffer.alloc(4); h[0] = 0x81; h[1] = 126; h.writeUInt16BE(n, 2);
  } else {
    h = Buffer.alloc(10); h[0] = 0x81; h[1] = 127; h.writeBigUInt64BE(BigInt(n), 2);
  }
  return Buffer.concat([h, p]);
}
function wsEncodeCtl(opcode, payload) {
  const p = Buffer.isBuffer(payload) ? payload : Buffer.alloc(0);
  const n = p.length;
  let h;
  if (n < 126) {
    h = Buffer.from([0x80 | opcode, n]);
  } else {
    h = Buffer.alloc(4); h[0] = 0x80 | opcode; h[1] = 126; h.writeUInt16BE(n, 2);
  }
  return Buffer.concat([h, p]);
}

/* ======================= 客户端与角色 ======================= */
const clients = new Set();
let hostClient = null;      // 唯一的「主机」= 负责连手表并上报数据的那台
let lastFrame  = null;      // 缓存最近一帧，后进来的人立刻能看到数据
let lastStatus = '';
let totalFrames = 0, rateFrames = 0, rateTs = Date.now();

const shortName = (c) => c.name || c.ip;

function send(c, obj) {
  if (!c || c.sock.destroyed) return;
  try { c.sock.write(wsEncodeText(JSON.stringify(obj))); } catch (e) { /* 忽略 */ }
}
function broadcast(obj, except) {
  for (const c of clients) if (c !== except) send(c, obj);
}

function pushRoles() {
  const viewers = clients.size - (hostClient ? 1 : 0);
  for (const c of clients) {
    send(c, {
      t: 'roles',
      role: c === hostClient ? 'host' : 'viewer',
      host: hostClient ? shortName(hostClient) : '',
      viewers: viewers < 0 ? 0 : viewers
    });
  }
}

// 主机选举：有 Web Bluetooth 的那台优先；主机掉线后自动移交
function electHost() {
  if (hostClient && clients.has(hostClient)) return;
  hostClient = null;
  for (const c of clients) if (c.canHost) { hostClient = c; break; }
  if (hostClient) console.log('[角色] 主机 = ' + shortName(hostClient));
}

/* ======================= 消息处理 ======================= */
function onMessage(c, text) {
  let m;
  try { m = JSON.parse(text); } catch (e) { return; }

  if (m.t === 'hello') {
    c.canHost = !!m.canHost;
    c.name    = String(m.name || '').slice(0, 24);
    electHost();
    send(c, {
      t: 'info',
      port: PORT,
      urls: lanIPs().map((ip) => 'http://' + ip + ':' + PORT)
    });
    if (lastStatus) send(c, { t: 'status', s: lastStatus });
    if (lastFrame && c !== hostClient) send(c, { t: 'frame', d: lastFrame });
    pushRoles();
    console.log('[接入] ' + c.ip + (c.name ? ' (' + c.name + ')' : '') +
                ' 共 ' + clients.size + ' 个连接');

  } else if (m.t === 'frame') {
    if (c !== hostClient) return;              // 只接受主机上报的数据帧
    lastFrame = m.d;
    totalFrames++; rateFrames++;
    for (const v of clients) if (v !== c) send(v, { t: 'frame', d: m.d });

  } else if (m.t === 'status') {
    if (c !== hostClient) return;
    lastStatus = String(m.s || '').slice(0, 60);
    broadcast({ t: 'status', s: lastStatus }, c);
    console.log('[主机状态] ' + lastStatus);

  } else if (m.t === 'cmd') {
    const cmd = String(m.c || '').slice(0, 32);
    if (!hostClient) { send(c, { t: 'status', s: '主机不在线，指令未执行' }); return; }
    send(hostClient, { t: 'cmd', c: cmd });
    console.log('[指令] ' + c.ip + ' → ' + cmd);
  }
}

/* ======================= 帧解析（客户端发来的帧一定带掩码） ======================= */
function onData(c, chunk) {
  c.buf = Buffer.concat([c.buf, chunk]);

  for (;;) {
    const b = c.buf;
    if (b.length < 2) return;

    const opcode = b[0] & 0x0f;
    const masked = (b[1] & 0x80) !== 0;
    let len = b[1] & 0x7f;
    let off = 2;

    if (len === 126) {
      if (b.length < 4) return;
      len = b.readUInt16BE(2); off = 4;
    } else if (len === 127) {
      if (b.length < 10) return;
      len = Number(b.readBigUInt64BE(2)); off = 10;
    }
    if (len > 2 * 1024 * 1024) { c.sock.destroy(); return; }   // 异常大的帧直接断开

    let maskKey = null;
    if (masked) {
      if (b.length < off + 4) return;
      maskKey = b.slice(off, off + 4);
      off += 4;
    }
    if (b.length < off + len) return;

    let data = b.slice(off, off + len);
    if (masked) {
      data = Buffer.from(data);
      for (let i = 0; i < data.length; i++) data[i] ^= maskKey[i % 4];
    }
    c.buf = b.slice(off + len);

    if (opcode === 0x8) { c.sock.end(); return; }              // close
    if (opcode === 0x9) { c.sock.write(wsEncodeCtl(0xA, data)); continue; }  // ping -> pong
    if (opcode === 0xA) continue;                              // pong
    if (opcode === 0x1) onMessage(c, data.toString('utf8'));    // text
    // 0x0 continuation：浏览器发小文本不会分片，忽略
  }
}

function drop(c) {
  if (!clients.has(c)) return;
  clients.delete(c);
  if (hostClient === c) { hostClient = null; console.log('[角色] 主机已离线，重新选举'); }
  electHost();
  pushRoles();
  console.log('[离开] ' + c.ip + ' 剩余 ' + clients.size + ' 个连接');
}

/* ======================= HTTP：同时托管仪表盘页面 ======================= */
const server = http.createServer((req, res) => {
  let u = decodeURIComponent((req.url || '/').split('?')[0]);
  if (u === '/') u = '/' + INDEX;

  if (u === '/health') {
    res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8' });
    res.end(JSON.stringify({
      ok: true,
      clients: clients.size,
      host: hostClient ? shortName(hostClient) : null,
      frames: totalFrames,
      hasLastFrame: !!lastFrame
    }));
    return;
  }

  const rel = path.normalize(u).replace(/^[/\\]+/, '');
  const fp  = path.join(WEB_ROOT, rel);
  if (!fp.startsWith(WEB_ROOT)) {
    res.writeHead(403, { 'Content-Type': 'text/plain; charset=utf-8' });
    res.end('403 forbidden');
    return;
  }

  fs.readFile(fp, (err, buf) => {
    if (err) {
      res.writeHead(404, { 'Content-Type': 'text/plain; charset=utf-8' });
      res.end('404 ' + u);
      return;
    }
    res.writeHead(200, {
      'Content-Type': MIME[path.extname(fp).toLowerCase()] || 'application/octet-stream',
      'Cache-Control': 'no-store'
    });
    res.end(buf);
  });
});

server.on('upgrade', (req, sock) => {
  if (!req.url || !req.url.startsWith('/ws') ||
      String(req.headers.upgrade || '').toLowerCase() !== 'websocket') {
    sock.destroy();
    return;
  }
  const key = req.headers['sec-websocket-key'];
  if (!key) { sock.destroy(); return; }

  const accept = crypto.createHash('sha1').update(key + WS_GUID).digest('base64');
  sock.write('HTTP/1.1 101 Switching Protocols\r\n' +
             'Upgrade: websocket\r\n' +
             'Connection: Upgrade\r\n' +
             'Sec-WebSocket-Accept: ' + accept + '\r\n\r\n');
  sock.setNoDelay(true);

  const c = {
    sock,
    buf: Buffer.alloc(0),
    ip: (req.socket.remoteAddress || '').replace(/^::ffff:/, ''),
    name: '',
    canHost: false
  };
  clients.add(c);
  sock.on('data',  (chunk) => onData(c, chunk));
  sock.on('close', () => drop(c));
  sock.on('error', () => drop(c));
});

/* ======================= 启动 ======================= */
server.on('error', (e) => {
  if (e.code === 'EADDRINUSE') {
    console.log('\n[错误] 端口 ' + PORT + ' 已被占用。');
    console.log('       换个端口运行，例如： node server.js 8123\n');
  } else {
    console.log('\n[错误] ' + e.message + '\n');
  }
  process.exit(1);
});

server.listen(PORT, () => {
  const ips = lanIPs();
  const line = '='.repeat(64);
  console.log('\n' + line);
  console.log('  SmartWitch 局域网中转服务 已启动');
  console.log(line);
  console.log('  本机（主机）打开：  http://localhost:' + PORT + '/');
  console.log('  其他人（手机）打开：');
  for (const ip of ips) console.log('      http://' + ip + ':' + PORT + '/');
  console.log(line);
  console.log('  步骤：');
  console.log('   1. 在「本机」那个页面点【连接蓝牙】，选中 SmartWitch');
  console.log('   2. 把上面的手机地址发给别人，他们打开就能同步看（iPhone 也行）');
  console.log('   3. 手机端点的切屏 / 按键 / 语音按钮，会经本机下发给手表');
  console.log(line);
  console.log('  停止服务：在本窗口按 Ctrl + C');
  console.log('  首次运行 Windows 可能弹出防火墙提示，请勾选「专用网络」并允许访问');
  console.log(line + '\n');

  if (!NO_OPEN && process.platform === 'win32') {
    exec('start "" "http://localhost:' + PORT + '/"', () => { /* 忽略失败 */ });
  }
});

setInterval(() => {
  const now = Date.now();
  const secs = (now - rateTs) / 1000;
  if (rateFrames > 0) {
    console.log('[速率] ' + (rateFrames / secs).toFixed(1) + ' 帧/秒 · 在线 ' +
                clients.size + ' 个连接 · 累计 ' + totalFrames + ' 帧');
  }
  rateFrames = 0;
  rateTs = now;
}, 10000);
