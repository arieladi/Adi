// Minimal RFC 6455 WebSocket server, localhost only, zero dependencies.
//
// Studio OS carries small JSON control messages (MIDI notes, CC, keystroke
// tokens, short replies), so a full library would be dead weight and one more
// thing to vendor, audit and keep current. This handles exactly what the IPC
// contract needs: the handshake, masked client text frames including fragmented
// and 16/64-bit-length ones, ping/pong, and a clean close.
//
// It refuses any connection that is not from the loopback interface. The socket
// can drive MIDI and synthesise keystrokes, so it must never be reachable off
// this machine even if the bind address is ever changed by mistake.
import http from "node:http";
import crypto from "node:crypto";

const GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

const OP = { CONT: 0x0, TEXT: 0x1, BIN: 0x2, CLOSE: 0x8, PING: 0x9, PONG: 0xa };
const HEARTBEAT_MS = 15000;  // ping cadence; a client that misses one is dropped
const MAX_MESSAGE = 1 << 20; // 1 MiB — far above any real payload; a bigger
                             // frame means something is wrong, so we close.

export class WsServer {
  constructor({ port = 9011, host = "127.0.0.1", logger = console } = {}) {
    this.port = port;
    this.host = host;
    this.log = logger;
    this.clients = new Set();
    this.onMessage = () => {};
    this.onConnect = () => {};
    this.onDisconnect = () => {};
    this.server = null;
  }

  listen() {
    this.server = http.createServer((req, res) => {
      // A plain GET is useful for "is the service alive?" from a shell.
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify({ service: "studioos", clients: this.clients.size }));
    });

    this.server.on("upgrade", (req, socket, head) => this._upgrade(req, socket, head));
    /* V64 — A LISTEN FAILURE MUST EXIT, NOT LIMP ON. This used to log and keep
       running: measured, after EADDRINUSE the process stayed alive forever with
       no listener, so every key on the board was a silent no-op and KeepAlive
       could not rescue it because it never exited. Now the supervisor gets its
       chance. Non-listen errors still only log — those are per-connection. */
    this.server.on("error", (e) => {
      this.log.error?.(`ws listen failed: ${e.message}`);
      if (e && (e.code === "EADDRINUSE" || e.code === "EACCES")) {
        this.log.error?.(`port ${this.port} is unusable — exiting so the supervisor can retry`);
        process.exit(1);
      }
    });
    this.server.listen(this.port, this.host, () => {
      this.log.info?.(`studioos service listening on ws://${this.host}:${this.port}`);
    });

