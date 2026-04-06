<script setup lang="ts">
import { ref, computed, watch, onMounted, onUnmounted } from 'vue'
import {
	fetchData, IS_SERVER_HOSTED,
	getTuyaSettings, saveTuyaSettings,
	getRemoteSettings, saveRemoteSettings,
	getAlarmConfig,
} from '@/api/pitpirate'
import type { TuyaSettings, RemoteSettings, AlarmConfig } from '@/api/pitpirate'
import { RANGES, selectedHours, selectedHeight } from '@/composables/useChartRange'
import { probePollMs, staleDataMs, appWidth } from '@/composables/useAppSettings'
import BottomNav from '@/components/BottomNav.vue'
import SettingsView from '@/components/SettingsView.vue'
import ProbeChart from '@/components/ProbeChartSvg.vue'
import FanChart from '@/components/FanChartSvg.vue'
import AlarmDialog from '@/components/AlarmDialog.vue'

// ── view state ────────────────────────────────────────────────────────────────
const view = ref<'probes' | 'settings'>('probes')

// ── probe data ────────────────────────────────────────────────────────────────
interface Probe { id: string; temp: number | null }
const PROBE_IDS_KEY = 'pitpirate_probe_ids'
function loadStoredProbes(): Probe[] {
	try {
		const ids: string[] = JSON.parse(localStorage.getItem(PROBE_IDS_KEY) ?? '[]')
		return ids.map(id => ({ id, temp: null }))
	} catch { return [] }
}
const probes = ref<Probe[]>(loadStoredProbes())
const openCharts = ref<Record<string, boolean>>({})
function toggleChart(id: string) {
	openCharts.value[id] = !openCharts.value[id]
}
const fanChartOpen = ref(false)
const fanSeen = ref(localStorage.getItem('pitpirate_fan_seen') === '1')
const alarmOpen = ref(false)
const alarmProbeId = ref('')
const alarmLimits = ref<AlarmConfig>({})
function openAlarm(id: string) {
	alarmProbeId.value = id
	alarmOpen.value = true
}
async function refreshAlarms() {
	try { alarmLimits.value = await getAlarmConfig() } catch { /* ignore */ }
}
const battery = ref<number | null>(null)
const rssi = ref<number | null>(null)
const fanPct = ref<number | null>(null)
const connecting = ref(true)
const hasError = ref(false)

// ── stale-data alarm ──────────────────────────────────────────────────────────
const lastDataTs = ref<number>(0)
const staleAlert = ref<boolean>(false)
const showStaleSnack = ref<boolean>(false)

function checkStale() {
	const stale = staleDataMs.value > 0 && lastDataTs.value > 0
		&& Date.now() - lastDataTs.value > staleDataMs.value
	staleAlert.value = stale
}

watch(staleAlert, (isStale) => {
	showStaleSnack.value = isStale
})

// When server-hosted the chart uses the current origin; otherwise the user-configured URL.
// remote.value.url may be a full PHP endpoint URL (e.g. ".../php/telemetry.php") —
// strip back to the bare origin so chart components can prepend /php/ themselves.
const chartServerUrl = computed(() =>
	IS_SERVER_HOSTED ? window.location.origin : remote.value.url.replace(/\/php\/[^/]*\.php$/i, '')
)

// ── settings data ─────────────────────────────────────────────────────────────

const tuya = ref<TuyaSettings>({ ip: '', id: '', key: '' })
const remote = ref<RemoteSettings>({ url: '', token: '' })
const tuyaSaving = ref(false)
const remoteSaving = ref(false)

// ── snackbar ──────────────────────────────────────────────────────────────────
const snack = ref({ show: false, text: '', color: 'success' })
function notify(text: string, color = 'success') {
	snack.value = { show: true, text, color }
}

// ── polling ───────────────────────────────────────────────────────────────────
let timer: ReturnType<typeof setInterval> | undefined
let staleTimer: ReturnType<typeof setInterval> | undefined
let alarmTimer: ReturnType<typeof setInterval> | undefined
let pollInFlight = false

