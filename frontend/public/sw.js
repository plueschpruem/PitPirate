// PitPirate service worker — handles incoming push notifications

self.addEventListener('push', function (event) {
    let title    = 'PitPirate';
    let body     = 'Alarm triggered';
    let tag      = 'pitpirate';
    let silent   = false;
    let renotify = true;

    if (event.data) {
        try {
            const d  = event.data.json();
            title    = d.title    ?? title;
            body     = d.body     ?? body;
            tag      = d.tag      ?? tag;
            silent   = d.silent   ?? false;
            renotify = d.renotify !== undefined ? d.renotify : true;
        } catch {
            body = event.data.text();
        }
    }

    event.waitUntil(
        self.registration.showNotification(title, {
            body,
            icon:     '/favicon/web-app-manifest-192x192.png',
            badge:    '/favicon/web-app-manifest-192x192.png',
            tag,
            renotify,
            silent,
        })
    );
});

self.addEventListener('notificationclick', function (event) {
    event.notification.close();
    event.waitUntil(
        clients.matchAll({ type: 'window', includeUncontrolled: true }).then(function (list) {
            if (list.length > 0) {
                return list[0].focus();
            }
            return clients.openWindow('/');
        })
    );
});
