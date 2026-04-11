<script setup lang="ts">
import { ref, onMounted } from 'vue'
import type { TuyaSettings, RemoteSettings, WifiConfig, WifiNetwork, PidConfig, ServoConfig } from '@/api/pitpirate'
import { getWifiConfig, scanWifi, saveWifi, getPidConfig, savePidConfig, getTelemetryConfig, saveTelemetryConfig, getServoConfig, setServoAngle, saveServoLimits, saveServoAuto } from '@/api/pitpirate'

const props = defineProps<{
	tuya: TuyaSettings
	remote: RemoteSettings
	tuyaSaving: boolean
	remoteSaving: boolean
}>()

const emit = defineEmits<{
	'save-tuya': []
	'save-remote': []
}>()

const isHttps = typeof window !== 'undefined' && window.location.protocol === 'https:'

// ── WiFi provisioning ─────────────────────────────────────────────────────────

const wifiConfig   = ref<WifiConfig | null>(null)
const wifiNetworks = ref<WifiNetwork[]>([])
const wifiSSID     = ref('')
const wifiPW       = ref('')
const showPW       = ref(false)
const wifiScanning = ref(false)
const wifiSaving   = ref(false)
const wifiError    = ref('')

async function loadWifiConfig() {
	if (isHttps) return
	try {
		wifiConfig.value = await getWifiConfig()
		wifiSSID.value   = wifiConfig.value.ssid
	} catch { /* ignore — might be an older firmware */ }
}

async function doScanWifi() {
	wifiScanning.value = true
	wifiError.value    = ''
	try {
		const nets = await scanWifi()
		const seen = new Set<string>()
		wifiNetworks.value = nets
			.sort((a, b) => b.rssi - a.rssi)
			.filter(n => { if (seen.has(n.ssid)) return false; seen.add(n.ssid); return true })
	} catch (e: unknown) {
		wifiError.value = (e as Error).message
	} finally {
		wifiScanning.value = false
	}
}

async function doSaveWifi() {
	if (!wifiSSID.value) return
	wifiSaving.value = true
	wifiError.value  = ''
	try {
		await saveWifi(wifiSSID.value, wifiPW.value)
		wifiError.value = 'Saved — esp32 is rebooting…'
	} catch (e: unknown) {
		wifiError.value = (e as Error).message
	} finally {
		wifiSaving.value = false
	}
}

onMounted(loadWifiConfig)

// ── PID controller ────────────────────────────────────────────────────────────

const pidCfg = ref<PidConfig | null>(null)
const pidSaving  = ref(false)
const pidError   = ref('')
const pidSuccess = ref(false)

async function loadPidConfig() {
	if (isHttps) return
	try { pidCfg.value = await getPidConfig() }
	catch { /* older firmware — hide section */ }
}

async function doSavePid() {
	if (!pidCfg.value) return
	pidSaving.value  = true
	pidError.value   = ''
	pidSuccess.value = false
	try {
		const { output, error, integral, lid_open, ...settable } = pidCfg.value
		await savePidConfig(settable)
		// Refresh so read-only diagnostics update
		pidCfg.value    = await getPidConfig()
		pidSuccess.value = true
		setTimeout(() => { pidSuccess.value = false }, 3000)
	} catch (e: unknown) {
		pidError.value = (e as Error).message
	} finally {
		pidSaving.value = false
	}
}

const probeOptions = [
	{ title: 'Probe 1', value: 0 },
	{ title: 'Probe 2', value: 1 },
	{ title: 'Probe 3', value: 2 },
	{ title: 'Probe 4', value: 3 },
	{ title: 'Probe 5', value: 4 },
	{ title: 'Ambient (Pit)', value: 6 },
]

onMounted(loadPidConfig)

// ── Telemetry post settings ────────────────────────────────────────────────

const TELEMETRY_INTERVAL_OPTIONS = [
	{ s: 10,  label: '10 s' },
	{ s: 30,  label: '30 s' },
	{ s: 60,  label: '1 min' },
	{ s: 180, label: '3 min' },
]

const telInterval  = ref(10)
const telOnChange  = ref(false)
const telSaving    = ref(false)
const telError     = ref('')

async function loadTelemetryConfig() {
	if (isHttps) return
	try {
		const cfg = await getTelemetryConfig()
		telInterval.value = cfg.interval_s
		telOnChange.value = cfg.on_change
	} catch { /* older firmware */ }
}

