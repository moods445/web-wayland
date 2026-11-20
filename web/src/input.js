// 键鼠捕获 → 远端注入
// KeyboardEvent.code → Linux evdev keycode（虚拟键盘协议要求 evdev 码）
const KEYMAP = {
  Escape: 1, Digit1: 2, Digit2: 3, Digit3: 4, Digit4: 5, Digit5: 6, Digit6: 7, Digit7: 8,
  Digit8: 9, Digit9: 10, Digit0: 11, Minus: 12, Equal: 13, Backspace: 14, Tab: 15,
  KeyQ: 16, KeyW: 17, KeyE: 18, KeyR: 19, KeyT: 20, KeyY: 21, KeyU: 22, KeyI: 23, KeyO: 24,
  KeyP: 25, BracketLeft: 26, BracketRight: 27, Enter: 28, ControlLeft: 29,
  KeyA: 30, KeyS: 31, KeyD: 32, KeyF: 33, KeyG: 34, KeyH: 35, KeyJ: 36, KeyK: 37, KeyL: 38,
  Semicolon: 39, Quote: 40, Backquote: 41, ShiftLeft: 42, Backslash: 43,
  KeyZ: 44, KeyX: 45, KeyC: 46, KeyV: 47, KeyB: 48, KeyN: 49, KeyM: 50,
  Comma: 51, Period: 52, Slash: 53, ShiftRight: 54, NumpadMultiply: 55, AltLeft: 56,
  Space: 57, CapsLock: 58, F1: 59, F2: 60, F3: 61, F4: 62, F5: 63, F6: 64, F7: 65, F8: 66,
  F9: 67, F10: 68, NumLock: 69, ScrollLock: 70, F11: 87, F12: 88,
  ControlRight: 97, NumpadDivide: 98, AltRight: 100, Home: 102, ArrowUp: 103, PageUp: 104,
  ArrowLeft: 105, ArrowRight: 106, End: 107, ArrowDown: 108, PageDown: 109, Insert: 110,
  Delete: 111, MetaLeft: 125, MetaRight: 126,
};

const BTN = { 0: 272, 1: 274, 2: 273 }; // left / middle / right (Linux BTN_*)

export class Input {
  /* 不要在这里画自绘光标：wwcap 的 screencopy 已经把 compositor 光标
     一起编进视频帧（Hyprland 会忽略 overlay_cursor=0），本地再画一支
     就是屏幕上同时出现两个鼠标。这里只负责把事件转成注入消息。 */
  constructor(canvas, send, { onToggleStats }) {
    this.canvas = canvas;
    this.send = send;
    this.onToggleStats = onToggleStats;
    this.locked = false;
    this.rafPending = false;
    this.rect = null;
    // 绝对模式：每帧只发最新位置，避免高频 pointermove 把可靠控制流刷爆
    this.absX = null;
    this.absY = null;
    // 相对模式（?lock=1）累积位移
    this.relX = 0;
    this.relY = 0;
    this.wheel = 0;
    // 已按下的键/按钮，失焦或断线时补发释放，避免远端卡住
    this.keysDown = new Set();
    this.btnsDown = new Set();
    this.#bind();
  }

