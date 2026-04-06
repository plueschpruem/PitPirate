<script setup lang="ts">
import { ref, computed, watch, onMounted, onUnmounted } from 'vue'
import { selectedHours, selectedHeight } from '../composables/useChartRange'
import { chartPollMs, chartSamples, smoothLine } from '../composables/useAppSettings'

const props = defineProps<{
	probeId: string
	serverUrl: string
	color: string
	lo?: number
	hi?: number
}>()

interface HistoryPoint { ts: number; temp: number }

const loading = ref(true)
const error = ref('')
const history = ref<HistoryPoint[]>([])
const lastTs = ref<number | null>(null)

function fmtTime(ts: number) {
	const d = new Date(ts * 1000)
	return `${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`
}

// ── Layout ────────────────────────────────────────────────────────────────────

const wrapperEl = ref<HTMLDivElement | null>(null)
const containerWidth = ref(600)
let ro: ResizeObserver | null = null

const PAD_LEFT = 32
const PAD_RIGHT = 16
const PAD_TOP = 16
const PAD_BOTTOM = 40

const svgH = computed(() => selectedHeight.value)
const plotW = computed(() => Math.max(10, containerWidth.value - PAD_LEFT - PAD_RIGHT))
const plotH = computed(() => svgH.value - PAD_TOP - PAD_BOTTOM)

// ── Y scale / nice ticks ──────────────────────────────────────────────────────

const dataMin = computed(() => {
	if (history.value.length === 0) return 0
	const vals = history.value.map(p => p.temp)
	if ((props.lo ?? 0) > 0) vals.push(props.lo!)
	return Math.min(...vals)
})
const dataMax = computed(() => {
	if (history.value.length === 0) return 100
	const vals = history.value.map(p => p.temp)
	if ((props.hi ?? 0) > 0) vals.push(props.hi!)
	return Math.max(...vals)
})

function niceStep(range: number, targetTicks = 8): number {
	if (range === 0) return 1
	const rough = range / targetTicks
	const pow10 = Math.pow(10, Math.floor(Math.log10(rough)))
	const norm = rough / pow10
	const nice = norm < 1.5 ? 1 : norm < 3 ? 2 : norm < 7 ? 5 : 10
	return nice * pow10
}

const yTargetTicks = computed(() => Math.max(2, Math.floor(plotH.value / 40)))
const yStep = computed(() => niceStep(dataMax.value - dataMin.value, yTargetTicks.value))
const yTickMin = computed(() => Math.floor(dataMin.value / yStep.value) * yStep.value)
const yTickMax = computed(() => Math.ceil(dataMax.value / yStep.value) * yStep.value)
const yRange = computed(() => yTickMax.value - yTickMin.value || 1)

const yTicks = computed(() => {
	const ticks: number[] = []
	for (let v = yTickMin.value; v <= yTickMax.value + 1e-9; v += yStep.value) {
		ticks.push(Math.round(v))
	}
	return ticks
})

function yPx(temp: number): number {
	return PAD_TOP + plotH.value - ((temp - yTickMin.value) / yRange.value) * plotH.value
}

// ── X scale / time labels ─────────────────────────────────────────────────────

// Anchor the x-axis to the selected time window so data starts at the correct
// horizontal position (e.g. 4hrs of data in a 24hr range starts at 4/6 width).
const tsMax = computed(() => Math.floor(Date.now() / 1000))
const tsMin = computed(() => {
	if (selectedHours.value > 0) return tsMax.value - selectedHours.value * 3600
	// "All" mode: span the actual data
	return history.value[0]?.ts ?? tsMax.value - 1
})
const tsRange = computed(() => tsMax.value - tsMin.value || 1)

function xPx(ts: number): number {
	return PAD_LEFT + ((ts - tsMin.value) / tsRange.value) * plotW.value
}

const xTicks = computed(() => {
	if (history.value.length < 2) return []
	const count = 6
	return Array.from({ length: count + 1 }, (_, i) => {
		const ts = tsMin.value + (i / count) * tsRange.value
		return { x: xPx(ts), label: fmtTime(ts) }
	})
})

// ── SVG paths ─────────────────────────────────────────────────────────────────

function buildLinePath(pts: HistoryPoint[]): string {
	if (pts.length === 0) return ''
	if (!smoothLine.value || pts.length < 3) {
		return pts
			.map((p, i) => `${i === 0 ? 'M' : 'L'}${xPx(p.ts).toFixed(1)},${yPx(p.temp).toFixed(1)}`)
			.join(' ')
	}
	// Catmull-Rom → cubic Bézier
	const tension = 0.3
	const cmds: string[] = [`M${xPx(pts[0].ts).toFixed(1)},${yPx(pts[0].temp).toFixed(1)}`]
	for (let i = 1; i < pts.length; i++) {
		const p0 = pts[Math.max(0, i - 2)]
		const p1 = pts[i - 1]
		const p2 = pts[i]
		const p3 = pts[Math.min(pts.length - 1, i + 1)]
		const cp1x = xPx(p1.ts) + (xPx(p2.ts) - xPx(p0.ts)) * tension
		const cp1y = yPx(p1.temp) + (yPx(p2.temp) - yPx(p0.temp)) * tension
		const cp2x = xPx(p2.ts) - (xPx(p3.ts) - xPx(p1.ts)) * tension
		const cp2y = yPx(p2.temp) - (yPx(p3.temp) - yPx(p1.temp)) * tension
		cmds.push(`C${cp1x.toFixed(1)},${cp1y.toFixed(1)} ${cp2x.toFixed(1)},${cp2y.toFixed(1)} ${xPx(p2.ts).toFixed(1)},${yPx(p2.temp).toFixed(1)}`)
	}
	return cmds.join(' ')
}