async function poll() {
	// Never queue up a second request — if one is already running, skip this tick
	if (pollInFlight) return
	pollInFlight = true
	try {
		// On ESP32: abort after 10 s so a hung request doesn't block forever
		const signal = !IS_SERVER_HOSTED ? AbortSignal.timeout(10_000) : undefined
		const data = await fetchData(signal)

		if ('error' in data) { hasError.value = true; connecting.value = false; return }
		if ('connecting' in data) { connecting.value = true; hasError.value = false; return }

		connecting.value = false
		hasError.value = false
		if (data.battery  !== undefined) battery.value  = data.battery
		if (data.rssi     !== undefined) rssi.value     = data.rssi
		if (data.fan_pct  !== undefined) {
			fanPct.value = data.fan_pct
			if (!fanSeen.value) { fanSeen.value = true; localStorage.setItem('pitpirate_fan_seen', '1') }
		}

		const next: Probe[] = []
		for (const [id, temp] of Object.entries(data.probes ?? {})) {
			next.push({ id, temp })
		}
		probes.value = next
		localStorage.setItem(PROBE_IDS_KEY, JSON.stringify(next.map(p => p.id)))
		// Server path: use the actual log timestamp so stale-check reflects real data age.
		// ESP32 path: use now (successful fetch == fresh data).
		lastDataTs.value = (IS_SERVER_HOSTED && data.ts) ? data.ts * 1000 : Date.now()
		checkStale()
	} catch {
		hasError.value = true
		if (!IS_SERVER_HOSTED) staleAlert.value = true    // timeout / network error → alert
	} finally {
		pollInFlight = false
	}
}

onMounted(() => {
	poll()
	timer = setInterval(poll, probePollMs.value)
	staleTimer = setInterval(checkStale, 10000)
	refreshAlarms()
	alarmTimer = setInterval(refreshAlarms, 30_000)
	if (!IS_SERVER_HOSTED) {
		getRemoteSettings().then(r => { remote.value = r })
	}
	document.documentElement.style.setProperty("--container-main-width", `${appWidth.value}px`)
})
onUnmounted(() => { clearInterval(timer); clearInterval(staleTimer); clearInterval(alarmTimer) })

// Restart probe poll timer when the interval setting changes
watch(probePollMs, (ms) => {
	if (timer) clearInterval(timer)
	timer = setInterval(poll, ms)
})

// ── settings load / save ──────────────────────────────────────────────────────
async function loadSettings() {
	if (IS_SERVER_HOSTED) return
		;[tuya.value, remote.value] = await Promise.all([getTuyaSettings(), getRemoteSettings()])
}

async function onSaveTuya() {
	tuyaSaving.value = true
	try { await saveTuyaSettings(tuya.value); notify('Device settings saved') }
	catch (e: unknown) { notify((e as Error).message, 'error') }
	finally { tuyaSaving.value = false }
}

async function onSaveRemote() {
	remoteSaving.value = true
	try { await saveRemoteSettings(remote.value); notify(remote.value.url ? 'Server endpoint saved' : 'Server endpoint disabled') }
	catch (e: unknown) { notify((e as Error).message, 'error') }
	finally { remoteSaving.value = false }
}


// ── helpers ───────────────────────────────────────────────────────────────────
const probeBaseColor: Record<string, string> = {
	'1': '#e53935', // red
	'2': '#fb8c00', // orange
	'3': '#fdd835', // yellow
	'4': '#43a047', // green
	'5': '#8e24aa', // purple
	'6': '#00acc1', // cyan (ambient)
	'7': '#777777', // gray (ambient alt)
}
function probeTextColor(id: string): string {
	return probeBaseColor[id] ?? '#616161'
}

function onNavChange(val: 'probes' | 'settings') {
	if (val === 'settings') loadSettings()
}
</script>

