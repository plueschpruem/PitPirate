const NO_CACHE = { 'pragma': 'no-cache', 'cache-control': 'no-cache' }

// True when the app is served from the HTTPS remote server (not the ESP32).
// In this mode the ESP32 is not reachable; use server-side PHP endpoints instead.
export const IS_SERVER_HOSTED =
  typeof window !== 'undefined' && window.location.protocol === 'https:'

// ── /data ─────────────────────────────────────────────────────────────────────
export interface ProbeMap {
  [id: string]: number
}

export interface RawData {
  probes?:        ProbeMap
  battery?:       number
  probe_state?:   number
  cook_alarm?:    number
  device_alarm?:  number
  device_on?:     number
  error?:         number
  connecting?:    number
  ts?:            number   // Unix seconds — present in server-side log entries
  rssi?:          number   // WiFi signal strength in dBm
  fan_pct?:       number   // Current fan speed 0–100 %
}

export async function fetchData(signal?: AbortSignal): Promise<RawData> {
  if (IS_SERVER_HOSTED) {
    // Served from the HTTPS server — read the latest log entry
    const res = await fetch(`/php/telemetry_get.php?probe=all&_=${Date.now()}`)
    if (!res.ok) throw new Error(`HTTP ${res.status}`)
    return res.json() as Promise<RawData>
  }
  const res = await fetch('/data', { headers: NO_CACHE, signal })
  if (!res.ok) throw new Error(`HTTP ${res.status}`)
  const json = await res.json()
  return json[0] as RawData
}


// ── /tuya-config & /settings ──────────────────────────────────────────────────
export interface TuyaSettings { ip: string; id: string; key: string }

export async function getTuyaSettings(): Promise<TuyaSettings> {
  const res = await fetch('/tuya-config', { headers: NO_CACHE })
  return res.json()
}

export async function saveTuyaSettings(s: TuyaSettings): Promise<void> {
  const res = await fetch(
    `/settings?ip=${encodeURIComponent(s.ip)}&id=${encodeURIComponent(s.id)}&key=${encodeURIComponent(s.key)}`,
    { headers: NO_CACHE },
  )
  if (!res.ok) throw new Error(await res.text())
}

// ── /remote-config & /save-remote ─────────────────────────────────────────────
export interface RemoteSettings { url: string; token: string }

export async function getRemoteSettings(): Promise<RemoteSettings> {
  const res = await fetch('/remote-config', { headers: NO_CACHE })
  return res.json()
}

export async function saveRemoteSettings(s: RemoteSettings): Promise<void> {
  const res = await fetch(
    `/save-remote?url=${encodeURIComponent(s.url)}&token=${encodeURIComponent(s.token)}`,
    { headers: NO_CACHE },
  )
  if (!res.ok) throw new Error(await res.text())
}

// ── /alarm-config & /save-alarms ──────────────────────────────────────────────
export interface AlarmLimit { lo: number; hi: number }
export type AlarmConfig = Record<string, AlarmLimit>

export async function getAlarmConfig(): Promise<AlarmConfig> {
  if (IS_SERVER_HOSTED) {
    const res = await fetch(`/php/telemetry_get.php?probe=alarms&_=${Date.now()}`)
    if (!res.ok) throw new Error(`HTTP ${res.status}`)
    return res.json()
  }
  const res = await fetch('/alarm-config', { headers: NO_CACHE })
  if (!res.ok) throw new Error(`HTTP ${res.status}`)
  return res.json()
}

export async function saveAlarmConfig(id: string, lo: number, hi: number): Promise<void> {
  if (IS_SERVER_HOSTED) {
    const res = await fetch('/php/telemetry.php?action=alarms', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ [id]: { lo, hi } }),
    })
    if (!res.ok) throw new Error(`HTTP ${res.status}`)
    return
  }
  const res = await fetch(
    `/save-alarms?${encodeURIComponent(id)}_lo=${lo}&${encodeURIComponent(id)}_hi=${hi}`,
    { headers: NO_CACHE },
  )
  if (!res.ok) throw new Error(await res.text())
}

// ── /wifi-config, /wifi-scan, /save-wifi ──────────────────────────────────────
export interface WifiConfig  { ssid: string; ap_mode: boolean }
export interface WifiNetwork { ssid: string; rssi: number; secure: boolean }

export async function getWifiConfig(): Promise<WifiConfig> {
  const res = await fetch('/wifi-config', { headers: NO_CACHE })
  if (!res.ok) throw new Error(`HTTP ${res.status}`)
  return res.json()
}

export async function scanWifi(): Promise<WifiNetwork[]> {
  const res = await fetch('/wifi-scan', { headers: NO_CACHE })
  if (!res.ok) throw new Error(`HTTP ${res.status}`)
  return res.json()
}

export async function saveWifi(ssid: string, pw: string): Promise<void> {
  const res = await fetch(
    `/save-wifi?ssid=${encodeURIComponent(ssid)}&pw=${encodeURIComponent(pw)}`,
    { headers: NO_CACHE },
  )
  if (!res.ok) throw new Error(await res.text())
}

// ── /fan-config & /save-fan ─────────────────────────────────────────────────────────
export interface FanConfig { pct: number; start: number; min: number }

export async function getFanConfig(): Promise<FanConfig> {
  const url = IS_SERVER_HOSTED ? `/php/blower.php?_=${Date.now()}` : '/fan-config'
  const res = await fetch(url, { headers: NO_CACHE })
  if (!res.ok) throw new Error(`HTTP ${res.status}`)
  return res.json()
}

