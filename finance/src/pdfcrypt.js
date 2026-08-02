/**
 * Minimal decryptor for PDFs using the standard security handler with RC4
 * (/V 1 /R 2 = 40-bit, /V 2 /R 3 = 128-bit). Israeli payslip senders use both.
 *
 * Why hand-rolled rather than a library: the goal is to hand Gemini a *readable PDF*
 * so it can use vision, which read these payslips far better than scrambled RTL text
 * extraction would. That needs a decrypted PDF back out, and the JS PDF libraries that
 * can decrypt (pdf.js) cannot re-serialise, while the ones that can serialise (pdf-lib)
 * cannot decrypt.
 *
 * The trick that makes this small: RC4 is a stream cipher, so plaintext and ciphertext
 * are the same length. Every stream can be decrypted IN PLACE, and the `/Encrypt N 0 R`
 * reference in the trailer can be overwritten with the same number of spaces. No byte
 * offset ever moves, so the xref table stays valid and no re-serialiser is needed.
 *
 * Scope: streams only, not top-level strings. A decrypted literal string can contain
 * bytes needing escapes, which would change its length and break every later offset.
 * Page text lives inside content streams (decrypted as a unit), so this is enough to
 * render. Metadata strings stay encrypted and unread, which nothing here depends on.
 *
 * AES (/V 4 or /V 5, i.e. /AESV2 or /AESV3) is NOT supported — AES is block-padded, so
 * the length-preserving property is lost. detectPdfEncryption() reports it so callers
 * can fail with a clear message instead of producing a corrupt file.
 */

// --- MD5 (WebCrypto has no MD5, and the standard handler is defined in terms of it) ---

const S = [7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
           5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
           4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
           6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21];
const K = new Uint32Array(64);
for (let i = 0; i < 64; i++) K[i] = Math.floor(Math.abs(Math.sin(i + 1)) * 4294967296);

export function md5(bytes) {
  const len = bytes.length;
  const withOne = len + 1;
  const padded = new Uint8Array(Math.ceil((withOne + 8) / 64) * 64);
  padded.set(bytes);
  padded[len] = 0x80;
  const bitLen = len * 8;
  new DataView(padded.buffer).setUint32(padded.length - 8, bitLen >>> 0, true);
  new DataView(padded.buffer).setUint32(padded.length - 4, Math.floor(bitLen / 4294967296), true);

  let a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
  const M = new Uint32Array(16);
  const view = new DataView(padded.buffer);

  for (let off = 0; off < padded.length; off += 64) {
    for (let i = 0; i < 16; i++) M[i] = view.getUint32(off + i * 4, true);
    let A = a0, B = b0, C = c0, D = d0;
    for (let i = 0; i < 64; i++) {
      let F, g;
      if (i < 16) { F = (B & C) | (~B & D); g = i; }
      else if (i < 32) { F = (D & B) | (~D & C); g = (5 * i + 1) % 16; }
      else if (i < 48) { F = B ^ C ^ D; g = (3 * i + 5) % 16; }
      else { F = C ^ (B | ~D); g = (7 * i) % 16; }
      F = (F + A + K[i] + M[g]) >>> 0;
      A = D; D = C; C = B;
      B = (B + ((F << S[i]) | (F >>> (32 - S[i])))) >>> 0;
    }
    a0 = (a0 + A) >>> 0; b0 = (b0 + B) >>> 0; c0 = (c0 + C) >>> 0; d0 = (d0 + D) >>> 0;
  }
  const out = new Uint8Array(16);
  const ov = new DataView(out.buffer);
  ov.setUint32(0, a0, true); ov.setUint32(4, b0, true);
  ov.setUint32(8, c0, true); ov.setUint32(12, d0, true);
  return out;
}

// --- RC4 ---

export function rc4(key, data) {
  const s = new Uint8Array(256);
  for (let i = 0; i < 256; i++) s[i] = i;
  for (let i = 0, j = 0; i < 256; i++) {
    j = (j + s[i] + key[i % key.length]) & 255;
    [s[i], s[j]] = [s[j], s[i]];
  }
  const out = new Uint8Array(data.length);
  for (let n = 0, i = 0, j = 0; n < data.length; n++) {
    i = (i + 1) & 255;
    j = (j + s[i]) & 255;
    [s[i], s[j]] = [s[j], s[i]];
    out[n] = data[n] ^ s[(s[i] + s[j]) & 255];
  }
  return out;
}

// --- PDF plumbing ---

