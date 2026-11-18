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

/** 控制消息编码：uint32 LE 长度 + UTF-8 JSON */
export function encodeCtrl(obj) {
  const json = new TextEncoder().encode(JSON.stringify(obj));
  const out = new Uint8Array(4 + json.length);
  new DataView(out.buffer).setUint32(0, json.length, true);
  out.set(json, 4);
  return out;
}
