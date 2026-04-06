<script setup lang="ts">
import { ref } from 'vue'
import type { TuyaSettings, RemoteSettings } from '@/api/pitpirate'
import Esp32Tab    from './settings/Esp32Tab.vue'
import AppTab      from './settings/AppTab.vue'
import AlertsTab   from './settings/AlertsTab.vue'
import SessionsTab from './settings/SessionsTab.vue'

defineProps<{
	tuya: TuyaSettings
	remote: RemoteSettings
	tuyaSaving: boolean
	remoteSaving: boolean
}>()

const emit = defineEmits<{
	'save-tuya': []
	'save-remote': []
}>()

const isHttps   = typeof window !== 'undefined' && window.location.protocol === 'https:'
const activeTab = ref<string>(isHttps ? 'app' : 'esp32')
</script>

<template>
	<div class="pa-3">

		<v-tabs v-model="activeTab" grow color="primary" class="mb-3" density="compact">
			<v-tab value="esp32" prepend-icon="router-wireless">esp32</v-tab>
			<v-tab value="app" prepend-icon="tune">App</v-tab>
			<v-tab value="alerts" prepend-icon="bell-cog">Alerts</v-tab>
			<v-tab value="sessions" prepend-icon="archive">Sessions</v-tab>
		</v-tabs>

		<v-window v-model="activeTab">

			<v-window-item value="esp32">
				<Esp32Tab
					:tuya="tuya"
					:remote="remote"
					:tuyaSaving="tuyaSaving"
					:remoteSaving="remoteSaving"
					@save-tuya="emit('save-tuya')"
					@save-remote="emit('save-remote')"
				/>
			</v-window-item>

			<v-window-item value="app">
				<AppTab />
			</v-window-item>

			<v-window-item value="alerts">
				<AlertsTab :remote="remote" />
			</v-window-item>

			<v-window-item value="sessions">
				<SessionsTab :remote="remote" />
			</v-window-item>

		</v-window>

	</div>
</template>