const PAD = new Uint8Array([
  0x28, 0xBF, 0x4E, 0x5E, 0x4E, 0x75, 0x8A, 0x41, 0x64, 0x00, 0x4E, 0x56, 0xFF, 0xFA, 0x01, 0x08,
  0x2E, 0x2E, 0x00, 0xB6, 0xD0, 0x68, 0x3E, 0x80, 0x2F, 0x0C, 0xA9, 0xFE, 0x64, 0x53, 0x69, 0x7A]);

const latin1 = (bytes) => String.fromCharCode(...bytes);
const toBytes = (str) => Uint8Array.from(str, (c) => c.charCodeAt(0) & 255);

function hexToBytes(hex) {
  const clean = hex.replace(/[^0-9a-fA-F]/g, '');
  const out = new Uint8Array(clean.length >> 1);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(clean.substr(i * 2, 2), 16);
  return out;
}

/** Reads a PDF string that may be written as <hex> or (literal), returning raw bytes. */
function readPdfString(src, from) {
  let i = from;
  while (i < src.length && /\s/.test(src[i])) i++;
  if (src[i] === '<') {
    const end = src.indexOf('>', i);
    return hexToBytes(src.slice(i + 1, end));
  }
  if (src[i] !== '(') return null;
  const out = [];
  let depth = 0;
  for (i++; i < src.length; i++) {
    const c = src[i];
    if (c === '\\') {
      const n = src[++i];
      const map = { n: 10, r: 13, t: 9, b: 8, f: 12, '(': 40, ')': 41, '\\': 92 };
      if (n in map) out.push(map[n]);
      else if (n >= '0' && n <= '7') {
        let oct = n;
        while (oct.length < 3 && src[i + 1] >= '0' && src[i + 1] <= '7') oct += src[++i];
        out.push(parseInt(oct, 8) & 255);
      } else out.push(n.charCodeAt(0) & 255);
    } else if (c === '(') { depth++; out.push(40); }
    else if (c === ')') { if (!depth) break; depth--; out.push(41); }
    else out.push(c.charCodeAt(0) & 255);
  }
  return new Uint8Array(out);
}