    /* Heartbeat. Observed on the real install: after a few Stream Deck app
       restarts the service reported 3 clients when only 1 existed. A CEF page
       that goes away without a clean close leaves a half-open TCP connection
       that never fires 'close', so the socket count only ever grows.

       That is not cosmetic. "Silence every sounding note when the LAST client
       disconnects" is the guarantee that stops a crash mid-nudge from leaving a
       note stuck on in rekordbox — and a phantom client means the last real one
       leaving never looks like the last one. */
    this.heartbeat = setInterval(() => {
      for (const c of [...this.clients]) {
        if (!c.awaitingPong) { c.awaitingPong = true; c.ping(); continue; }
        this.log.warn?.("dropping a client that missed its pong (half-open socket)");
        c.terminate();
      }
    }, HEARTBEAT_MS);
    this.heartbeat.unref?.();   // never hold the process open on its own
    return this;
  }

  _upgrade(req, socket, head) {
    const addr = socket.remoteAddress || "";
    const local = addr === "127.0.0.1" || addr === "::1" || addr === "::ffff:127.0.0.1";
    const key = req.headers["sec-websocket-key"];
    if (!local || !key) {
      socket.end("HTTP/1.1 403 Forbidden\r\n\r\n");
      return;
    }

    const accept = crypto.createHash("sha1").update(key + GUID).digest("base64");
    socket.write(
      "HTTP/1.1 101 Switching Protocols\r\n" +
      "Upgrade: websocket\r\n" +
      "Connection: Upgrade\r\n" +
      `Sec-WebSocket-Accept: ${accept}\r\n\r\n`
    );
    socket.setNoDelay(true); // control messages must not wait on Nagle

    const client = new WsClient(socket, this);
    this.clients.add(client);
    this.onConnect(client);
    if (head && head.length) client._feed(head);

    socket.on("data", (chunk) => client._feed(chunk));
    socket.on("error", () => client._gone());
    socket.on("close", () => client._gone());
    /* V64 — 'end' IS THE EVENT THAT ACTUALLY FIRES, and its absence was the whole
       bug. http.Server builds its sockets with allowHalfOpen: true, so when a CEF
       page vanishes the server socket gets 'end' and then stays writable FOREVER
       — 'close' is never emitted. Measured on the app's own bundled node 20.20.0:
       'end' true, 'close' false at +300ms, +1s and +2s.

       So the only reaper was the 15 s heartbeat, and it needs TWO cycles because
       cycle 1 only arms awaitingPong. Thirty seconds of a phantom client — during
       which index.js's `clients.size === 0` never becomes true, so the REAL client
       leaving never triggers midi.panicAll() and a held note can stay sounding.
       That is the one guarantee this service's own header promises.

       `terminate()` rather than `_gone()`: the peer is gone, so there is nothing
       to hand a close handshake to, and destroy() is what frees the fd. */
    socket.on("end", () => client.terminate());
  }

  broadcast(obj) {
    const s = JSON.stringify(obj);
    for (const c of this.clients) c.sendRaw(s);
  }

  close() {
    clearInterval(this.heartbeat);
    for (const c of [...this.clients]) c.close();
    this.server?.close();
  }
}

class WsClient {
  constructor(socket, server) {
    this.socket = socket;
    this.server = server;
    this.buf = Buffer.alloc(0);
    this.fragments = [];   // continuation frames of the current message
    this.fragOp = null;
    this.alive = true;
    this.awaitingPong = false;
  }

  ping() {
    if (!this.alive) return;
    try { this.socket.write(encode(OP.PING, Buffer.alloc(0))); }
    catch { this._gone(); }
  }

  // Hard drop for a socket that stopped answering — close() waits on a TCP
  // handshake the peer will never complete.
  terminate() {
    if (!this.alive) return;
    try { this.socket.destroy(); } catch { /* already torn down */ }
    this._gone();
  }

  send(obj) { this.sendRaw(JSON.stringify(obj)); }

  sendRaw(text) {
    if (!this.alive) return false;
    try { this.socket.write(encode(OP.TEXT, Buffer.from(text, "utf8"))); return true; }
    catch { this._gone(); return false; }
  }

  close(code = 1000) {
    if (!this.alive) return;
    try {
      const b = Buffer.alloc(2);
      b.writeUInt16BE(code, 0);
      this.socket.write(encode(OP.CLOSE, b));
      this.socket.end();
    } catch { /* already gone */ }
    this._gone();
  }

  /* V64 — _gone() USED TO LEAVE THE SOCKET OPEN with all three listeners still
     attached. Since the heartbeat only walks `server.clients`, once _gone() had
     run nothing could ever reap that socket again — and `_feed` kept buffering on
     it. Measured: 1 MiB retained per continuation frame, no ceiling, on a client
     the service had already reported as disconnected.

     Destroying here is safe for the graceful path too: close() writes its CLOSE
     frame and calls end() BEFORE calling us, so the bytes are already queued. */
  _gone() {
    if (!this.alive) return;
    this.alive = false;
    this.server.clients.delete(this);
    try { this.socket.destroy(); } catch { /* already torn down */ }
    this.buf = Buffer.alloc(0);
    this.fragments = [];
    this.fragOp = null;
    this.server.onDisconnect(this);
  }

