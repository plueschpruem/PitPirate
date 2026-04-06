<script setup lang="ts">
import { ref, computed, watch } from 'vue'
import { getAlarmConfig, saveAlarmConfig } from '@/api/pitpirate'

const props = defineProps<{
	modelValue: boolean
	probeId: string
	color: string
}>()

const emit = defineEmits<{
	(e: 'update:modelValue', val: boolean): void
	(e: 'saved'): void
}>()

const MIN_GAP = 10

const maxTemp = computed(() => props.probeId === '7' ? 600 : 200)
const probeName = computed(() =>
	props.probeId === '7' ? `Ambient ${props.probeId}` : `Probe ${props.probeId}`
)

const lo = ref(0)
const hi = ref(0)
const saving = ref(false)
const loading = ref(false)

watch(() => props.modelValue, async (open) => {
	if (!open) return
	loading.value = true
	try {
		const config = await getAlarmConfig()
		const entry = config[props.probeId]
		lo.value = entry?.lo ?? 0
		hi.value = entry?.hi ?? 0
	} finally {
		loading.value = false
	}
})

// Enforce minimum gap: when lo moves up, push hi up with it
watch(lo, (newLo) => {
	if (newLo > 0 && hi.value > 0 && hi.value < newLo + MIN_GAP) {
		hi.value = Math.min(newLo + MIN_GAP, maxTemp.value)
	}
})
// Enforce minimum gap: when hi moves down, push lo down with it
watch(hi, (newHi) => {
	if (newHi > 0 && lo.value > 0 && lo.value > newHi - MIN_GAP) {
		lo.value = Math.max(newHi - MIN_GAP, 1)
	}
})

function tempLabel(val: number) {
	return val === 0 ? 'OFF' : `${val}°`
}

async function save() {
	saving.value = true
	try {
		await saveAlarmConfig(props.probeId, lo.value, hi.value)
		emit('saved')
		emit('update:modelValue', false)
	} finally {
		saving.value = false
	}
}

function close() {
	emit('update:modelValue', false)
}
</script>

<template>
	<v-bottom-sheet :model-value="modelValue" @update:model-value="emit('update:modelValue', $event as boolean)">
		<v-card rounded="t-xl" class="pb-2 mx-4">
			<!-- ── Header ───────────────────────────────────────────────────── -->
			<v-card-title class="pt-5 px-6">
				<v-icon icon="alarm" :color="color" class="mr-2" />
				{{ probeName }} — Alarm Limits
			</v-card-title>
			<v-card-subtitle class="px-6 mb-2">
				Set a limit to 0 to disable it (shown as OFF).
				When both are active, high must be at least {{ MIN_GAP }}° above low.
			</v-card-subtitle>

			<v-divider />

			<!-- ── Sliders ──────────────────────────────────────────────────── -->
			<v-card-text class="px-6 py-3">
				<v-progress-linear v-if="loading" indeterminate :color="color" class="mb-4" />

				<!-- Low alarm -->
				<div class="mb-0">
					<div class="d-flex justify-space-between align-baseline mb-1">
						<span class="text-body-2 text-medium-emphasis">
							Low alarm
						</span>
						<span class="text-h6 font-weight-bold" :style="lo > 0 ? `color: #2196F3` : ''">{{ tempLabel(lo) }}</span>
					</div>
					<div class="d-flex">
						<v-icon icon="thermometer-chevron-down" size="30" class="mr-3" color="blue" />
						<v-slider v-model="lo" :min="0" :max="maxTemp" :step="1" color="blue" :disabled="loading" thumb-label="hover" :thumb-size="20" track-color="blue" class="alarm-slider">
							<template #thumb-label="{ modelValue: v }">
								{{ v === 0 ? 'OFF' : v }}
							</template>
						</v-slider>
					</div>
				</div>

				<!-- High alarm -->
				<div class="mb-0">
					<div class="d-flex justify-space-between align-baseline mb-1">
						<span class="text-body-2 text-medium-emphasis">
							High alarm
						</span>
						<span class="text-h6 font-weight-bold" :style="hi > 0 ? `color: #F44336` : ''">{{ tempLabel(hi) }}</span>
					</div>
					<div class="d-flex">
						<v-icon icon="thermometer-chevron-up" size="30" class="mr-3" color="red" />
						<v-slider v-model="hi" :min="0" :max="maxTemp" :step="1" color="red" :disabled="loading" thumb-label="hover" :thumb-size="20" track-color="red" class="alarm-slider">
							<template #thumb-label="{ modelValue: v }">
								<v-chip variant="elevated"> {{ v === 0 ? 'OFF' : v }}</v-chip>
							</template>
						</v-slider>
					</div>
				</div>
			</v-card-text>
			<v-divider />
			<!-- ── Actions ──────────────────────────────────────────────────── -->
			<v-card-actions class="px-6 pb-4">
				<v-btn variant="tonal" @click="close">Cancel</v-btn>
				<v-spacer />
				<v-btn :color="color" variant="elevated" :loading="saving" @click="save">
					Save
				</v-btn>
			</v-card-actions>
		</v-card>
	</v-bottom-sheet>
</template>

<style scoped>
.alarm-slider :deep(.v-slider-thumb__label) {
	font-size: 11px;
	font-weight: 700;
	min-width: 32px;
	text-align: center;
}

.alarm-slider :deep(.v-input__details) {
	display: none;
}
</style>