/** → null when unencrypted, else the parsed /Encrypt parameters. */
export function detectPdfEncryption(buffer) {
  const bytes = new Uint8Array(buffer);
  const src = latin1(bytes);
  if (!/\/Encrypt\s+\d+\s+\d+\s+R/.test(src) && !/\/Filter\s*\/Standard/.test(src)) return null;

  const encRef = /\/Encrypt\s+(\d+)\s+(\d+)\s+R/.exec(src);
  const dictStart = src.search(/\/Filter\s*\/Standard/);
  if (dictStart === -1) return { unsupported: 'no_standard_handler' };
  const dict = src.slice(dictStart, dictStart + 1200);

  const num = (re, dflt) => { const m = re.exec(dict); return m ? parseInt(m[1], 10) : dflt; };
  const V = num(/\/V\s+(\d+)/, 0);
  const R = num(/\/R\s+(\d+)/, 0);
  const Length = num(/\/Length\s+(\d+)/, 40);
  let P = num(/\/P\s+(-?\d+)/, -1);
  if (P > 2147483647) P -= 4294967296;                  // stored unsigned, used signed

  if (V >= 4 || /AESV[23]/.test(dict)) {
    return { unsupported: 'aes', V, R };                // AES is block-padded: not length-preserving
  }

  const O = readPdfString(dict, dict.search(/\/O\s*[<(]/) + 2);
  const U = readPdfString(dict, dict.search(/\/U\s*[<(]/) + 2);

  // First element of the trailer /ID array feeds the key derivation.
  const idMatch = /\/ID\s*\[\s*([<(][^>)]*[>)])/.exec(src);
  const id0 = idMatch ? readPdfString(idMatch[1], 0) : new Uint8Array(0);

  const encMeta = !/\/EncryptMetadata\s+false/.test(dict);
  return { V, R, Length, P, O, U, id0, encMeta,
           encryptRef: encRef ? { num: +encRef[1], gen: +encRef[2], raw: encRef[0] } : null };
}

/** Algorithm 2: derive the file encryption key from the user password. */
export function computeKey(password, enc) {
  const pw = toBytes(String(password ?? ''));
  const padded = new Uint8Array(32);
  padded.set(pw.slice(0, 32));
  if (pw.length < 32) padded.set(PAD.slice(0, 32 - pw.length), pw.length);

  const n = enc.R === 2 ? 5 : Math.max(5, Math.floor(enc.Length / 8));
  const pBytes = new Uint8Array(4);
  new DataView(pBytes.buffer).setInt32(0, enc.P, true);

  const parts = [padded, enc.O.slice(0, 32), pBytes, enc.id0];
  if (enc.R >= 4 && !enc.encMeta) parts.push(new Uint8Array([255, 255, 255, 255]));

  const total = parts.reduce((a, p) => a + p.length, 0);
  const buf = new Uint8Array(total);
  let off = 0;
  for (const p of parts) { buf.set(p, off); off += p.length; }

  let hash = md5(buf);
  if (enc.R >= 3) for (let i = 0; i < 50; i++) hash = md5(hash.slice(0, n));
  return hash.slice(0, n);
}

/** Algorithm 1: per-object key = MD5(fileKey ‖ objLow3 ‖ genLow2), truncated. */
function objectKey(fileKey, num, gen) {
  const ext = new Uint8Array(fileKey.length + 5);
  ext.set(fileKey);
  ext[fileKey.length] = num & 255;
  ext[fileKey.length + 1] = (num >> 8) & 255;
  ext[fileKey.length + 2] = (num >> 16) & 255;
  ext[fileKey.length + 3] = gen & 255;
  ext[fileKey.length + 4] = (gen >> 8) & 255;
  return md5(ext).slice(0, Math.min(fileKey.length + 5, 16));
}

/**
 * Verify the user password (Algorithm 6 for R2, Algorithm 7 for R>=3).
 * R2: RC4(key, PAD) must equal /U exactly.
 * R>=3: only the first 16 bytes are defined — the rest is arbitrary padding.
 */
export function checkPassword(fileKey, enc) {
  if (enc.R === 2) {
    const test = rc4(fileKey, PAD);
    return test.every((b, i) => b === enc.U[i]);
  }
  const idPart = new Uint8Array(PAD.length + enc.id0.length);
  idPart.set(PAD);
  idPart.set(enc.id0, PAD.length);
  let test = md5(idPart);
  test = rc4(fileKey, test);
  for (let i = 1; i <= 19; i++) {
    const k = new Uint8Array(fileKey.length);
    for (let j = 0; j < fileKey.length; j++) k[j] = fileKey[j] ^ i;
    test = rc4(k, test);
  }
  for (let i = 0; i < 16; i++) if (test[i] !== enc.U[i]) return false;
  return true;
}

/**
 * Decrypt every stream in place and neutralise the /Encrypt reference.
 * Returns a NEW Uint8Array of identical length; the input is not mutated.
 */
export function decryptPdf(buffer, password) {
  const enc = detectPdfEncryption(buffer);
  if (!enc) return { ok: true, unchanged: true, bytes: new Uint8Array(buffer) };
  if (enc.unsupported) return { ok: false, error: `unsupported_encryption:${enc.unsupported}` };

  const fileKey = computeKey(password, enc);
  if (!checkPassword(fileKey, enc)) return { ok: false, error: 'wrong_password' };

  const out = new Uint8Array(buffer.byteLength);
  out.set(new Uint8Array(buffer));
  const src = latin1(out);

  // Walk "N G obj … stream\r?\n <bytes> endstream". Offsets are preserved throughout,
  // so a single forward pass over the original text stays valid.
  const objRe = /(\d+)\s+(\d+)\s+obj\b/g;
  let m, streams = 0;
  while ((m = objRe.exec(src)) !== null) {
    const num = +m[1], gen = +m[2];
    const objEnd = src.indexOf('endobj', m.index);
    const sIdx = src.indexOf('stream', m.index);
    if (sIdx === -1 || (objEnd !== -1 && sIdx > objEnd)) continue;

    let dataStart = sIdx + 6;
    if (src[dataStart] === '\r') dataStart++;
    if (src[dataStart] === '\n') dataStart++;
    const endIdx = src.indexOf('endstream', dataStart);
    if (endIdx === -1) continue;

    let dataEnd = endIdx;
    if (src[dataEnd - 1] === '\n') dataEnd--;
    if (src[dataEnd - 1] === '\r') dataEnd--;
    if (dataEnd <= dataStart) continue;

    // The /Encrypt dictionary itself is never encrypted — skip it.
    if (enc.encryptRef && num === enc.encryptRef.num && gen === enc.encryptRef.gen) continue;

    const slice = out.subarray(dataStart, dataEnd);
    out.set(rc4(objectKey(fileKey, num, gen), slice), dataStart);
    streams++;
  }

  // Blank "/Encrypt N 0 R" with the same number of spaces so nothing shifts.
  if (enc.encryptRef) {
    let at = -1;
    while ((at = src.indexOf(enc.encryptRef.raw, at + 1)) !== -1) {
      for (let i = 0; i < enc.encryptRef.raw.length; i++) out[at + i] = 0x20;
    }
  }
  return { ok: true, bytes: out, streams, bits: fileKey.length * 8, R: enc.R, V: enc.V };
}
