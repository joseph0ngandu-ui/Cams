/* ── Shared JS ───────────────────────────────────────────────────────── */

// Mobile nav toggle
export function initNav() {
  const toggle = document.getElementById('nav-toggle')
  const menu = document.getElementById('nav-mobile')
  if (!toggle || !menu) return
  toggle.addEventListener('click', () => {
    const open = menu.classList.toggle('hidden')
    toggle.setAttribute('aria-expanded', String(!open))
  })

  // Close on outside click
  document.addEventListener('click', (e) => {
    if (!menu.contains(e.target) && !toggle.contains(e.target)) {
      menu.classList.add('hidden')
    }
  })
}

// Intersection Observer — fade/slide-up on scroll
export function initScrollAnimations() {
  const els = document.querySelectorAll('[data-animate]')
  if (!els.length) return
  const io = new IntersectionObserver((entries) => {
    entries.forEach(entry => {
      if (entry.isIntersecting) {
        entry.target.classList.add('animate-slide-up')
        entry.target.style.opacity = '1'
        io.unobserve(entry.target)
      }
    })
  }, { threshold: 0.1 })
  els.forEach(el => {
    el.style.opacity = '0'
    io.observe(el)
  })
}

// Simulated telemetry counter — cycles realistic values
export function initTelemetryDemo(containerEl) {
  if (!containerEl) return
  const bitrateEl = containerEl.querySelector('[data-tel="bitrate"]')
  const dropsEl   = containerEl.querySelector('[data-tel="drops"]')
  const thermalEl = containerEl.querySelector('[data-tel="thermal"]')

  const bitrateValues = ['7.4', '7.8', '8.1', '7.9', '8.3', '8.0', '7.6']
  let idx = 0

  function tick() {
    if (bitrateEl) {
      bitrateEl.textContent = bitrateValues[idx % bitrateValues.length]
    }
    if (dropsEl) dropsEl.textContent = '0'
    if (thermalEl) thermalEl.textContent = 'NOMINAL'
    idx++
  }
  tick()
  return setInterval(tick, 1200)
}

// Smooth copy-to-clipboard for code blocks
export function initCopyButtons() {
  document.querySelectorAll('[data-copy]').forEach(btn => {
    btn.addEventListener('click', async () => {
      const target = document.querySelector(btn.dataset.copy)
      if (!target) return
      await navigator.clipboard.writeText(target.textContent.trim())
      const orig = btn.textContent
      btn.textContent = 'Copied'
      setTimeout(() => { btn.textContent = orig }, 1800)
    })
  })
}

// Docs sidebar — active state on scroll
export function initDocsSidebar() {
  const links = document.querySelectorAll('.sidebar-link')
  const sections = document.querySelectorAll('section[id]')
  if (!links.length || !sections.length) return

  const io = new IntersectionObserver((entries) => {
    entries.forEach(entry => {
      if (entry.isIntersecting) {
        links.forEach(l => l.classList.remove('active'))
        const active = document.querySelector(`.sidebar-link[href="#${entry.target.id}"]`)
        if (active) active.classList.add('active')
      }
    })
  }, { rootMargin: '-30% 0px -60% 0px' })

  sections.forEach(s => io.observe(s))
}

// Auto-run on DOMContentLoaded
document.addEventListener('DOMContentLoaded', () => {
  initNav()
  initScrollAnimations()
  initCopyButtons()
  initDocsSidebar()
})