const linePath = computed(() => buildLinePath(history.value))

const areaPath = computed(() => {
	const pts = history.value
	if (pts.length === 0) return ''
	const base = (PAD_TOP + plotH.value).toFixed(1)
	const lastX = xPx(pts[pts.length - 1].ts).toFixed(1)
	const firstX = xPx(pts[0].ts).toFixed(1)
	return `${linePath.value} L${lastX},${base} L${firstX},${base} Z`
})

// ── Tooltip ───────────────────────────────────────────────────────────────────

interface TooltipState { px: number; py: number; temp: number; ts: number }
const tooltip = ref<TooltipState | null>(null)

function onMouseMove(e: MouseEvent) {
	const svg = wrapperEl.value?.querySelector('svg')
	if (!svg) return
	const rect = svg.getBoundingClientRect()
	const mouseX = e.clientX - rect.left
	const pts = history.value
	if (pts.length === 0) return

	// Binary-search nearest point by x pixel
	let lo = 0, hi = pts.length - 1
	while (lo < hi) {
		const mid = (lo + hi) >> 1
		if (xPx(pts[mid].ts) < mouseX) lo = mid + 1
		else hi = mid
	}
	// Compare lo and lo-1
	const best = (lo > 0 && Math.abs(xPx(pts[lo - 1].ts) - mouseX) < Math.abs(xPx(pts[lo].ts) - mouseX))
		? pts[lo - 1]
		: pts[lo]

	tooltip.value = { px: xPx(best.ts), py: yPx(best.temp), temp: best.temp, ts: best.ts }
}

function onMouseLeave() { tooltip.value = null }

// Flip tooltip box to the left when it would overflow the right edge
const tooltipFlip = computed(() =>
	tooltip.value !== null && tooltip.value.px + 8 + 78 > containerWidth.value
)
const tooltipBoxX = computed(() =>
	tooltipFlip.value ? (tooltip.value!.px - 8 - 78) : (tooltip.value!.px + 8)
)

// ── Data fetching (identical to ProbeChart.vue) ───────────────────────────────

async function load(showSpinner = true) {
	if (!props.serverUrl) {
		error.value = 'No server endpoint set in Settings'
		loading.value = false
		return
	}
	if (showSpinner) loading.value = true
	error.value = ''
	try {
		const base = props.serverUrl.replace(/\/+$/, '')
		const now = Math.floor(Date.now() / 1000)
		let rangeParam: string
		if (lastTs.value !== null) {
			rangeParam = `&since=${lastTs.value}`
		} else if (selectedHours.value > 0) {
			rangeParam = `&since=${now - selectedHours.value * 3600}&samples=${chartSamples.value}`
		} else {
			rangeParam = `&limit=8640&samples=${chartSamples.value}`
		}
		const res = await fetch(
			`${base}/php/telemetry_get.php?probe=${encodeURIComponent(props.probeId)}${rangeParam}&_=${Date.now()}`
		)
		if (!res.ok) throw new Error(`HTTP ${res.status}`)
		const raw: HistoryPoint[] = await res.json()
		const fresh = raw.filter(p => p.ts > 0).sort((a, b) => a.ts - b.ts)
		let updated = lastTs.value === null ? fresh : [...history.value, ...fresh]
		if (selectedHours.value > 0) {
			const cutoff = now - selectedHours.value * 3600
			updated = updated.filter(p => p.ts >= cutoff)
		}
		history.value = updated
		if (fresh.length > 0) lastTs.value = fresh[fresh.length - 1].ts
	} catch (e: unknown) {
		error.value = (e as Error).message
	} finally {
		loading.value = false
	}
}

watch(selectedHours, () => { history.value = []; lastTs.value = null; load(true) })
watch(chartSamples, () => { history.value = []; lastTs.value = null; load(true) })
watch(chartPollMs, (ms) => {
	if (timer) { clearInterval(timer); timer = null }
	timer = setInterval(() => load(false), ms)
})

let timer: ReturnType<typeof setInterval> | null = null

onMounted(() => {
	load()
	timer = setInterval(() => load(false), chartPollMs.value)
	if (wrapperEl.value) {
		ro = new ResizeObserver(entries => { containerWidth.value = entries[0].contentRect.width })
		ro.observe(wrapperEl.value)
		containerWidth.value = wrapperEl.value.clientWidth
	}
})
onUnmounted(() => {
	if (timer) clearInterval(timer)
	if (ro) ro.disconnect()
})
</script>