async function doSaveTelemetryConfig() {
	telSaving.value = true
	telError.value  = ''
	try {
		await saveTelemetryConfig({ interval_s: telInterval.value, on_change: telOnChange.value })
	} catch (e: unknown) {
		telError.value = (e as Error).message
	} finally {
		telSaving.value = false
	}
}

onMounted(loadTelemetryConfig)

// ── Servo ────────────────────────────────────────────────────────────────────

const servoCfg    = ref<ServoConfig>({ angle: 90, min: 0, max: 180, auto: false })
const servoAngle  = ref(90)
const servoAuto   = ref(false)
const servoSaving = ref(false)
const servoError  = ref('')
let   servoDebounceTimer: ReturnType<typeof setTimeout> | null = null

async function loadServoConfig() {
	if (isHttps) return
	try {
		servoCfg.value   = await getServoConfig()
		servoAngle.value = servoCfg.value.angle
		servoAuto.value  = servoCfg.value.auto
	} catch { /* keep defaults */ }
}

function onServoSlider(val: number | undefined) {
	if (val === undefined) return
	if (servoDebounceTimer) clearTimeout(servoDebounceTimer)
	servoDebounceTimer = setTimeout(async () => {
		try { await setServoAngle(val) }
		catch (e: unknown) { servoError.value = (e as Error).message }
	}, 80)
}

async function doSaveServoLimits() {
	servoSaving.value = true
	servoError.value  = ''
	try {
		await saveServoLimits(servoCfg.value.min, servoCfg.value.max)
		// clamp current angle into new limits
		servoAngle.value = Math.min(Math.max(servoAngle.value, servoCfg.value.min), servoCfg.value.max)
	} catch (e: unknown) {
		servoError.value = (e as Error).message
	} finally {
		servoSaving.value = false
	}
}

async function toggleServoAuto() {
	const next = !servoAuto.value
	try {
		await saveServoAuto(next)
		servoAuto.value = next
		servoCfg.value.auto = next
	} catch (e: unknown) {
		servoError.value = (e as Error).message
	}
}

onMounted(loadServoConfig)
</script>