<template>
	<v-app>
		<v-main>
			<v-container class="container-main">

				<!-- ── Probes view ───────────────────────────────────────────────── -->
				<div v-if="view === 'probes'" class="px-3">

					<!-- connecting spinner — shown only on first-ever load (no stored probe IDs yet) -->
					<v-card v-if="connecting && probes.length === 0" class="text-center pa-8" rounded="lg">
						<v-progress-circular indeterminate color="primary" size="48" />
						<div class="mt-4 text-body-1">Connecting…</div>
					</v-card>

					<template v-else-if="probes.length > 0">
						<!-- chart controls — only shown when at least one chart is open -->
						<div v-if="Object.values(openCharts).some(Boolean)" class="probe-chart__header d-flex flex-wrap ga-1  align-center justify-center justify-lg-space-between w-100 mb-2">
							<div class="probe-chart__header__height d-flex align-center" style="min-width:180px">
								<v-label text="Height" class="text-label-small font-weight-bold">
									<v-slider
										v-model="selectedHeight"
										:min="150" :max="600" :step="25"
										hide-details
										density="compact"
										thumb-size="10"
										thumb-color="#ddd"
										track-size="2"
										track-color="#444"
										track-fill-color="#999"
										width="150"
										class="ml-5"
									>
										<template #thumb-label="{ modelValue }">{{ modelValue }}px</template>
									</v-slider>
								</v-label>
							</div>
							<div class="probe-chart__header__range d-flex gc-3">
								<v-chip v-for="r in RANGES" :key="r.hours" :variant="selectedHours === r.hours ? 'elevated' : 'tonal'" size="small" density="compact" @click="selectedHours = r.hours">{{ r.label }}</v-chip>
							</div>
						</div>
					<!-- fan speed card & probe cards -->
					<v-row dense>
						<v-col v-if="fanSeen" cols="12" class="probe">
							<v-card color="#001a33" rounded="lg">
								<template #prepend>
									<div class="probe__header d-flex gc-1 pa-1">
										<v-btn icon size="small" variant="text" density="compact" title="Graph" @click.stop="fanChartOpen = !fanChartOpen">
											<v-icon icon="chart-bell-curve-cumulative" size="15" style="color: #4fc3f7" />
										</v-btn>
									</div>
								</template>
								<template #title>
									<div class="probe__content d-flex flex-column align-center">
										<div class="probe__content__title text-label-medium font-weight-bold">Fan Speed</div>
										<div class="probe__content__value text-h2 font-weight-bold" style="color: #4fc3f7">{{ fanPct ?? '—' }}<span v-if="fanPct !== null" class="text-h5">%</span></div>
									</div>
								</template>
								<template v-if="fanChartOpen" #text>
									<FanChart :serverUrl="chartServerUrl" color="#4fc3f7" />
								</template>
							</v-card>
						</v-col>
						<v-col v-for="p in probes" :key="p.id" cols="12" class="probe">
								<v-card rounded="lg" :style="`background-color: var(--probe-bg--${p.id})`">
									<!-- ICON HEADER -->
									<template #prepend>
										<div class="probe__header d-flex gc-1 pa-1">
											<v-btn icon size="small" variant="text" density="compact" title="Graph" @click.stop="toggleChart(p.id)">
												<v-icon icon="chart-bell-curve-cumulative" size="15" :style="`color: var(--probe-text--${p.id})`" />
											</v-btn>
											<v-btn icon size="small" variant="text" density="compact" title="Alarms" @click.stop="openAlarm(p.id)">
												<v-icon icon="alarm" size="15" :style="`color: var(--probe-text--${p.id})`" />
											</v-btn>
										</div>
									</template>
									<!-- PROBE DATA -->
									<template #title>
										<div class="probe__content d-flex flex-column align-center">
											<div class="probe__content__title text-label-medium font-weight-bold">{{ (p.id != "7") ? `Probe ${p.id}` : `Ambient ${p.id}` }}</div>
											<div class="probe__content__value text-h2 font-weight-bold" :style="`color: var(--probe-text--${p.id})`">{{ p.temp ?? '—' }}<span v-if="p.temp !== null">&deg;</span></div>
										</div>
									</template>
									<template v-if="openCharts[p.id]" #text>
										<ProbeChart :probeId="p.id" :serverUrl="chartServerUrl" :color="probeTextColor(p.id)" :lo="alarmLimits[p.id]?.lo ?? 0" :hi="alarmLimits[p.id]?.hi ?? 0" />
									</template>
								</v-card>
							</v-col>
						</v-row>
					</template>

				</div>

				<!-- ── Settings view ─────────────────────────────────────────────── -->
				<SettingsView v-else :tuya="tuya" :remote="remote" :tuyaSaving="tuyaSaving" :remoteSaving="remoteSaving" @save-tuya="onSaveTuya" @save-remote="onSaveRemote" />
			</v-container><!-- /app-container -->
		</v-main>

		<!-- ── Bottom navigation ─────────────────────────────────────────────── -->
		<BottomNav v-model="view" :battery :rssi :fanPct :hasError :hasAlert="staleAlert" @update:model-value="onNavChange" />

		<!-- ── Alarm dialog ──────────────────────────────────────────────────── -->
		<AlarmDialog v-model="alarmOpen" :probeId="alarmProbeId" :color="probeTextColor(alarmProbeId)" @saved="refreshAlarms" />

		<!-- ── Device-not-found snackbar ───────────────────────────────────── -->
		<v-snackbar v-model="hasError" color="error" :timeout="-1" location="top">
			<v-icon icon="alert-circle-outline" size="18" class="mr-2" />
			No device found. Make sure both the NC01 and PitPirate are in the same WiFi network
			<template #actions>
				<v-btn variant="text" density="compact" @click="hasError = false">Dismiss</v-btn>
			</template>
		</v-snackbar>

		<!-- ── Stale-data persistent snackbar ───────────────────────────────── -->
		<v-snackbar v-model="showStaleSnack" color="warning" :timeout="-1" location="top">
			<v-icon icon="alert-circle-outline" size="18" class="mr-2" />
			No fresh data received from device
			<template #actions>
				<v-btn variant="text" density="compact" @click="showStaleSnack = false">Dismiss</v-btn>
			</template>
		</v-snackbar>

		<!-- ── Snackbar feedback ─────────────────────────────────────────────── -->
		<v-snackbar v-model="snack.show" :color="snack.color" timeout="3000" location="top">
			{{ snack.text }}
		</v-snackbar>
	</v-app>
</template>