<template>
	<div ref="wrapperEl" class="probe-chart pchart" @mousemove="onMouseMove" @mouseleave="onMouseLeave">
		<div v-if="loading" class="probe-chart__state">Loading chart data…</div>
		<div v-else-if="error" class="probe-chart__state text-caption">{{ error }}</div>
		<div v-else-if="history.length === 0" class="probe-chart__state text-caption">No data yet</div>
		<svg v-else class="pchart__svg" :width="containerWidth" :height="svgH">
			<defs>
				<!-- clip plot area so paths don't bleed over axis labels -->
				<clipPath :id="`pchart-clip-${probeId}`">
					<rect :x="PAD_LEFT" :y="PAD_TOP" :width="plotW" :height="plotH" />
				</clipPath>
				<!-- vertical gradient for the area fill -->
				<linearGradient :id="`pchart-grad-${probeId}`" x1="0" y1="0" x2="0" y2="1">
					<stop offset="0%" :stop-color="color" stop-opacity="0.40" />
					<stop offset="100%" :stop-color="color" stop-opacity="0.03" />
				</linearGradient>
			</defs>

			<!-- ── Horizontal grid lines ── -->
			<g class="pchart__grid">
				<line v-for="tick in yTicks" :key="tick" class="pchart__grid-line" :x1="PAD_LEFT" :y1="yPx(tick)" :x2="PAD_LEFT + plotW" :y2="yPx(tick)" :stroke="color" />
			</g>

			<!-- ── Y-axis labels ── -->
			<g class="pchart__y-axis">
				<text v-for="tick in yTicks" :key="tick" class="pchart__y-label" :x="PAD_LEFT - 8" :y="yPx(tick)" :fill="color">{{ tick }}</text>
			</g>

			<!-- ── X-axis labels ── -->
			<g class="pchart__x-axis">
				<text v-for="xt in xTicks" :key="xt.x" class="pchart__x-label" :x="xt.x" :y="PAD_TOP + plotH + 26" :fill="color">{{ xt.label }}</text>
			</g>

			<!-- ── Area fill + line (clipped) ── -->
			<g class="pchart__plot" :clip-path="`url(#pchart-clip-${probeId})`">
				<path class="pchart__area" :d="areaPath" :fill="`url(#pchart-grad-${probeId})`" />
				<path class="pchart__line" :d="linePath" :stroke="color" />
			</g>

			<!-- ── Lo alarm line ── -->
			<g v-if="(lo ?? 0) > 0" class="pchart__alarm pchart__alarm--lo">
				<line class="pchart__alarm-line" :x1="PAD_LEFT" :y1="yPx(lo!)" :x2="PAD_LEFT + plotW" :y2="yPx(lo!)" />
				<text class="pchart__alarm-label" :x="PAD_LEFT + 10" :y="yPx(lo!) - 6">▼ {{ lo }}°</text>
			</g>

			<!-- ── Hi alarm line ── -->
			<g v-if="(hi ?? 0) > 0" class="pchart__alarm pchart__alarm--hi">
				<line class="pchart__alarm-line" :x1="PAD_LEFT" :y1="yPx(hi!)" :x2="PAD_LEFT + plotW" :y2="yPx(hi!)" />
				<text class="pchart__alarm-label pchart__alarm-label--hi" :x="PAD_LEFT + plotW - 10" :y="yPx(hi!) + 20">▲ {{ hi }}°</text>
			</g>

			<!-- ── Tooltip ── -->
			<g v-if="tooltip" class="pchart__tooltip">
				<line class="pchart__crosshair" :x1="tooltip.px" :y1="PAD_TOP" :x2="tooltip.px" :y2="PAD_TOP + plotH" />
				<circle class="pchart__dot" :cx="tooltip.px" :cy="tooltip.py" r="4" :fill="color" />
				<rect class="pchart__tooltip-box" :x="tooltipBoxX" :y="tooltip.py - 34" width="78" height="44" rx="4" />
				<text class="pchart__tooltip-time" :x="tooltipBoxX + 39" :y="tooltip.py - 18">{{ fmtTime(tooltip.ts) }}</text>
				<line class="pchart__tooltip-divider" :x1="tooltipBoxX + 4" :y1="tooltip.py - 13" :x2="tooltipBoxX + 74" :y2="tooltip.py - 13" />
				<text class="pchart__tooltip-value" :x="tooltipBoxX + 39" :y="tooltip.py + 3">{{ tooltip.temp }}°</text>
			</g>

			<!-- ── Invisible overlay to capture mouse events across whole plot area ── -->
			<rect class="pchart__overlay" :x="PAD_LEFT" :y="PAD_TOP" :width="plotW" :height="plotH" />
		</svg>
	</div>
</template>

<style scoped>
/* all in main.scss */
</style>
