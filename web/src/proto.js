// 与 proto/wire.h 严格对应的协议常量与解析
export const WIRE_VERSION = 1;
export const VID_HDR_SIZE = 24;
export const FRAG_KEY = 0x01;
export const FRAG_FIRST = 0x02;
export const FRAG_LAST = 0x04;

/** 解析一个视频分片头（小端） */
export function parseFrag(buf) {
  const dv = new DataView(buf.buffer, buf.byteOffset, VID_HDR_SIZE);
  return {
    version: dv.getUint8(0),
    flags: dv.getUint8(1),
    streamId: dv.getUint16(2, true),
    frameId: dv.getUint32(4, true),
    fragIndex: dv.getUint16(8, true),
    fragCount: dv.getUint16(10, true),
    payloadLen: dv.getUint32(12, true),
    ptsUs: Number(dv.getBigUint64(16, true)),
  };
}

/* ── 音频（与 proto/wire.h 的 ww_aud_hdr 严格对应）─────────────
   datagram 首字节即媒体类型：1 = 视频分片，2 = 音频包。 */
export const AUDIO_VERSION = 2;
export const AUDIO_HDR_SIZE = 16;
export const AUD_MAIN = 0x01;
export const AUD_RED = 0x02;
export const AUDIO_FRAME_MS = 20;

/** 解析音频包头（小端） */
export function parseAudio(buf) {
  const dv = new DataView(buf.buffer, buf.byteOffset, AUDIO_HDR_SIZE);
  return {
    version: dv.getUint8(0),
    flags: dv.getUint8(1),
    seq: dv.getUint16(2, true),
    len: dv.getUint16(4, true),
    redLen: dv.getUint16(6, true),
    tsMs: dv.getUint32(8, true),
  };
}

/** 控制消息编码：uint32 LE 长度 + UTF-8 JSON */
export function encodeCtrl(obj) {
  const json = new TextEncoder().encode(JSON.stringify(obj));
  const out = new Uint8Array(4 + json.length);
  new DataView(out.buffer).setUint32(0, json.length, true);
  out.set(json, 4);
  return out;
}
