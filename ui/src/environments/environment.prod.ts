// Production environment. Selected by angular.json `fileReplacements`
// when building with `ng build --configuration production`.

export const environment = {
  production: true,
  cloudOrigin: '',                       // empty = same-origin (Heroku serves both UI and API)
  sipWsPath:  '/sip-ws',
  apiBase:    '/api/v1',
};
