<script setup lang="ts">
import { ref, computed, onMounted } from 'vue'
import type { RemoteSettings } from '@/api/pitpirate'
import { listSessions, startNewSession } from '@/api/pitpirate'

const props = defineProps<{
	remote: RemoteSettings
}>()

const sessions         = ref<string[]>([])
const sessionsLoading  = ref(false)
const sessionsError    = ref('')
const sessionsBusy     = ref(false)
const confirmDialog    = ref(false)
const lastArchivedName = ref('')

// Base URL: empty when server-hosted (relative URLs work),
// otherwise use the configured remote server URL so it works from the ESP32 app too.
const sessionsBase = computed(() => {
	if (typeof window !== 'undefined' && window.location.protocol === 'https:') return ''
	try { return new URL(props.remote.url).origin } catch { return '' }
})

async function loadSessions() {
	sessionsLoading.value = true
	sessionsError.value   = ''
	try {
		sessions.value = await listSessions(sessionsBase.value)
	} catch (e: unknown) {
		sessionsError.value = (e as Error).message
	} finally {
		sessionsLoading.value = false
	}
}

async function doStartNewSession() {
	confirmDialog.value    = false
	sessionsBusy.value     = true
	sessionsError.value    = ''
	lastArchivedName.value = ''
	try {
		const result = await startNewSession(sessionsBase.value)
		lastArchivedName.value = result.name
		await loadSessions()
	} catch (e: unknown) {
		sessionsError.value = (e as Error).message
	} finally {
		sessionsBusy.value = false
	}
}

function formatSessionName(filename: string): string {
	// "23-11-2026_09-47_24-11-2026_12-33.ndjson" → "23 Nov 2026  09:47  →  24 Nov 2026  12:33"
	const m = filename.match(
		/^(\d{2})-(\d{2})-(\d{4})_(\d{2})-(\d{2})_(\d{2})-(\d{2})-(\d{4})_(\d{2})-(\d{2})/
	)
	if (!m) return filename.replace(/\.ndjson$/, '')
	const months = ['Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec']
	const mo1 = months[parseInt(m[2]) - 1] ?? m[2]
	const mo2 = months[parseInt(m[8]) - 1] ?? m[8]
	return `${m[1]} ${mo1} ${m[3]}  ${m[4]}:${m[5]}  →  ${m[6]} ${mo2} ${m[9]}  ${m[10]}:${m[11]}`
}

onMounted(loadSessions)
</script>

<template>
	<div>

		<!-- Explanation -->
		<v-card rounded="lg" class="mb-3">
			<v-card-text class="px-4 text-body-2">
				Starting a new session <strong>archives</strong> the current log and begins collecting
				fresh data. The archive is named after the date range of its entries.
			</v-card-text>
			<v-card-actions class="px-4 pb-4">
				<v-spacer />
				<v-btn
					color="primary"
					variant="tonal"
					prepend-icon="archive-arrow-down"
					:loading="sessionsBusy"
					@click="confirmDialog = true"
				>Start New Session</v-btn>
			</v-card-actions>
		</v-card>

		<!-- Success banner -->
		<v-alert
			v-if="lastArchivedName"
			color="success"
			variant="tonal"
			density="compact"
			class="mb-3"
			closable
			@click:close="lastArchivedName = ''"
		>
			Archived as <strong>{{ lastArchivedName }}</strong>
		</v-alert>

		<!-- Error banner -->
		<v-alert
			v-if="sessionsError"
			color="error"
			variant="tonal"
			density="compact"
			class="mb-3"
		>{{ sessionsError }}</v-alert>

		<!-- Archive list -->
		<v-card rounded="lg">
			<v-card-title class="text-body-1 font-weight-bold pt-4 px-4">
				<v-icon icon="archive" size="18" class="mr-2 mb-1" />
				Archived Sessions
			</v-card-title>
			<v-card-text class="px-2">
				<div v-if="sessionsLoading" class="d-flex justify-center pa-4">
					<v-progress-circular indeterminate size="24" />
				</div>
				<v-list v-else-if="sessions.length" density="compact">
					<v-list-item
						v-for="s in sessions" :key="s"
						:subtitle="s"
						:title="formatSessionName(s)"
						prepend-icon="file-document-outline"
					/>
				</v-list>
				<div v-else class="text-caption text-medium-emphasis pa-4 text-center">
					No archived sessions yet.
				</div>
			</v-card-text>
		</v-card>

		<!-- Confirmation dialog -->
		<v-dialog v-model="confirmDialog" max-width="340">
			<v-card rounded="lg">
				<v-card-title class="text-body-1 font-weight-bold pt-4 px-4">
					Start a new session?
				</v-card-title>
				<v-card-text class="px-4">
					The current log will be <strong>archived</strong> and a fresh log will begin.
					This cannot be undone.
				</v-card-text>
				<v-card-actions class="px-4 pb-4">
					<v-btn variant="text" @click="confirmDialog = false">Cancel</v-btn>
					<v-spacer />
					<v-btn color="primary" variant="tonal" :loading="sessionsBusy" @click="doStartNewSession">
						Archive &amp; Start Fresh
					</v-btn>
				</v-card-actions>
			</v-card>
		</v-dialog>

	</div>
</template>
