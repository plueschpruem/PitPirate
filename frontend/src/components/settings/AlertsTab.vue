<script setup lang="ts">
import { ref, computed, onMounted } from 'vue'
import type { RemoteSettings } from '@/api/pitpirate'

const props = defineProps<{
	remote: RemoteSettings
}>()

const isHttps = typeof window !== 'undefined' && window.location.protocol === 'https:'
const isStandalone = typeof window !== 'undefined'
	&& (
		(window.navigator as unknown as { standalone?: boolean }).standalone === true
		|| window.matchMedia('(display-mode: standalone)').matches
	)

type PushCtx = 'http' | 'https-browser' | 'https-pwa'
const pushCtx: PushCtx = !isHttps ? 'http' : isStandalone ? 'https-pwa' : 'https-browser'

const serverOrigin = computed(() => {
	const s = props.remote.url
	if (!s.startsWith('https://')) return ''
	try { return new URL(s).origin } catch { return '' }
})

// ── Subscribe / unsubscribe (state 3 only) ────────────────────────────────────

type SubState = 'unknown' | 'subscribed' | 'unsubscribed' | 'unsupported'
const subState = ref<SubState>('unknown')
const subBusy  = ref(false)
const subError = ref('')

function b64urlToUint8(b64url: string): ArrayBuffer {
	const pad = '='.repeat((4 - (b64url.length % 4)) % 4)
	const b64 = (b64url + pad).replace(/-/g, '+').replace(/_/g, '/')
	const bin = atob(b64)
	const buf = new ArrayBuffer(bin.length)
	const arr = new Uint8Array(buf)
	for (let i = 0; i < bin.length; i++) arr[i] = bin.charCodeAt(i)
	return buf
}

onMounted(async () => {
	if (pushCtx !== 'https-pwa') return
	if (!('serviceWorker' in navigator) || !('PushManager' in window)) {
		subState.value = 'unsupported'; return
	}
	try {
		const reg = await navigator.serviceWorker.getRegistration('/sw.js')
		if (!reg) { subState.value = 'unsubscribed'; return }
		const sub = await reg.pushManager.getSubscription()
		subState.value = sub ? 'subscribed' : 'unsubscribed'
	} catch { subState.value = 'unsubscribed' }
})

async function subscribe() {
	subBusy.value  = true
	subError.value = ''
	try {
		const reg = await navigator.serviceWorker.register('/sw.js')
		await navigator.serviceWorker.ready

		const perm = await Notification.requestPermission()
		if (perm !== 'granted') { subError.value = 'Permission denied.'; return }

		const base = window.location.origin
		const keyRes = await fetch(`${base}/php/push-subscriptions.php?action=vapid-public-key`)
		if (!keyRes.ok) throw new Error(`Server ${keyRes.status}`)
		const { publicKey } = await keyRes.json()

		const subscription = await reg.pushManager.subscribe({
			userVisibleOnly: true,
			applicationServerKey: b64urlToUint8(publicKey),
		})

		const res = await fetch(`${base}/php/push-subscriptions.php?action=subscribe`, {
			method: 'POST',
			headers: { 'Content-Type': 'application/json' },
			body: JSON.stringify(subscription),
		})
		if (!res.ok) throw new Error(`Server ${res.status}`)
		subState.value = 'subscribed'
	} catch (e: unknown) {
		subError.value = (e as Error).message
	} finally {
		subBusy.value = false
	}
}

async function unsubscribe() {
	subBusy.value  = true
	subError.value = ''
	try {
		const reg = await navigator.serviceWorker.getRegistration('/sw.js')
		const sub = reg ? await reg.pushManager.getSubscription() : null
		if (sub) {
			const base = window.location.origin
			await fetch(`${base}/php/push-subscriptions.php?action=unsubscribe`, {
				method: 'POST',
				headers: { 'Content-Type': 'application/json' },
				body: JSON.stringify({ endpoint: sub.endpoint }),
			})
			await sub.unsubscribe()
		}
		subState.value = 'unsubscribed'
	} catch (e: unknown) {
		subError.value = (e as Error).message
	} finally {
		subBusy.value = false
	}
}
</script>

<template>
	<v-card rounded="lg">
		<v-card-text class="px-4">

			<!-- State 1: served over HTTP from ESP32 -->
			<template v-if="pushCtx === 'http'">
				<p class="font-weight-bold mb-3">
					Push notifications require HTTPS.
					<template v-if="serverOrigin">
						<br>
						Open the server app to subscribe:
					</template>
				</p>
				<v-btn v-if="serverOrigin" :href="serverOrigin" target="_blank" rel="noopener" color="primary" variant="tonal" prepend-icon="open-in-new" block class="mb-3 font-weight-bold">
					Open on Server
				</v-btn>
				<v-alert density="compact" color="gray" variant="tonal" class="font-weight-bold">
					<v-icon icon="lightbulb-on" size="18" class="mr-2 mb-1" />
					<span class="font-weight-bold">Tip: install the server app to your Home Screen for push alerts.</span>
				</v-alert>
			</template>

			<!-- State 2: HTTPS but not installed as PWA -->
			<template v-else-if="pushCtx === 'https-browser'">
				<v-alert density="compact" color="gray" variant="tonal" class="font-weight-bold">
					<div style="font-size: 20px;">
						<strong>Install app to enable notifications.</strong><br />
						Tap <v-icon size="20" icon="dots-horizontal-circle-outl" /> → <v-icon size="20" icon="export-variant" /> Share → "Add to Home Screen"
						<br>
						then open PitPirate from your Home Screen.
					</div>
				</v-alert>
			</template>

			<!-- State 3: HTTPS PWA – full subscribe / unsubscribe -->
			<template v-else>
				<div v-if="subState === 'unsupported'" class="font-weight-bold">
					Push notifications are not supported on this device.
				</div>
				<template v-else>
					<div class="d-flex align-center mb-3">
						<v-icon :icon="subState === 'subscribed' ? 'bell-ring' : 'bell-off'" size="20" :color="subState === 'subscribed' ? 'success' : 'grey'" class="mr-2" />
						<span class="font-weight-bold">
							<template v-if="subState === 'unknown'">Checking…</template>
							<template v-else-if="subState === 'subscribed'">This device will receive alarm notifications.</template>
							<template v-else>Notifications are off for this device.</template>
						</span>
					</div>
					<v-alert v-if="subError" density="compact" variant="tonal" class="mb-3 font-weight-bold">
						{{ subError }}
					</v-alert>
				</template>
			</template>

		</v-card-text>

		<v-card-actions v-if="pushCtx === 'https-pwa' && subState !== 'unsupported'" class="px-4 pb-4">
			<v-spacer />
			<v-btn v-if="subState !== 'subscribed'" color="primary" variant="tonal" :loading="subBusy" :disabled="subState === 'unknown'" prepend-icon="bell-ring" @click="subscribe">
				Subscribe
			</v-btn>
			<v-btn v-else color="error" variant="flat" :loading="subBusy" prepend-icon="bell-off" @click="unsubscribe">
				Unsubscribe
			</v-btn>
		</v-card-actions>
	</v-card>
</template>
