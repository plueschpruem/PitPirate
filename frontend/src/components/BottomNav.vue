<script setup lang="ts">
import { computed } from 'vue'

const model = defineModel<'probes' | 'settings'>({ default: 'probes' })

// Guard against Vuetify emitting a numeric index for indicator buttons
// (buttons without a value="" prop get assigned their 0-based index as value)
const navValue = computed({
	get: () => model.value,
	set: (v: unknown) => {
		if (v === 'probes' || v === 'settings') model.value = v
	},
})

const props = defineProps<{
	battery: number | null,
	rssi: number | null,
	fanPct: number | null,
	hasAlert: boolean,
	hasError: boolean,
}>()

const fanColor = computed(() => {
	if (props.fanPct) {
		return (props.fanPct >= 1 && props.fanPct < 40) ? 'green'
			: (props.fanPct >= 40 && props.fanPct < 60) ? 'orange'
				: (props.fanPct >= 60) ? 'red' : 'gray'
	}
});

const wifiIcon = computed(() => {
	if (props.rssi) {
		return props.rssi <= -85 ? 'wifi-strength-outline' :
			props.rssi <= -75 ? 'wifi-strength-1' :
				props.rssi <= -67 ? 'wifi-strength-2' :
					props.rssi <= -50 ? 'wifi-strength-3' : 'wifi-strength-4'
	}
});

const wifiColor = computed(() => {
	if (props.rssi) {
		return props.rssi <= -85 ? 'danger' :
			props.rssi <= -75 ? 'warning' :
				props.rssi <= -67 ? 'blue' :
					props.rssi <= -50 ? 'green' : 'green'
	}
});
</script>

<template>
	<v-bottom-navigation v-model="navValue" color="primary" grow class="w-100">
		<v-btn value="probes">
			<v-icon icon="thermometer" />
			<span>Probes</span>
		</v-btn>
		<v-btn value="settings">
			<v-icon icon="cog" />
			<span>Settings</span>
		</v-btn>

		<v-divider vertical />

		<!-- —————————— FAN —————————— -->
		<v-btn style="pointer-events:none" tabindex="-1" class="flex-grow-0">
			<v-icon class="mt-1" size="25" :icon="(fanPct != 0) ? 'fan' : 'fan-off'" :color="fanColor" />
			<v-chip :text="`${fanPct}%`" size="xsmall" variant="tonal" class="px-1" :color="fanColor" />
		</v-btn>

		<v-divider vertical />

		<!-- —————————— WIFI —————————— -->
		<v-btn v-if="rssi && (!hasAlert && !hasError)" style="pointer-events:none" tabindex="-1" class="flex-grow-0">
			<v-icon size="25" :icon="wifiIcon" :color="wifiColor" />
			<v-chip :text="`${rssi} dB`" size="xsmall" variant="tonal" class="px-1" :color="wifiColor" />
		</v-btn>
		<v-btn v-else-if="hasAlert || hasError" style="pointer-events:none" tabindex="-1" class="flex-grow-0">
			<v-icon size="40" icon="wifi-strength-alert-outline" color="red" class="warning-pulse" />
		</v-btn>

		<v-divider vertical />

		<!-- —————————— BATTERY —————————— -->
		<v-btn v-if="battery !== null && (!hasAlert && !hasError)" style="pointer-events:none" tabindex="-1" class="flex-grow-0">
			<v-icon size="40" :style="`color: var(--battery--${battery})`" :icon="battery <= 33 ? 'battery-low' : battery <= 66 ? 'battery-medium' : 'battery-high'" />
		</v-btn>
		<v-btn v-else-if="hasAlert || hasError" style="pointer-events:none" tabindex="-1" class="flex-grow-0">
			<v-icon size="40" icon="alert" color="red" class="warning-pulse" />
		</v-btn>
	</v-bottom-navigation>
</template>