export async function saveFanSpeed(pct: number): Promise<void> {
  if (IS_SERVER_HOSTED) {
    const res = await fetch('/php/blower.php', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ pct }),
    })
    if (!res.ok) throw new Error(`HTTP ${res.status}`)
    return
  }
  const res = await fetch(`/save-fan?pct=${pct}`, { headers: NO_CACHE })
  if (!res.ok) throw new Error(await res.text())
}

export async function saveFanSettings(start: number, min: number): Promise<void> {
  if (IS_SERVER_HOSTED) {
    const res = await fetch('/php/blower.php', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ start, min }),
    })
    if (!res.ok) throw new Error(`HTTP ${res.status}`)
    return
  }
  const res = await fetch(`/save-fan-settings?start=${start}&min=${min}`, { headers: NO_CACHE })
  if (!res.ok) throw new Error(await res.text())
}

// ── /telemetry-config & /save-telemetry-config ────────────────────────────────
// ESP32-direct only — reads/writes the firmware post-interval and on-change flag.
export interface TelemetryConfig { interval_s: number; on_change: boolean }

export async function getTelemetryConfig(): Promise<TelemetryConfig> {
  const res = await fetch('/telemetry-config', { headers: NO_CACHE })
  if (!res.ok) throw new Error(`HTTP ${res.status}`)
  return res.json()
}

export async function saveTelemetryConfig(cfg: TelemetryConfig): Promise<void> {
  const res = await fetch(
    `/save-telemetry-config?interval_s=${cfg.interval_s}&on_change=${cfg.on_change ? '1' : '0'}`,
    { headers: NO_CACHE },
  )
  if (!res.ok) throw new Error(await res.text())
}

// ── /pid-config & /save-pid ──────────────────────────────────────────────────
export interface PidConfig {
  enabled:  boolean
  setpoint: number
  probe:    number   // 0-5 = food probes 1-6, 6 = ambient/pit
  kp_con:   number
  ki_con:   number
  kd_con:   number
  kp_agg:   number
  ki_agg:   number
  kd_agg:   number
  bias:     number
  out_min:  number
  out_max:  number
  lid_detect: boolean   // enable lid-open detection
  // read-only runtime diagnostics
  output:   number
  error:    number
  integral: number
  lid_open: boolean
}

export async function getPidConfig(): Promise<PidConfig> {
  const res = await fetch('/pid-config', { headers: NO_CACHE })
  if (!res.ok) throw new Error(`HTTP ${res.status}`)
  return res.json()
}

export async function savePidConfig(cfg: Omit<PidConfig, 'output' | 'error' | 'integral' | 'lid_open'>): Promise<void> {
  const p = new URLSearchParams({
    enabled:  cfg.enabled ? '1' : '0',
    setpoint: String(cfg.setpoint),
    probe:    String(cfg.probe),
    kp_con:   String(cfg.kp_con),
    ki_con:   String(cfg.ki_con),
    kd_con:   String(cfg.kd_con),
    kp_agg:   String(cfg.kp_agg),
    ki_agg:   String(cfg.ki_agg),
    kd_agg:   String(cfg.kd_agg),
    bias:       String(cfg.bias),
    out_min:    String(cfg.out_min),
    out_max:    String(cfg.out_max),
    lid_detect: cfg.lid_detect ? '1' : '0',
  })
  const res = await fetch(`/save-pid?${p}`, { headers: NO_CACHE })
  if (!res.ok) throw new Error(await res.text())
}

// ── /servo-config & /save-servo ───────────────────────────────────────────────
export interface ServoConfig { angle: number; min: number; max: number; auto: boolean }

export async function getServoConfig(): Promise<ServoConfig> {
  const res = await fetch('/servo-config', { headers: NO_CACHE })
  if (!res.ok) throw new Error(`HTTP ${res.status}`)
  return res.json()
}

// Immediately moves the servo to the given angle (called while dragging the slider).
export async function setServoAngle(angle: number): Promise<void> {
  const res = await fetch(`/save-servo?angle=${angle}`, { headers: NO_CACHE })
  if (!res.ok) throw new Error(await res.text())
}

// Persists the min/max limits (and optionally a new angle) to NVS.
export async function saveServoLimits(min: number, max: number): Promise<void> {
  const res = await fetch(`/save-servo?min=${min}&max=${max}`, { headers: NO_CACHE })
  if (!res.ok) throw new Error(await res.text())
}

// Enables or disables automatic (fan-coupled) mode.
export async function saveServoAuto(enabled: boolean): Promise<void> {
  const res = await fetch(`/save-servo?auto=${enabled ? '1' : '0'}`, { headers: NO_CACHE })
  if (!res.ok) throw new Error(await res.text())
}

// ── Sessions ──────────────────────────────────────────────────────────────────

export async function listSessions(base = ''): Promise<string[]> {
  const res = await fetch(`${base}/php/sessions.php?action=list&_=${Date.now()}`)
  if (!res.ok) throw new Error(`HTTP ${res.status}`)
  return res.json()
}

export async function startNewSession(base = ''): Promise<{ name: string }> {
  const res = await fetch(`${base}/php/sessions.php?action=new-session`, { method: 'POST' })
  if (!res.ok) {
    const body = await res.json().catch(() => ({}))
    throw new Error((body as { error?: string }).error ?? `HTTP ${res.status}`)
  }
  return res.json()
}
