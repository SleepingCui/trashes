window.__ModuleLoader__.load({
  id: '@local/cpu-ram-meter',
  factory(require) {
    const React = require('react')
    const h = React.createElement

    /** Locale namespace for the dictionaries registered below. */
    const NS = 'cpu-ram-meter'
    /** Document-relative: the page may be served from a sub-path. */
    const ENDPOINT = 'plugins/cpu-ram-meter/stats'
    const REFRESH_MS = 3000
    const HIDDEN_RETRY_MS = 15000

    /** Strings the slot resolves through the client locale service. */
    const DICTIONARIES = {
      en: {
        'label.cpu': 'CPU',
        'label.ram': 'RAM',
        'aria': 'CPU {cpu}%, memory {ram}%',
        'detail': '{cores} cores · {used} of {total} memory in use',
      },
      zh: {
        'label.cpu': 'CPU',
        'label.ram': '内存',
        'aria': 'CPU 占用 {cpu}，内存占用 {ram}',
        'detail': '{cores} 核 · 已用 {used} / {total}',
      },
    }

    /** Same table in English, used when the locale service is unavailable. */
    const FALLBACK = DICTIONARIES.en

    /** Rebound to the locale service in `apply`; the dock slot passes no `t`. */
    let translate = (key, params) => interpolate(FALLBACK[key] === undefined ? key : FALLBACK[key], params)

    function interpolate(template, params) {
      return template.replace(/\{(\w+)\}/g, (match, key) =>
        params !== undefined && params[key] !== undefined ? String(params[key]) : match,
      )
    }

    const styles = {
      chip: {
        display: 'inline-flex',
        flex: 'none',
        alignItems: 'center',
        gap: '6px',
        padding: '1px 8px',
        borderRadius: 'var(--dsw-radius-sm)',
        color: 'var(--dsw-alias-label-tertiary)',
        fontFamily: 'inherit',
        fontSize: 'var(--dsh-content-font-size-secondary, 13px)',
        lineHeight: 'calc(20px + var(--dsh-content-font-delta-secondary, 0px))',
        fontVariantNumeric: 'tabular-nums',
        whiteSpace: 'nowrap',
        cursor: 'default',
      },
      label: {
        letterSpacing: '0.02em',
        textTransform: 'uppercase',
        fontSize: '0.85em',
      },
      value: {
        color: 'var(--dsw-alias-label-secondary)',
        fontWeight: 500,
      },
      separator: {
        opacity: 0.4,
      },
      pair: {
        display: 'inline-flex',
        alignItems: 'baseline',
        gap: '3px',
      },
    }

    function gib(bytes) {
      return `${(bytes / 1024 / 1024 / 1024).toFixed(1)} GB`
    }

    function percent(value) {
      return value === null || value === undefined || !Number.isFinite(value) ? '--' : `${Math.round(value)}%`
    }

    /** One compact dock entry: `CPU 12% · RAM 64%`, refreshed by polling. */
    function CpuRamMeter(props) {
      const t = typeof props.t === 'function' ? props.t : translate
      const [reading, setReading] = React.useState(null)

      React.useEffect(() => {
        let disposed = false
        let timer = 0
        let controller = null

        const schedule = (delay) => {
          if (disposed) return
          timer = window.setTimeout(load, delay)
        }

        const load = () => {
          if (disposed) return
          const hidden = typeof document !== 'undefined' && document.visibilityState === 'hidden'
          if (hidden) {
            schedule(HIDDEN_RETRY_MS)
            return
          }
          controller = new AbortController()
          fetch(ENDPOINT, { cache: 'no-store', signal: controller.signal })
            .then((response) => {
              if (!response.ok) throw new Error(`cpu-ram-meter: HTTP ${response.status}`)
              return response.json()
            })
            .then((data) => {
              if (!disposed) setReading(data)
            })
            .catch(() => {
              // A failed poll keeps the last reading on screen instead of blanking.
            })
            .then(() => {
              controller = null
              schedule(REFRESH_MS)
            })
        }

        load()
        return () => {
          disposed = true
          window.clearTimeout(timer)
          if (controller !== null) controller.abort()
        }
      }, [])

      if (reading === null) return null

      const cpu = percent(reading.cpu)
      const ram = percent(reading.ram)
      const aria = t('aria', { cpu, ram })
      const detail =
        Number.isFinite(reading.cores) && Number.isFinite(reading.totalBytes) && Number.isFinite(reading.usedBytes)
          ? t('detail', { cores: reading.cores, used: gib(reading.usedBytes), total: gib(reading.totalBytes) })
          : undefined
      const title = detail === undefined ? aria : `${aria}\n${detail}`

      return h(
        'span',
        { style: styles.chip, title, 'aria-label': aria, role: 'img', 'data-cpu-ram-meter': '' },
        h(
          'span',
          { style: styles.pair },
          h('span', { style: styles.label }, t('label.cpu')),
          h('span', { style: styles.value }, cpu),
        ),
        h('span', { style: styles.separator, 'aria-hidden': true }, '·'),
        h(
          'span',
          { style: styles.pair },
          h('span', { style: styles.label }, t('label.ram')),
          h('span', { style: styles.value }, ram),
        ),
      )
    }

    return {
      inject: ['slots', 'locale'],
      apply(ctx) {
        ctx.effect(() => ctx.locale.register(NS, DICTIONARIES), 'cpu-ram-meter: dictionaries')
        const bound = ctx.locale.bind(NS)
        if (typeof bound === 'function') translate = bound
        ctx.slots.inject('conversation.composer.dock', () =>
          ctx.slots.register(
            {
              name: 'conversation.composer.dock',
              id: 'cpu-ram-meter',
              order: -1,
            },
            CpuRamMeter,
          ),
        )
      },
    }
  },
})
