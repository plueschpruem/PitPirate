import { ref, watch } from 'vue'

const STORAGE_KEY = 'pitpirate_settings'

function loadStored(): Record<string, unknown> {
	try { return JSON.parse(localStorage.getItem(STORAGE_KEY) ?? '{}') } catch { return {} }
}

const s = loadStored()

// ── Settings refs ─────────────────────────────────────────────────────────────

/** How often the live probe temperature tiles refresh (ms) */
export const probePollMs = ref<number>(typeof s.probePollMs === 'number' ? s.probePollMs : 5000)

/** How often each chart fetches new data points (ms) */
export const chartPollMs = ref<number>(typeof s.chartPollMs === 'number' ? s.chartPollMs : 10000)

/** Max downsampled datapoints returned per chart request */
export const chartSamples = ref<number>(typeof s.chartSamples === 'number' ? s.chartSamples : 500)

/** Render chart lines as smooth Bézier curves */
export const smoothLine = ref<boolean>(typeof s.smoothLine === 'boolean' ? s.smoothLine : true)

/** Stale-data alarm threshold (ms). 0 = disabled */
export const staleDataMs = ref<number>(typeof s.staleDataMs === 'number' ? s.staleDataMs : 60000)

/** Preset app content width for desktop browsers */
export const appWidth = ref<number>(typeof s.appWidth === 'number' ? s.appWidth : 700)

// ── Option lists (used by SettingsView chips) ─────────────────────────────────

export const PROBE_POLL_OPTIONS = [
	{ label: '2s',  ms: 2000  },
	{ label: '5s',  ms: 5000  },
	{ label: '10s', ms: 10000 },
	{ label: '30s', ms: 30000 },
] as const

export const CHART_POLL_OPTIONS = [
	{ label: '10s', ms: 10000  },
	{ label: '30s', ms: 30000  },
	{ label: '1m',  ms: 60000  },
	{ label: '5m',  ms: 300000 },
] as const

export const CHART_SAMPLES_OPTIONS = [
	{ label: '100',  n: 100  },
	{ label: '250',  n: 250  },
	{ label: '500',  n: 500  },
	{ label: '1000', n: 1000 },
] as const

export const STALE_DATA_OPTIONS = [
	{ label: 'Off', ms: 0      },
	{ label: '30s', ms: 30000  },
	{ label: '60s', ms: 60000  },
	{ label: '90s', ms: 90000  },
	{ label: '2m',  ms: 120000 },
] as const

// ── Auto-persist on any change ────────────────────────────────────────────────
watch([probePollMs, chartPollMs, chartSamples, smoothLine, staleDataMs, appWidth], () => {
	localStorage.setItem(STORAGE_KEY, JSON.stringify({
		probePollMs:  probePollMs.value,
		chartPollMs:  chartPollMs.value,
		chartSamples: chartSamples.value,
		smoothLine:   smoothLine.value,
		staleDataMs:  staleDataMs.value,
		appWidth:  	  appWidth.value,
	}))
})
