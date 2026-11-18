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
  constructor(canvas, cursorEl, send, { onToggleHud }) {
    this.canvas = canvas;
    this.cursor = cursorEl;
    this.send = send;
    this.onToggleHud = onToggleHud;
    this.locked = false;
    this.mode = 'abs';
    this.accX = 0;
    this.accY = 0;
    this.rafPending = false;
    this.vx = 0.5;   // 指针锁定时的虚拟光标位置（归一化）
    this.vy = 0.5;
    this.#bind();
  }

  #bind() {
    const c = this.canvas;

    c.addEventListener('click', () => {
      if (document.pointerLockElement !== c) c.requestPointerLock?.();
    });

    document.addEventListener('pointerlockchange', () => {
      this.locked = document.pointerLockElement === c;
      // 锁定后系统指针被隐藏，改为用自绘光标跟踪「虚拟位置」，保证始终可见
      this.cursor.classList.remove('hidden');
      if (this.locked) {
        // 锁定瞬间先做一次绝对定位对齐，否则本地基准与远端光标不一致，指针会整体偏移
        this.send({ t: 'pm', x: this.vx, y: this.vy });
        this.accX = 0;
        this.accY = 0;
      }
    });

    window.addEventListener('keydown', (e) => {
      if (e.ctrlKey && e.altKey && e.code === 'F1') {
        e.preventDefault();
        this.onToggleHud?.();
        return;
      }
      const k = KEYMAP[e.code];
      if (k === undefined) return;
      e.preventDefault();
      this.send({ t: 'kk', k, d: 1 });
    });

    window.addEventListener('keyup', (e) => {
      const k = KEYMAP[e.code];
      if (k === undefined) return;
      e.preventDefault();
      this.send({ t: 'kk', k, d: 0 });
    });

    window.addEventListener('mousemove', (e) => {
      if (this.locked) {
        // 指针锁定：按帧合并相对位移，避免高 DPI 鼠标刷爆控制流
        this.accX += e.movementX;
        this.accY += e.movementY;

        // 锁定期间 clientX 固定，用累积位移推进自绘光标的虚拟位置
        this.vx = Math.min(1, Math.max(0, this.vx + e.movementX / window.innerWidth));
        this.vy = Math.min(1, Math.max(0, this.vy + e.movementY / window.innerHeight));
        this.cursor.style.transform =
          `translate3d(${this.vx * window.innerWidth}px, ${this.vy * window.innerHeight}px, 0)`;
        this.cursor.classList.remove('hidden');

        if (!this.rafPending) {
          this.rafPending = true;
          requestAnimationFrame(() => {
            this.rafPending = false;
            if (this.accX || this.accY) {
              this.send({ t: 'pr', dx: this.accX, dy: this.accY });
              this.accX = 0;
              this.accY = 0;
            }
          });
        }
      } else {
        this.vx = e.clientX / window.innerWidth;
        this.vy = e.clientY / window.innerHeight;
        const r = c.getBoundingClientRect();
        const x = (e.clientX - r.left) / r.width;
        const y = (e.clientY - r.top) / r.height;
        this.cursor.style.transform =
          `translate3d(${e.clientX}px, ${e.clientY}px, 0)`;
        this.cursor.classList.remove('hidden');
        this.send({ t: 'pm', x: Math.min(1, Math.max(0, x)), y: Math.min(1, Math.max(0, y)) });
      }
    });

    c.addEventListener('mousedown', (e) => {
      const b = BTN[e.button];
      if (b) this.send({ t: 'pb', b, d: 1 });
    });

    c.addEventListener('mouseup', (e) => {
      const b = BTN[e.button];
      if (b) this.send({ t: 'pb', b, d: 0 });
    });

    c.addEventListener('contextmenu', (e) => e.preventDefault());

    c.addEventListener('wheel', (e) => {
      e.preventDefault();
      const v = (e.deltaY > 0 ? 1 : -1) * 15;
      this.send({ t: 'pw', v });
    }, { passive: false });
  }
}