  #rect() {
    if (!this.rect) this.rect = this.canvas.getBoundingClientRect();
    return this.rect;
  }

  /* 记录归一化坐标；画布外（信箱黑边、HUD、设置面板）返回 false，
     不移动远端指针：以前会 clamp 到 0/1，指针被钉在边缘像在乱飘 */
  #setAbs(e) {
    const r = this.#rect();
    if (!r.width || !r.height) return false;
    const x = (e.clientX - r.left) / r.width;
    const y = (e.clientY - r.top) / r.height;
    if (x < 0 || x > 1 || y < 0 || y > 1) return false;
    this.absX = x;
    this.absY = y;
    return true;
  }

  #flush() {
    this.rafPending = false;
    if (this.absX !== null && !this.locked) {
      this.send({ t: 'pm', x: this.absX, y: this.absY });
      this.absX = null;
      this.absY = null;
    }
    if (this.locked && (this.relX || this.relY)) {
      this.send({ t: 'pr', dx: this.relX, dy: this.relY });
      this.relX = 0;
      this.relY = 0;
    }
    if (this.wheel) {
      this.send({ t: 'pw', v: this.wheel });
      this.wheel = 0;
    }
  }

  #schedule() {
    if (this.rafPending) return;
    this.rafPending = true;
    requestAnimationFrame(() => this.#flush());
  }

  /* 释放所有按下的键鼠：切标签页/刷新/断线后远端不会残留 Shift/Super/Ctrl */
  releaseAll() {
    for (const k of this.keysDown) this.send({ t: 'kk', k, d: 0 });
    for (const b of this.btnsDown) this.send({ t: 'pb', b, d: 0 });
    this.keysDown.clear();
    this.btnsDown.clear();
  }

  destroy() {
    this.releaseAll();
    const h = this.h || {};
    for (const [el, type, fn] of h.list || []) el.removeEventListener(type, fn);
    this.h = null;
  }

  #bind() {
    const c = this.canvas;
    this.h = { list: [] };
    const on = (el, type, fn, opts) => {
      el.addEventListener(type, fn, opts);
      this.h.list.push([el, type, fn]);
    };

    /* 注意：进入串流时**不要**自动请求 Pointer Lock。
       锁定后浏览器只给 movementX/Y，必须靠服务端相对位移换算，极易漂移；
       默认保持绝对定位（点击位置与画面严格对应）。需要相对模式时用 ?lock=1。 */
    on(document, 'pointerlockchange', () => {
      this.locked = document.pointerLockElement === c;
      if (this.locked) {
        // 进入相对模式前用当前绝对位置对齐一次，避免远端光标跳变
        if (this.absX !== null) this.send({ t: 'pm', x: this.absX, y: this.absY });
        this.relX = 0;
        this.relY = 0;
      }
    });

    on(window, 'keydown', (e) => {
      if (e.ctrlKey && e.altKey && e.code === 'F1') {
        e.preventDefault();
        this.onToggleStats?.();
        return;
      }
      const k = KEYMAP[e.code];
      if (k === undefined) return;
      e.preventDefault();
      if (e.repeat) return;          // 远端自行处理长按重复
      this.keysDown.add(k);
      this.send({ t: 'kk', k, d: 1 });
    });

    on(window, 'keyup', (e) => {
      const k = KEYMAP[e.code];
      if (k === undefined) return;
      e.preventDefault();
      this.keysDown.delete(k);
      this.send({ t: 'kk', k, d: 0 });
    });

    on(window, 'mousemove', (e) => {
      if (this.locked) {
        this.relX += e.movementX;
        this.relY += e.movementY;
        this.#schedule();
        return;
      }
      if (this.#setAbs(e)) this.#schedule();
    });

    on(c, 'mousedown', (e) => {
      const b = BTN[e.button];
      if (!b) return;
      /* mousemove 是 rAF 合并发送的：同一帧内“移动后立刻点击”时，
         pb 会先于 pm 到达，点击就落在旧位置。按下前先把坐标刷出去。 */
      this.#setAbs(e);
      this.#flush();
      this.btnsDown.add(b);
      this.send({ t: 'pb', b, d: 1 });
    });

    /* 松开可能发生在画布外/窗口外，绑 window 才不会永久按住左键 */
    on(window, 'mouseup', (e) => {
      const b = BTN[e.button];
      if (!b) return;
      this.btnsDown.delete(b);
      this.send({ t: 'pb', b, d: 0 });
    });

    on(c, 'contextmenu', (e) => e.preventDefault());

    on(c, 'wheel', (e) => {
      e.preventDefault();
      this.wheel += (e.deltaY > 0 ? 1 : -1) * 15;
      this.#schedule();
    }, { passive: false });

    on(window, 'resize', () => { this.rect = null; });
    on(window, 'blur', () => this.releaseAll());
    on(document, 'visibilitychange', () => {
      if (document.visibilityState === 'hidden') this.releaseAll();
    });
    on(window, 'pagehide', () => this.releaseAll());
  }
}
