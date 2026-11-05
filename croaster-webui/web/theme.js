// Croaster web UI: colour schemes and style options. Pure functions plus a small
// browser part that applies the saved scheme before the page paints. Loaded by
// index.html in <head> and by the host tests (module.exports).
(function (root, factory) {
  const T = factory();
  if (typeof module === 'object' && module.exports) module.exports = T;
  else root.Theme = T;
  if (typeof document !== 'undefined' && typeof localStorage !== 'undefined') {
    let raw = null;
    try { raw = localStorage.getItem(T.STORAGE_KEY); } catch (_) { /* storage blocked: defaults */ }
    T.apply(T.migrate(raw).theme, document.documentElement);
  }
})(typeof self !== 'undefined' ? self : this, function () {
  'use strict';

  const STORAGE_KEY = 'croaster.theme';
  const FORMAT = 'croaster-theme';
  const VERSION = 1;
  const DEFAULT_PRESET = 'neon';

  // Every colour the page uses, as CSS custom properties (--<key>).
  const TOKENS = [
    { key: 'bg', label: 'Background', group: 'Interface' },
    { key: 'panel', label: 'Surface (cards)', group: 'Interface' },
    { key: 'panel-2', label: 'Elevated surface (buttons, inputs)', group: 'Interface' },
    { key: 'line', label: 'Border', group: 'Interface' },
    { key: 'grid', label: 'Grid lines', group: 'Interface' },
    { key: 'text', label: 'Text', group: 'Interface' },
    { key: 'muted', label: 'Secondary text', group: 'Interface' },
    { key: 'accent', label: 'Accent', group: 'Interface' },
    { key: 'on-accent', label: 'Text on accent', group: 'Interface' },
    { key: 'stop', label: 'Danger / trip / STOP', group: 'Status' },
    { key: 'on-danger', label: 'Text on danger', group: 'Status' },
    { key: 'ok', label: 'Success / connected', group: 'Status' },
    { key: 'warn', label: 'Warning / pending', group: 'Status' },
    { key: 'bt', label: 'BT', group: 'Chart' },
    { key: 'et', label: 'ET', group: 'Chart' },
    { key: 'ror', label: 'RoR (BT)', group: 'Chart' },
    { key: 'ror-et', label: 'RoR (ET)', group: 'Chart' },
    { key: 'heat', label: 'Heater', group: 'Chart' },
    { key: 'fan', label: 'Exhaust Fan', group: 'Chart' },
    { key: 'sv', label: 'Setpoint', group: 'Chart' },
    { key: 'event', label: 'Event markers', group: 'Chart' },
  ];

  // Style options. Numbers are clamped to [min, max] and rounded to step.
  const STYLE_FIELDS = {
    radius: { label: 'Corner radius', type: 'number', min: 0, max: 24, step: 1, unit: 'px' },
    scale: { label: 'Text size', type: 'number', min: 80, max: 140, step: 5, unit: '%' },
    density: { label: 'Density', type: 'choice', options: ['comfortable', 'compact'] },
    lineWidth: { label: 'Chart line width', type: 'number', min: 1, max: 4, step: 0.5, unit: 'px' },
    numerals: { label: 'Numerals', type: 'choice', options: ['mono', 'sans'] },
    glass: { label: 'Card opacity', type: 'number', min: 40, max: 100, step: 5, unit: '%' },
    glow: { label: 'Glow', type: 'number', min: 0, max: 100, step: 5, unit: '%' },
    reducedMotion: { label: 'Reduce motion', type: 'bool' },
  };

  const BASE_STYLE = { radius: 10, scale: 100, density: 'comfortable', lineWidth: 2, numerals: 'mono',
    glass: 100, glow: 0, reducedMotion: false };

  const PRESETS = {
    neon: {
      label: 'Neon (default)',
      colors: {
        bg: '#0b0c0a', panel: '#161a14', 'panel-2': '#1f251c', line: '#2e3b2a', grid: '#1b2219',
        text: '#ecf3e8', muted: '#9aab93', accent: '#4cf27c', 'on-accent': '#04150a',
        stop: '#d42a36', 'on-danger': '#ffffff', ok: '#4cf27c', warn: '#facc15',
        bt: '#4cf27c', et: '#38bdf8', ror: '#fbbf24', 'ror-et': '#f472b6', heat: '#ff6b4a', fan: '#a78bfa',
        sv: '#e2e8f0', event: '#b9c8b2',
      },
      style: { radius: 12, glass: 75, glow: 60 },
    },
    copper: {
      label: 'Copper dark',
      colors: {
        bg: '#0f0d0c', panel: '#191513', 'panel-2': '#221c19', line: '#3a302a', grid: '#2a2320',
        text: '#f1e8e1', muted: '#a39283', accent: '#e07b39', 'on-accent': '#140b06',
        stop: '#d63a2f', 'on-danger': '#ffffff', ok: '#5cc27a', warn: '#f0b429',
        bt: '#e07b39', et: '#5b8fc7', ror: '#5cc27a', 'ror-et': '#3f8f57', heat: '#e0533a', fan: '#7aa6d6',
        sv: '#c9b8a8', event: '#a39283',
      },
      style: {},
    },
    light: {
      label: 'Light',
      colors: {
        bg: '#f6f2ee', panel: '#ffffff', 'panel-2': '#f1ebe5', line: '#ddd2c8', grid: '#e9e1d9',
        text: '#241b15', muted: '#6f6054', accent: '#c0601f', 'on-accent': '#120a05',
        stop: '#c62f24', 'on-danger': '#ffffff', ok: '#2f9a4f', warn: '#b7800a',
        bt: '#c0601f', et: '#2f6aa8', ror: '#2f9a4f', 'ror-et': '#5fb87a', heat: '#c23b24', fan: '#4a7fb8',
        sv: '#5c5047', event: '#76675b',
      },
      style: {},
    },
    contrast: {
      label: 'High contrast',
      colors: {
        bg: '#000000', panel: '#0a0a0a', 'panel-2': '#1a1a1a', line: '#ffffff', grid: '#4d4d4d',
        text: '#ffffff', muted: '#e6e6e6', accent: '#ffd400', 'on-accent': '#000000',
        stop: '#ff3b30', 'on-danger': '#000000', ok: '#00e676', warn: '#ffd400',
        bt: '#ff9f0a', et: '#40c4ff', ror: '#00e676', 'ror-et': '#eeff41', heat: '#ff5252', fan: '#b388ff',
        sv: '#ffffff', event: '#ffffff',
      },
      style: { radius: 4, lineWidth: 3 },
    },
    graphite: {
      label: 'Graphite',
      colors: {
        bg: '#121417', panel: '#1b1e23', 'panel-2': '#252a31', line: '#363c45', grid: '#262b32',
        text: '#e6e9ee', muted: '#9aa3ad', accent: '#6ea8fe', 'on-accent': '#0b1220',
        stop: '#d13438', 'on-danger': '#ffffff', ok: '#46a758', warn: '#f5d90a',
        bt: '#f5a524', et: '#6ea8fe', ror: '#46a758', 'ror-et': '#8bd59a', heat: '#e5484d', fan: '#a5b4fc',
        sv: '#cbd5e1', event: '#9aa3ad',
      },
      style: { radius: 8 },
    },
  };

  // Text pairs that must stay readable. Surfaces are compared as drawn, with card
  // opacity blended over the background.
  const CONTRAST_PAIRS = [
    { fg: 'text', bg: 'bg', label: 'Text on background' },
    { fg: 'text', bg: 'panel', label: 'Text on cards' },
    { fg: 'text', bg: 'panel-2', label: 'Text on buttons and inputs' },
    { fg: 'muted', bg: 'panel', label: 'Secondary text on cards' },
    { fg: 'on-accent', bg: 'accent', label: 'Text on accent' },
    { fg: 'on-danger', bg: 'stop', label: 'Text on STOP and trip banner' },
  ];
  const AA_MIN = 4.5;

  const copy = (o) => JSON.parse(JSON.stringify(o));

  // "#abc", "abc", "#AABBCC" -> "#aabbcc"; anything else -> null.
  function normalizeHex(value) {
    if (typeof value !== 'string') return null;
    let h = value.trim().toLowerCase();
    if (h[0] === '#') h = h.slice(1);
    if (/^[0-9a-f]{3}$/.test(h)) h = h[0] + h[0] + h[1] + h[1] + h[2] + h[2];
    return /^[0-9a-f]{6}$/.test(h) ? '#' + h : null;
  }

  function fromPreset(name) {
    const p = PRESETS[name] || PRESETS[DEFAULT_PRESET];
    return { preset: PRESETS[name] ? name : DEFAULT_PRESET, colors: copy(p.colors),
      style: Object.assign({}, BASE_STYLE, p.style) };
  }

  function cleanStyle(key, value) {
    const f = STYLE_FIELDS[key];
    if (f.type === 'bool') return typeof value === 'boolean' ? value : null;
    if (f.type === 'choice') return f.options.includes(value) ? value : null;
    const n = typeof value === 'number' ? value : typeof value === 'string' && value.trim() ? Number(value) : NaN;
    if (!isFinite(n)) return null;
    const c = Math.min(f.max, Math.max(f.min, Math.round(n / f.step) * f.step));
    return Math.round(c * 100) / 100;
  }

  // Any object with optional preset, colors and style -> a complete theme. Missing
  // values come from the named preset (or the default); invalid ones are reported
  // and replaced the same way.
  function normalize(input) {
    const errors = [];
    if (!input || typeof input !== 'object' || Array.isArray(input)) {
      return { theme: fromPreset(DEFAULT_PRESET), errors: ['not a theme object'] };
    }
    const theme = fromPreset(input.preset);
    if (input.preset !== undefined && !PRESETS[input.preset]) errors.push('unknown preset "' + input.preset + '"');
    const colors = input.colors && typeof input.colors === 'object' ? input.colors : {};
    for (const t of TOKENS) {
      if (colors[t.key] === undefined) continue;
      const hex = normalizeHex(colors[t.key]);
      if (hex) theme.colors[t.key] = hex;
      else errors.push(t.label + ': "' + colors[t.key] + '" is not a #rrggbb colour');
    }
    const style = input.style && typeof input.style === 'object' ? input.style : {};
    for (const key of Object.keys(STYLE_FIELDS)) {
      if (style[key] === undefined) continue;
      const v = cleanStyle(key, style[key]);
      if (v === null) errors.push(STYLE_FIELDS[key].label + ': invalid value');
      else theme.style[key] = v;
    }
    return { theme, errors };
  }

  // Value stored under croaster.theme -> theme. Before schemes existed the key held
  // "dark" or "light". "dark" was the only dark option, so it moves to the current
  // default; "light" keeps the light preset.
  function migrate(raw) {
    if (raw === null || raw === undefined || raw === '' || raw === 'dark') {
      return { theme: fromPreset(DEFAULT_PRESET), migrated: raw === 'dark', errors: [] };
    }
    if (raw === 'light') return { theme: fromPreset('light'), migrated: true, errors: [] };
    let obj;
    try { obj = JSON.parse(raw); } catch (_) {
      return { theme: fromPreset(DEFAULT_PRESET), migrated: true, errors: ['saved theme is not JSON'] };
    }
    const r = normalize(obj);
    return { theme: r.theme, migrated: false, errors: r.errors };
  }

  // Text from an export file or a paste -> { theme, errors }; theme is null when
  // the text is not a theme at all.
  function parseImport(text) {
    let obj;
    try { obj = JSON.parse(text); } catch (_) { return { theme: null, errors: ['not valid JSON'] }; }
    if (!obj || typeof obj !== 'object' || Array.isArray(obj) || (!obj.colors && !obj.style && !obj.preset)) {
      return { theme: null, errors: ['no colors, style or preset in this JSON'] };
    }
    if (obj.format !== undefined && obj.format !== FORMAT) return { theme: null, errors: ['not a Croaster theme'] };
    if (obj.v !== undefined && obj.v > VERSION) return { theme: null, errors: ['theme version ' + obj.v + ' is newer than this page'] };
    return normalize(obj);
  }

  function serialize(theme, pretty) {
    return JSON.stringify({ format: FORMAT, v: VERSION, preset: theme.preset, colors: theme.colors,
      style: theme.style }, null, pretty ? 2 : 0);
  }

  function rgb(hex) {
    const n = parseInt(hex.slice(1), 16);
    return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
  }
  function toHex(c) {
    return '#' + c.map((v) => Math.round(v).toString(16).padStart(2, '0')).join('');
  }
  // fg drawn at alpha (0..1) over bg.
  function blend(fg, bg, alpha) {
    const a = rgb(fg), b = rgb(bg);
    return toHex(a.map((v, i) => v * alpha + b[i] * (1 - alpha)));
  }
  // WCAG 2 relative luminance.
  function luminance(hex) {
    const ch = rgb(hex).map((v) => {
      const s = v / 255;
      return s <= 0.03928 ? s / 12.92 : Math.pow((s + 0.055) / 1.055, 2.4);
    });
    return 0.2126 * ch[0] + 0.7152 * ch[1] + 0.0722 * ch[2];
  }
  function contrast(a, b) {
    const la = luminance(a), lb = luminance(b);
    return (Math.max(la, lb) + 0.05) / (Math.min(la, lb) + 0.05);
  }

  // Pairs below WCAG AA (4.5:1), as { label, fg, bg, ratio }. Advisory only.
  function contrastWarnings(theme) {
    const c = theme.colors;
    const alpha = theme.style.glass / 100;
    const surface = (key) => (key === 'panel' ? blend(c.panel, c.bg, alpha) : c[key]);
    const out = [];
    for (const p of CONTRAST_PAIRS) {
      const ratio = contrast(c[p.fg], surface(p.bg));
      if (ratio < AA_MIN) out.push({ label: p.label, fg: p.fg, bg: p.bg, ratio: Math.round(ratio * 100) / 100 });
    }
    return out;
  }

  // CSS custom properties and root attributes for a theme.
  function cssVars(theme) {
    const s = theme.style;
    const vars = {};
    for (const t of TOKENS) vars['--' + t.key] = theme.colors[t.key];
    vars['--radius'] = s.radius + 'px';
    vars['--radius-sm'] = Math.round(s.radius * 0.7) + 'px';
    vars['--font-scale'] = String(s.scale / 100);
    vars['--line-width'] = String(s.lineWidth);
    vars['--panel-alpha'] = s.glass + '%';
    vars['--glow'] = String(s.glow / 100);
    vars['--num-font'] = s.numerals === 'sans' ? 'var(--font)' : 'var(--mono)';
    return {
      vars,
      attrs: { density: s.density, motion: s.reducedMotion ? 'reduce' : 'normal' },
      scheme: luminance(theme.colors.bg) < 0.4 ? 'dark' : 'light',
    };
  }

  function apply(theme, el) {
    const css = cssVars(theme);
    for (const k of Object.keys(css.vars)) el.style.setProperty(k, css.vars[k]);
    el.dataset.density = css.attrs.density;
    el.dataset.motion = css.attrs.motion;
    el.style.colorScheme = css.scheme;
    const meta = typeof document !== 'undefined' && document.querySelector('meta[name="theme-color"]');
    if (meta) meta.setAttribute('content', theme.colors.bg);
  }

  return { STORAGE_KEY, FORMAT, VERSION, DEFAULT_PRESET, TOKENS, STYLE_FIELDS, BASE_STYLE, PRESETS,
    CONTRAST_PAIRS, AA_MIN, normalizeHex, fromPreset, normalize, migrate, parseImport, serialize,
    blend, luminance, contrast, contrastWarnings, cssVars, apply };
});
