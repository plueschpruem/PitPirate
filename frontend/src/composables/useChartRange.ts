import { ref, watch } from 'vue'

// Module-level singleton — shared across ALL ProbeChart instances on the page.
// Changing selectedHours in any chart instantly syncs every other chart.

export const RANGES = [
	{ label: '30min', hours: 0.5},
	{ label: '1hr',   hours: 1  },
	{ label: '2hrs',  hours: 2  },
	{ label: '4hrs',  hours: 4  },
	{ label: '12hrs', hours: 12 },
	{ label: '24hrs', hours: 24 },
	{ label: 'All',   hours: 0  },
] as const

const STORAGE_KEY = 'pitpirate_chart_range'

function load(): { hours: number; height: number } {
	try { return JSON.parse(localStorage.getItem(STORAGE_KEY) ?? '{}') } catch { return {} as any }
}

const stored = load()
const validHours = RANGES.map(r => r.hours)

export const selectedHeight = ref<number>(
	(typeof stored.height === 'number' && stored.height >= 200 && stored.height <= 700)
		? stored.height
		: 300
)
export const selectedHours = ref<number>(
	validHours.includes(stored.hours as any) ? stored.hours : 12
)

watch([selectedHours, selectedHeight], ([hours, height]) => {
	localStorage.setItem(STORAGE_KEY, JSON.stringify({ hours, height }))
})
