import os from 'node:os'

/** Exact browser route the client half polls; document-relative on the client. */
const STATS_PATH = '/plugins/cpu-ram-meter/stats'
/** A cached tick window older than this is too stale to describe the present. */
const MAX_STALE_MS = 10000
/** Short fresh window taken when no usable cached window exists. */
const FRESH_WINDOW_MS = 250

/** Summed cumulative CPU tick counters across every core. */
function cpuTicks() {
  let idle = 0
  let total = 0
  for (const cpu of os.cpus()) {
    const times = cpu.times
    const idleTicks = times.idle || 0
    idle += idleTicks
    total += idleTicks + (times.user || 0) + (times.nice || 0) + (times.sys || 0) + (times.irq || 0)
  }
  return { idle, total }
}

/** Busy share between two tick snapshots, or null when no tick elapsed. */
function busyPercent(next, base) {
  const totalDelta = next.total - base.total
  const idleDelta = next.idle - base.idle
  if (totalDelta <= 0) return null
  return Math.min(100, Math.max(0, 100 * (1 - idleDelta / totalDelta)))
}

const wait = (ms) => new Promise((resolve) => setTimeout(resolve, ms))

/**
 * Whole-machine CPU and memory usage, as percentages rounded to one decimal.
 * Consecutive polls reuse the tick window between them, so a steady poller
 * sees the average over its own interval instead of a near-empty window.
 */
function createSampler() {
  let previous = cpuTicks()
  let previousAt = Date.now()

  return async () => {
    let current = cpuTicks()
    let cpu = Date.now() - previousAt <= MAX_STALE_MS ? busyPercent(current, previous) : null
    if (cpu === null) {
      const base = cpuTicks()
      await wait(FRESH_WINDOW_MS)
      current = cpuTicks()
      cpu = busyPercent(current, base)
    }
    previous = current
    previousAt = Date.now()

    const totalBytes = os.totalmem()
    const usedBytes = Math.max(0, totalBytes - os.freemem())
    const ram = totalBytes > 0 ? (100 * usedBytes) / totalBytes : null

    return {
      cpu: cpu === null ? null : Math.round(cpu * 10) / 10,
      ram: ram === null ? null : Math.round(ram * 10) / 10,
      cores: os.cpus().length,
      totalBytes,
      usedBytes,
      sampledAt: Date.now(),
    }
  }
}

/**
 * Host half: exposes one read-only JSON route with this machine's live load.
 * The client dock polls it; nothing is pushed, so an idle meter costs nothing.
 */
export function apply(ctx) {
  const sample = createSampler()

  const handler = async (req, res) => {
    if (req.method !== 'GET' && req.method !== 'HEAD') {
      res.writeHead(405, { allow: 'GET, HEAD' })
      res.end()
      return
    }
    let body
    try {
      body = JSON.stringify(await sample())
    } catch (error) {
      res.writeHead(500, { 'content-type': 'text/plain; charset=utf-8', 'cache-control': 'no-store' })
      res.end(String(error))
      return
    }
    res.writeHead(200, {
      'content-type': 'application/json; charset=utf-8',
      'cache-control': 'no-store',
    })
    res.end(req.method === 'HEAD' ? undefined : body)
  }

  ctx.inject(['webServer'], (webCtx) => {
    webCtx.effect(
      () => webCtx.webServer.register({ kind: 'exact', path: STATS_PATH, handler }),
      'cpu-ram-meter: stats route',
    )
  })
}
