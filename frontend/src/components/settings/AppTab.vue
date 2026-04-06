<script setup lang="ts">
import { ref, computed, onMounted } from 'vue'
import {
	probePollMs, PROBE_POLL_OPTIONS,
	chartPollMs, CHART_POLL_OPTIONS,
	chartSamples, CHART_SAMPLES_OPTIONS,
	smoothLine,
	staleDataMs, STALE_DATA_OPTIONS,
	appWidth,
} from '@/composables/useAppSettings'
import { getFanConfig, saveFanSpeed, saveFanSettings } from '@/api/pitpirate'

// ── Fan speed ─────────────────────────────────────────────────────────────────

const fanSaving    = ref(false)
const fanError     = ref('')
const fanSlider    = ref(0)
const fanStartPct  = ref(40)
const fanMinPct    = ref(25)
const fanSetSaving = ref(false)

async function loadFanConfig() {
	try {
		const cfg = await getFanConfig()
		fanSlider.value   = cfg.pct
		fanStartPct.value = cfg.start
		fanMinPct.value   = cfg.min
	} catch { /* older firmware without fan support */ }
}

const fanSliderTicks = computed(() => ({
	0: 'Off',
	[fanMinPct.value]: 'Min',
	100: 'Max',
}))

function onFanSliderChange(v: number | string) {
	const n = Number(v)
	if (n > 0 && n < fanMinPct.value) fanSlider.value = fanMinPct.value
}

function onAppWidthChange(v: number | string) {
	const n = Number(v)
	document.documentElement.style.setProperty("--container-main-width", `${n}px`);
}

async function doSaveFan() {
	fanSaving.value = true
	fanError.value  = ''
	try {
		await saveFanSpeed(fanSlider.value)
	} catch (e: unknown) {
		fanError.value = (e as Error).message
	} finally {
		fanSaving.value = false
	}
}

async function doSaveFanSettings() {
	if (fanMinPct.value > fanStartPct.value) {
		fanError.value = 'Minimum speed must be ≤ start speed'
		return
	}
	fanSetSaving.value = true
	fanError.value     = ''
	try {
		await saveFanSettings(fanStartPct.value, fanMinPct.value)
	} catch (e: unknown) {
		fanError.value = (e as Error).message
	} finally {
		fanSetSaving.value = false
	}
}

onMounted(loadFanConfig)
</script>

<template>
	<div>

		<v-card rounded="lg">
			<v-card-text class="px-4">

				<div class="mb-4">
					<div class="text-caption text-medium-emphasis mb-2">Live data refresh</div>
					<div class="d-flex ga-2 flex-wrap">
						<v-chip
							v-for="o in PROBE_POLL_OPTIONS" :key="o.ms"
							:variant="probePollMs === o.ms ? 'elevated' : 'tonal'"
							size="small" @click="probePollMs = o.ms"
						>{{ o.label }}</v-chip>
					</div>
				</div>

				<div class="mb-4">
					<div class="text-caption text-medium-emphasis mb-2">Chart data refresh</div>
					<div class="d-flex ga-2 flex-wrap">
						<v-chip
							v-for="o in CHART_POLL_OPTIONS" :key="o.ms"
							:variant="chartPollMs === o.ms ? 'elevated' : 'tonal'"
							size="small" @click="chartPollMs = o.ms"
						>{{ o.label }}</v-chip>
					</div>
				</div>

				<div class="mb-4">
					<div class="text-caption text-medium-emphasis mb-2">Chart data points</div>
					<div class="d-flex ga-2 flex-wrap">
						<v-chip
							v-for="o in CHART_SAMPLES_OPTIONS" :key="o.n"
							:variant="chartSamples === o.n ? 'elevated' : 'tonal'"
							size="small" @click="chartSamples = o.n"
						>{{ o.label }}</v-chip>
					</div>
				</div>

				<v-switch
					v-model="smoothLine"
					label="Smooth chart line"
					density="compact"
					hide-details
					color="primary"
				/>

				<div class="mt-4">
					<div class="text-caption text-medium-emphasis mb-2">Stale data alarm</div>
					<div class="d-flex ga-2 flex-wrap">
						<v-chip
							v-for="o in STALE_DATA_OPTIONS" :key="o.ms"
							:variant="staleDataMs === o.ms ? 'elevated' : 'tonal'"
							size="small" @click="staleDataMs = o.ms"
						>{{ o.label }}</v-chip>
					</div>
				</div>

			</v-card-text>
		</v-card>

		<!-- App Width -->
		<v-card class="mt-3" rounded="lg">
			<v-card-title class="text-body-1 font-weight-bold pt-4 px-4">
				<v-icon icon="arrow-expand-horizontal" size="18" class="mr-2 mb-1" />
				App Width
			</v-card-title>
			<v-card-text>
				<v-slider
					label="Width"
					v-model="appWidth"
					:min="500" :max="1200" :step="50"
					color="blue"
					track-size="2"
					track-color="#666"
					track-fill-color="blue"
					thumb-label
					density="compact"
					class="mb-3"
					hint="Set app width on desktop screens"
					@update:model-value="onAppWidthChange"
				/>
			</v-card-text>
		</v-card>

		<!-- Fan Speed -->
		<v-card class="mt-3" rounded="lg">
			<v-card-title class="text-body-1 font-weight-bold pt-4 px-4">
				<v-icon icon="fan" size="18" class="mr-2 mb-1" />
				Fan Speed
			</v-card-title>
			<v-card-text>
				<v-slider
					label="Set Speed"
					v-model="fanSlider"
					:min="0" :max="100" :step="1"
					color="primary"
					thumb-label="hover"
					show-ticks="always"
					tick-size="5"
					track-size="2"
					track-color="#666"
					track-fill-color="primary"
					:ticks="fanSliderTicks"
					:thumb-color="(fanSlider == 0)?'red':'primary'"
					class="mb-1"
					@update:model-value="onFanSliderChange"
					density="compact"
					hint="Set the actual fan speed"
				/>
				<v-slider
					label="Start Speed"
					v-model="fanStartPct"
					:min="0" :max="100" :step="1"
					color="blue"
					track-size="2"
					track-color="#666"
					track-fill-color="blue"
					thumb-label
					density="compact"
					class="mb-3"
					hint="Minimum speed to get the motor running"
				/>
				<v-slider
					label="Minimum speed"
					v-model="fanMinPct"
					:min="0" :max="100" :step="1"
					color="teal"
					track-size="2"
					track-color="#666"
					track-fill-color="teal"
					thumb-label
					class="mb-1"
					hint="Lowest sustained speed once running"
				/>

				<v-alert
					v-if="fanError"
					color="error"
					variant="tonal"
					density="compact"
					class="mt-2"
				>{{ fanError }}</v-alert>
			</v-card-text>
			<v-card-actions class="px-4 pb-4">
				<v-btn
					variant="tonal"
					prepend-icon="refresh"
					@click="loadFanConfig"
				>Reload</v-btn>
				<v-spacer />
				<v-btn
					variant="tonal"
					prepend-icon="tune"
					:loading="fanSetSaving"
					@click="doSaveFanSettings"
				>Save Thresholds</v-btn>
				<v-btn
					color="primary"
					variant="tonal"
					prepend-icon="content-save"
					:loading="fanSaving"
					@click="doSaveFan"
				>Apply Speed</v-btn>
			</v-card-actions>
		</v-card>

	</div>
</template>
