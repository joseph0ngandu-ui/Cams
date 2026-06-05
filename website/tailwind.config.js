/** @type {import('tailwindcss').Config} */
export default {
  darkMode: 'class',
  content: ['./**/*.html', './src/**/*.{js,ts}'],
  theme: {
    extend: {
      colors: {
        /* === Surface Stack === */
        'surface-lowest': '#0e0e0e',
        'surface-dim':    '#131313',
        'surface':        '#131313',
        'surface-low':    '#1c1b1b',
        'surface-container': '#201f1f',
        'surface-high':   '#2a2a2a',
        'surface-highest':'#353534',
        'surface-bright': '#3a3939',

        /* === On-surface === */
        'on-surface':         '#e5e2e1',
        'on-surface-variant': '#c4c7c8',

        /* === Borders === */
        'outline':         '#8e9192',
        'outline-variant': '#444748',

        /* === Primary (white) === */
        'primary':    '#ffffff',
        'on-primary': '#2f3131',

        /* === Emerald — linked / live / success === */
        'emerald':       '#00e639',
        'emerald-dim':   '#00e475',
        'emerald-glow':  'rgba(0, 230, 57, 0.6)',

        /* === Garnet — stop / record / active stream === */
        'garnet':        '#d73b00',
        'garnet-soft':   '#ffb5a0',
        'garnet-deep':   '#3b0900',

        /* === Amber — warning / network pressure === */
        'amber':         '#feb300',
        'amber-soft':    '#ffd799',
        'amber-deep':    '#6a4800',

        /* === Error / Thermal Critical === */
        'error':              '#ffb4ab',
        'error-container':    '#93000a',
        'on-error-container': '#ffdad6',
      },

      fontFamily: {
        display: ['Hanken Grotesk', 'system-ui', 'sans-serif'],
        body:    ['Hanken Grotesk', 'system-ui', 'sans-serif'],
        mono:    ['JetBrains Mono', 'ui-monospace', 'Menlo', 'monospace'],
      },

      fontSize: {
        'display-2xl': ['72px',  { lineHeight: '1.0',  letterSpacing: '-0.03em', fontWeight: '700' }],
        'display-xl':  ['56px',  { lineHeight: '1.05', letterSpacing: '-0.03em', fontWeight: '700' }],
        'display-lg':  ['48px',  { lineHeight: '1.1',  letterSpacing: '-0.02em', fontWeight: '700' }],
        'display-md':  ['36px',  { lineHeight: '1.1',  letterSpacing: '-0.02em', fontWeight: '700' }],
        'display-sm':  ['28px',  { lineHeight: '1.15', letterSpacing: '-0.02em', fontWeight: '700' }],
        'headline-lg': ['24px',  { lineHeight: '1.2',  letterSpacing: '-0.01em', fontWeight: '600' }],
        'headline-md': ['20px',  { lineHeight: '1.3',  letterSpacing: '-0.01em', fontWeight: '600' }],
        'body-lg':     ['16px',  { lineHeight: '1.6',  letterSpacing: '0',       fontWeight: '400' }],
        'body-md':     ['14px',  { lineHeight: '1.5',  letterSpacing: '0',       fontWeight: '400' }],
        'body-sm':     ['13px',  { lineHeight: '1.4',  letterSpacing: '0',       fontWeight: '400' }],
        'mono-lg':     ['14px',  { lineHeight: '1.4',  letterSpacing: '0.01em',  fontWeight: '500' }],
        'mono-md':     ['13px',  { lineHeight: '1.2',  letterSpacing: '-0.01em', fontWeight: '500' }],
        'mono-sm':     ['11px',  { lineHeight: '1',    letterSpacing: '0',       fontWeight: '400' }],
        'label-caps':  ['10px',  { lineHeight: '1',    letterSpacing: '0.08em',  fontWeight: '700' }],
        'label-caps-md':['11px', { lineHeight: '1',    letterSpacing: '0.06em',  fontWeight: '700' }],
        'label-caps-lg':['12px', { lineHeight: '1',    letterSpacing: '0.06em',  fontWeight: '700' }],
      },

      borderRadius: {
        DEFAULT: '0.125rem',  /* 2px — industrial */
        sm:     '0.125rem',
        md:     '0.25rem',    /* 4px */
        lg:     '0.5rem',     /* 8px */
        xl:     '0.75rem',    /* 12px */
        '2xl':  '1rem',
        full:   '9999px',
      },

      spacing: {
        unit:   '4px',
        module: '8px',
        gutter: '12px',
        margin: '16px',
        32:     '32px',
      },

      boxShadow: {
        'led-emerald': '0 0 8px 1px rgba(0, 230, 57, 0.7)',
        'led-amber':   '0 0 8px 1px rgba(254, 179, 0, 0.7)',
        'led-garnet':  '0 0 8px 1px rgba(215, 59, 0, 0.7)',
        'led-error':   '0 0 8px 1px rgba(255, 180, 171, 0.5)',
        'inset-well':  'inset 0 2px 4px rgba(0, 0, 0, 0.6)',
        'milled':      '0 0.5px 0 rgba(255,255,255,0.06), 0 -0.5px 0 rgba(0,0,0,0.4)',
        'panel-up':    '0 -1px 0 rgba(255, 255, 255, 0.04)',
        'data-wave':   'inset 0 0 0 1px rgba(0, 230, 57, 0.25), inset 0 0 20px rgba(0, 230, 57, 0.1)',
      },

      keyframes: {
        'led-pulse': {
          '0%, 100%': { opacity: '1', boxShadow: '0 0 4px 1px rgba(0, 230, 57, 0.5)' },
          '50%':      { opacity: '0.7', boxShadow: '0 0 10px 2px rgba(0, 230, 57, 0.9)' },
        },
        'amber-pulse': {
          '0%, 100%': { opacity: '1', boxShadow: '0 0 4px 1px rgba(254, 179, 0, 0.5)' },
          '50%':      { opacity: '0.7', boxShadow: '0 0 10px 2px rgba(254, 179, 0, 0.9)' },
        },
        'garnet-pulse': {
          '0%, 100%': { opacity: '1', boxShadow: '0 0 4px 1px rgba(215, 59, 0, 0.5)' },
          '50%':      { opacity: '0.7', boxShadow: '0 0 10px 2px rgba(215, 59, 0, 0.9)' },
        },
        'error-blink': {
          '0%, 100%': { opacity: '1' },
          '50%':      { opacity: '0.2' },
        },
        'data-wave': {
          '0%':   { boxShadow: 'inset 0 0 0 1px rgba(0,230,57,0.08)' },
          '50%':  { boxShadow: 'inset 0 0 0 1px rgba(0,230,57,0.35), inset 0 0 24px rgba(0,230,57,0.12)' },
          '100%': { boxShadow: 'inset 0 0 0 1px rgba(0,230,57,0.08)' },
        },
        'scanline': {
          '0%':   { transform: 'translateY(-100%)' },
          '100%': { transform: 'translateY(100vh)' },
        },
        'slide-up': {
          from: { transform: 'translateY(12px)', opacity: '0' },
          to:   { transform: 'translateY(0)',    opacity: '1' },
        },
        'slide-down': {
          from: { transform: 'translateY(-12px)', opacity: '0' },
          to:   { transform: 'translateY(0)',     opacity: '1' },
        },
        'fade-in': {
          from: { opacity: '0' },
          to:   { opacity: '1' },
        },
        'number-tick': {
          '0%':   { transform: 'translateY(-4px)', opacity: '0' },
          '15%':  { transform: 'translateY(0)',    opacity: '1' },
          '85%':  { transform: 'translateY(0)',    opacity: '1' },
          '100%': { transform: 'translateY(4px)',  opacity: '0' },
        },
        'state-idle': {
          '0%, 28%':  { opacity: '1' },
          '33%, 95%': { opacity: '0' },
          '100%':     { opacity: '1' },
        },
        'state-streaming': {
          '0%, 28%':   { opacity: '0' },
          '33%, 61%':  { opacity: '1' },
          '66%, 100%': { opacity: '0' },
        },
        'state-linked': {
          '0%, 61%':   { opacity: '0' },
          '66%, 94%':  { opacity: '1' },
          '99%, 100%': { opacity: '0' },
        },
      },

      animation: {
        'led-pulse':    'led-pulse 2s ease-in-out infinite',
        'amber-pulse':  'amber-pulse 2s ease-in-out infinite',
        'garnet-pulse': 'garnet-pulse 1.5s ease-in-out infinite',
        'error-blink':  'error-blink 1s ease-in-out infinite',
        'data-wave':    'data-wave 2.5s ease-in-out infinite',
        'scanline':     'scanline 10s linear infinite',
        'slide-up':     'slide-up 0.3s cubic-bezier(0.16, 1, 0.3, 1) forwards',
        'slide-down':   'slide-down 0.3s cubic-bezier(0.16, 1, 0.3, 1) forwards',
        'fade-in':      'fade-in 0.4s ease-out forwards',
        'state-idle':      'state-idle 9s ease-in-out infinite',
        'state-streaming': 'state-streaming 9s ease-in-out infinite',
        'state-linked':    'state-linked 9s ease-in-out infinite',
      },
    },
  },
  plugins: [],
}