<template>
	<div>

		<v-alert v-if="isHttps" color="gray" variant="tonal" rounded="lg" class="mb-3">
			<strong>Remote view</strong> — esp32 settings can only be changed when you
			open PitPirate directly from your local network (via the esp32 IP).
		</v-alert>

		<!-- WiFi Settings -->
		<v-card v-if="!isHttps" class="mb-3" rounded="lg">
			<v-card-title class="text-body-1 font-weight-bold pt-4 px-4">
				<v-icon icon="wifi" size="18" class="mr-2 mb-1" />
				WiFi
			</v-card-title>
			<v-card-text>

				<!-- AP mode banner -->
				<v-alert v-if="wifiConfig?.ap_mode" color="warning" variant="tonal" rounded="lg" density="compact" class="mb-3">
					<strong>Setup mode</strong> — device is broadcasting <strong>SSID: PitPirate</strong>.
					Enter your home network credentials below and save to connect.
				</v-alert>

				<div v-if="wifiConfig && !wifiConfig.ap_mode" class="text-caption text-medium-emphasis mb-3">
					Current network: <strong>{{ wifiConfig.ssid }}</strong>
				</div>

				<!-- Scan results -->
				<div v-if="wifiNetworks.length" class="mb-3">
					<div class="text-caption text-medium-emphasis mb-1">Select network</div>
					<v-list density="compact" rounded="lg" border>
						<v-list-item
							v-for="n in wifiNetworks" :key="n.ssid"
							:title="n.ssid"
							:subtitle="`${n.rssi} dBm · ${n.secure ? 'secured' : 'open'}`"
							:active="wifiSSID === n.ssid"
							active-color="primary"
							@click="wifiSSID = n.ssid"
						/>
					</v-list>
				</div>

				<v-text-field
					v-model="wifiSSID"
					label="SSID"
					density="compact"
					class="mb-2"
					autocomplete="username"
					hide-details="auto"
				/>
				<v-text-field
					v-model="wifiPW"
					label="Password"
					density="compact"
					:type="showPW ? 'text' : 'password'"
					:append-inner-icon="showPW ? 'eye-off' : 'eye'"
					autocomplete="current-password"
					hide-details="auto"
					@click:append-inner="showPW = !showPW"
				/>

				<v-alert
					v-if="wifiError"
					:color="wifiError.startsWith('Saved') ? 'success' : 'error'"
					variant="tonal"
					density="compact"
					class="mt-3"
				>{{ wifiError }}</v-alert>

			</v-card-text>
			<v-card-actions class="px-4 pb-4">
				<v-btn
					variant="tonal"
					prepend-icon="wifi-sync"
					:loading="wifiScanning"
					@click="doScanWifi"
				>Scan</v-btn>
				<v-spacer />
				<v-btn
					color="primary"
					variant="tonal"
					prepend-icon="content-save"
					:loading="wifiSaving"
					:disabled="!wifiSSID"
					@click="doSaveWifi"
				>Save & Reboot</v-btn>
			</v-card-actions>
		</v-card>

		<!-- Device Settings -->
		<v-card v-if="!isHttps" class="mb-3" rounded="lg">
			<form autocomplete="on" @submit.prevent="emit('save-tuya')">
				<v-card-title class="text-body-1 font-weight-bold pt-4 px-4">Device Settings</v-card-title>
				<v-card-text>
					<v-text-field v-model="props.tuya.ip" label="IP Address" placeholder="192.168.x.x" density="compact" class="mb-2" autocomplete="off" hint="The local IP Address of the thermometer" persistent-hint />
					<v-text-field v-model="props.tuya.id" label="Device ID" density="compact" class="mb-2" autocomplete="off" hint="The Devicde ID of the thermometer [from Tuya.com]" persistent-hint />
					<v-text-field v-model="props.tuya.key" label="Local Key (16 chars)" density="compact" type="password" autocomplete="current-password" hint="The local_key the thermometer [from Tuya.com]" persistent-hint />
				</v-card-text>
				<v-card-actions class="px-4 pb-4">
					<v-spacer />
					<v-btn color="primary" variant="tonal" :loading="tuyaSaving" type="submit">
						Save &amp; Reconnect
					</v-btn>
				</v-card-actions>
			</form>
		</v-card>

		<!-- PID Controller -->
		<v-card v-if="!isHttps && pidCfg" class="mb-3" rounded="lg">
			<v-card-title class="text-body-1 font-weight-bold pt-4 px-4">
				<v-icon icon="heat-wave" size="18" class="mr-2 mb-1" />
				PID Fan Controller
			</v-card-title>
			<v-card-text>

				<!-- Enable + Target -->
				<div class="d-flex align-center ga-4 mb-4">
					<v-switch
						v-model="pidCfg.enabled"
						label="Enable PID"
						color="primary"
						hide-details
						density="compact"
					/>
					<v-switch
						v-model="pidCfg.lid_detect"
						label="Lid detect"
						color="primary"
						hide-details
						density="compact"
					/>
				</div>
				<v-row dense class="mb-2">
					<v-col cols="6">
						<v-text-field
							v-model.number="pidCfg.setpoint"
							label="Setpoint (°)"
							type="number" min="0" max="600" step="1"
							density="compact"
							hide-details="auto"
						/>
					</v-col>
					<v-col cols="6">
						<v-select
							v-model="pidCfg.probe"
							:items="probeOptions"
							label="Control probe"
							density="compact"
							hide-details
						/>
					</v-col>
				</v-row>

				<!-- Conservative gains -->
				<div class="text-caption text-medium-emphasis mb-1 mt-3">Conservative gains (near setpoint)</div>
				<v-row dense class="mb-1">
					<v-col cols="4">
						<v-text-field v-model.number="pidCfg.kp_con" label="Kp" type="number" step="0.001" density="compact" hide-details />
					</v-col>
					<v-col cols="4">
						<v-text-field v-model.number="pidCfg.ki_con" label="Ki" type="number" step="0.0001" density="compact" hide-details />
					</v-col>
					<v-col cols="4">
						<v-text-field v-model.number="pidCfg.kd_con" label="Kd" type="number" step="0.001" density="compact" hide-details />
					</v-col>
				</v-row>

				<!-- Aggressive gains -->
				<div class="text-caption text-medium-emphasis mb-1 mt-3">Aggressive gains (far from setpoint)</div>
				<v-row dense class="mb-1">
					<v-col cols="4">
						<v-text-field v-model.number="pidCfg.kp_agg" label="Kp" type="number" step="0.001" density="compact" hide-details />
					</v-col>
					<v-col cols="4">
						<v-text-field v-model.number="pidCfg.ki_agg" label="Ki" type="number" step="0.0001" density="compact" hide-details />
					</v-col>
					<v-col cols="4">
						<v-text-field v-model.number="pidCfg.kd_agg" label="Kd" type="number" step="0.001" density="compact" hide-details />
					</v-col>
				</v-row>

				<!-- Output limits -->
				<div class="text-caption text-medium-emphasis mb-1 mt-3">Output limits (%)</div>
				<v-row dense class="mb-1">
					<v-col cols="4">
						<v-text-field v-model.number="pidCfg.bias" label="Bias" type="number" step="1" density="compact" hide-details />
					</v-col>
					<v-col cols="4">
						<v-text-field v-model.number="pidCfg.out_min" label="Min" type="number" min="0" max="100" step="1" density="compact" hide-details />
					</v-col>
					<v-col cols="4">
						<v-text-field v-model.number="pidCfg.out_max" label="Max" type="number" min="0" max="100" step="1" density="compact" hide-details />
					</v-col>
				</v-row>

				<!-- Live diagnostics -->
				<div class="text-caption text-medium-emphasis mb-2 mt-4">Live diagnostics</div>
				<v-row dense>
					<v-col cols="6" sm="3">
						<v-card variant="tonal" rounded="lg" class="pa-2 text-center">
							<div class="text-caption text-medium-emphasis">Output</div>
							<div class="text-body-1 font-weight-bold">{{ pidCfg.output.toFixed(1) }}%</div>
						</v-card>
					</v-col>
					<v-col cols="6" sm="3">
						<v-card variant="tonal" rounded="lg" class="pa-2 text-center">
							<div class="text-caption text-medium-emphasis">Error</div>
							<div class="text-body-1 font-weight-bold">{{ pidCfg.error.toFixed(1) }}°</div>
						</v-card>
					</v-col>
					<v-col cols="6" sm="3">
						<v-card variant="tonal" rounded="lg" class="pa-2 text-center">
							<div class="text-caption text-medium-emphasis">Integral</div>
							<div class="text-body-1 font-weight-bold">{{ pidCfg.integral.toFixed(2) }}</div>
						</v-card>
					</v-col>
					<v-col cols="6" sm="3">
						<v-card :color="pidCfg.lid_detect && pidCfg.lid_open ? 'warning' : undefined" variant="tonal" rounded="lg" class="pa-2 text-center">
							<div class="text-caption text-medium-emphasis">Lid</div>
							<div class="text-body-1 font-weight-bold">{{ !pidCfg.lid_detect ? 'Off' : pidCfg.lid_open ? 'Open' : 'Closed' }}</div>
						</v-card>
					</v-col>
				</v-row>

				<v-alert v-if="pidError" color="error" variant="tonal" density="compact" class="mt-3">{{ pidError }}</v-alert>
				<v-alert v-if="pidSuccess" color="success" variant="tonal" density="compact" class="mt-3">PID settings saved</v-alert>

			</v-card-text>
			<v-card-actions class="px-4 pb-4">
				<v-btn variant="tonal" prepend-icon="refresh" @click="loadPidConfig">Refresh</v-btn>
				<v-spacer />
				<v-btn color="primary" variant="tonal" prepend-icon="content-save" :loading="pidSaving" @click="doSavePid">Save</v-btn>
			</v-card-actions>
		</v-card>

		<!-- Servo -->
		<v-card v-if="!isHttps" class="mb-3" rounded="lg">
			<v-card-title class="text-body-1 font-weight-bold pt-4 px-4">
				<v-icon icon="valve" size="18" class="mr-2 mb-1" />
				Servo (GPIO 27)
			</v-card-title>
			<v-card-text>

				<v-btn-toggle
					:model-value="servoAuto ? 'auto' : 'manual'"
					mandatory
					color="primary"
					variant="tonal"
					density="compact"
					class="mb-4"
					@update:model-value="toggleServoAuto"
				>
					<v-btn value="manual">Manual</v-btn>
					<v-btn value="auto">Auto (follows fan)</v-btn>
				</v-btn-toggle>

				<div class="text-caption text-medium-emphasis mb-1">
					Position: <strong>{{ servoAngle }}°</strong>
					<span v-if="servoAuto" class="ml-2 text-primary">({{ servoCfg.auto ? 'controlled by fan' : '' }})</span>
				</div>
				<v-slider
					v-model="servoAngle"
					:min="servoCfg.min"
					:max="servoCfg.max"
					step="1"
					thumb-label
					color="primary"
					hide-details
					:disabled="servoAuto"
					@update:model-value="onServoSlider"
				/>

				<div class="text-caption text-medium-emphasis mt-4 mb-2">Travel limits</div>
				<v-row dense>
					<v-col cols="6">
						<v-text-field
							v-model.number="servoCfg.min"
							label="Min angle (°)"
							type="number" min="0" max="180" step="1"
							density="compact"
							hide-details="auto"
						/>
					</v-col>
					<v-col cols="6">
						<v-text-field
							v-model.number="servoCfg.max"
							label="Max angle (°)"
							type="number" min="0" max="180" step="1"
							density="compact"
							hide-details="auto"
						/>
					</v-col>
				</v-row>

				<v-alert v-if="servoError" color="error" variant="tonal" density="compact" class="mt-3">{{ servoError }}</v-alert>

			</v-card-text>
			<v-card-actions class="px-4 pb-4">
				<v-btn variant="tonal" prepend-icon="refresh" @click="loadServoConfig">Refresh</v-btn>
				<v-spacer />
				<v-btn color="primary" variant="tonal" prepend-icon="content-save" :loading="servoSaving" @click="doSaveServoLimits">Save limits</v-btn>
			</v-card-actions>
		</v-card>

		<!-- Telemetry Post Settings -->
		<v-card v-if="!isHttps" class="mb-3" rounded="lg">
			<v-card-title class="text-body-1 font-weight-bold pt-4 px-4">
				<v-icon icon="cloud-upload" size="18" class="mr-2 mb-1" />
				Telemetry Post Settings
			</v-card-title>
			<v-card-text>
				<v-switch
					v-model="telOnChange"
					label="Update on probe change"
					density="compact"
					color="primary"
					hide-details
					class="mb-4"
				/>
				<div class="text-caption text-medium-emphasis mb-2">Post interval</div>
				<div class="d-flex ga-2 flex-wrap mb-2">
					<v-chip
						v-for="o in TELEMETRY_INTERVAL_OPTIONS" :key="o.s"
						:variant="telInterval === o.s ? 'elevated' : 'tonal'"
						size="small" @click="telInterval = o.s"
					>{{ o.label }}</v-chip>
				</div>
				<div class="text-caption text-medium-emphasis">
					<span v-if="telOnChange">
						Posts when probe data changes; heartbeat every
						{{ TELEMETRY_INTERVAL_OPTIONS.find(o => o.s === telInterval)?.label }} if unchanged.
					</span>
					<span v-else>
						Posts every {{ TELEMETRY_INTERVAL_OPTIONS.find(o => o.s === telInterval)?.label }}, regardless of changes.
					</span>
				</div>
				<v-alert
					v-if="telError"
					color="error"
					variant="tonal"
					density="compact"
					class="mt-2"
				>{{ telError }}</v-alert>
			</v-card-text>
			<v-card-actions class="px-4 pb-4">
				<v-btn
					variant="tonal"
					prepend-icon="refresh"
					@click="loadTelemetryConfig"
				>Reload</v-btn>
				<v-spacer />
				<v-btn
					color="primary"
					variant="tonal"
					prepend-icon="content-save"
					:loading="telSaving"
					@click="doSaveTelemetryConfig"
				>Save</v-btn>
			</v-card-actions>
		</v-card>

		<!-- Server endpoint -->
		<v-card v-if="!isHttps" class="mb-3" rounded="lg">
			<form autocomplete="on" @submit.prevent="emit('save-remote')">
				<v-card-title class="text-body-1 font-weight-bold pt-4 px-4">Server Endpoint</v-card-title>
				<v-card-text>
					<v-text-field v-model="props.remote.url" label="Server URL" placeholder="https://your-server.com" density="compact" class="mb-2" autocomplete="url" />
					<v-text-field v-model="props.remote.token" label="Token" placeholder="optional" density="compact" type="password" autocomplete="current-password" />
				</v-card-text>
				<v-card-actions class="px-4 pb-4">
					<v-spacer />
					<v-btn color="primary" variant="tonal" :loading="remoteSaving" type="submit">
						Save
					</v-btn>
				</v-card-actions>
			</form>
		</v-card>

	</div>
</template>