  _feed(chunk) {
    /* V64 — a dead client must not still dispatch. Without this, verbs arriving
       in the same tick as the teardown ran AFTER onDisconnect: measured, two of
       them. A midi.noteOn there is armed after the final panicAll() and, because
       the client is already out of the set, nothing will ever panic again — a
       permanently stuck note. */
    if (!this.alive) return;
    this.buf = this.buf.length ? Buffer.concat([this.buf, chunk]) : chunk;
    for (;;) {
      const frame = decode(this.buf);
      if (!frame) return;                       // need more bytes
      if (frame.error) { this.close(1002); return; }
      this.buf = this.buf.subarray(frame.size);

      this.awaitingPong = false;   // any frame proves the peer is alive
      if (frame.op === OP.CLOSE) { this.close(1000); return; }
      if (frame.op === OP.PING) {
        try { this.socket.write(encode(OP.PONG, frame.payload)); } catch { this._gone(); }
        continue;
      }
      if (frame.op === OP.PONG) { this.awaitingPong = false; continue; }

      // Text / continuation. Reassemble before parsing so a split JSON payload
      // is never handed to JSON.parse half-formed.
      if (frame.op === OP.CONT) {
        if (this.fragOp === null) { this.close(1002); return; }
        this.fragments.push(frame.payload);
      } else {
        if (this.fragOp !== null) { this.close(1002); return; }
        this.fragOp = frame.op;
        this.fragments = [frame.payload];
      }

      const total = this.fragments.reduce((n, b) => n + b.length, 0);
      if (total > MAX_MESSAGE) { this.close(1009); return; }
      if (!frame.fin) continue;

      const payload = this.fragments.length === 1 ? this.fragments[0] : Buffer.concat(this.fragments);
      const wasText = this.fragOp === OP.TEXT;
      this.fragments = []; this.fragOp = null;
      if (!wasText) continue;

      let msg;
      try { msg = JSON.parse(payload.toString("utf8")); } catch { continue; }
      this.server.onMessage(msg, this);
    }
  }
}

// ---------------------------------------------------------------- framing
function encode(op, payload) {
  const len = payload.length;
  let header;
  if (len < 126) {
    header = Buffer.alloc(2);
    header[1] = len;
  } else if (len < 65536) {
    header = Buffer.alloc(4);
    header[1] = 126;
    header.writeUInt16BE(len, 2);
  } else {
    header = Buffer.alloc(10);
    header[1] = 127;
    header.writeBigUInt64BE(BigInt(len), 2);
  }
  header[0] = 0x80 | op;   // FIN + opcode; the server never fragments
  return Buffer.concat([header, payload]);
}

// Returns null when more bytes are needed, { error } on a protocol violation,
// otherwise { fin, op, payload, size }.
function decode(buf) {
  if (buf.length < 2) return null;
  const fin = (buf[0] & 0x80) !== 0;
  const rsv = buf[0] & 0x70;
  const op = buf[0] & 0x0f;
  const masked = (buf[1] & 0x80) !== 0;
  let len = buf[1] & 0x7f;
  let off = 2;

  if (rsv) return { error: true };            // no extensions negotiated
  if (!masked) return { error: true };        // clients MUST mask (RFC 6455 §5.1)

  if (len === 126) {
    if (buf.length < off + 2) return null;
    len = buf.readUInt16BE(off); off += 2;
  } else if (len === 127) {
    if (buf.length < off + 8) return null;
    const big = buf.readBigUInt64BE(off); off += 8;
    if (big > BigInt(MAX_MESSAGE)) return { error: true };
    len = Number(big);
  }

  if (buf.length < off + 4 + len) return null;
  const mask = buf.subarray(off, off + 4); off += 4;
  const payload = Buffer.allocUnsafe(len);
  for (let i = 0; i < len; i++) payload[i] = buf[off + i] ^ mask[i & 3];

  return { fin, op, payload, size: off + len };
}
