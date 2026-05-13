// Development environment (default).
//
// The dev server proxies `/api` to the Heroku cloud via `proxy.conf.json`
// (added in a later slice). For now, all backend URLs are relative —
// the softphone treats whatever served index.html as the cloud origin.

export const environment = {
  production: false,
  cloudOrigin: '',                       // empty = same-origin (dev-server proxy)
  sipWsPath:  '/sip-ws',                 // SIP-over-WS upgrade endpoint
  apiBase:    '/api/v1',
};
