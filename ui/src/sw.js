// Service worker for the Society Softphone.
//
// Two responsibilities:
//   1. 'push'             — show an OS notification when the cloud
//                            announces an incoming call. Payload is
//                            decrypted by the browser via the push
//                            subscription's p256dh/auth keys; we get
//                            the plaintext JSON straight from event.data.
//   2. 'notificationclick' — focus an existing app tab if one is open;
//                            otherwise open '/' which AppComponent then
//                            routes to /main/dashboard (auth) or /login.
//
// The SIP INVITE itself still flows over the WebSocket once the app
// is in the foreground — this push only *wakes* the user.

const APP_HOME = '/';

self.addEventListener('install', (event) => {
    event.waitUntil(self.skipWaiting());
});

self.addEventListener('activate', (event) => {
    event.waitUntil(self.clients.claim());
});

self.addEventListener('push', (event) => {
    let payload = {};
    try { payload = event.data ? event.data.json() : {}; }
    catch (e) { payload = { kind: 'unknown' }; }

    const title = payload.kind === 'incoming-call'
        ? `Incoming call from ${payload.displayName || payload.fromFlat || 'a neighbour'}`
        : 'Society Softphone';

    const options = {
        body: payload.fromFlat ? `Flat ${payload.fromFlat}` : 'Open the app to view',
        tag:  payload.callId   || 'pbxui-push',           // de-dupe
        renotify: true,
        requireInteraction: payload.kind === 'incoming-call',
        data: payload,
    };

    event.waitUntil(self.registration.showNotification(title, options));
});

self.addEventListener('notificationclick', (event) => {
    event.notification.close();
    event.waitUntil((async () => {
        const all = await self.clients.matchAll({ type: 'window', includeUncontrolled: true });
        for (const c of all) {
            if (c.url.includes(self.location.origin)) {
                await c.focus();
                return;
            }
        }
        await self.clients.openWindow(APP_HOME);
    })());
});
