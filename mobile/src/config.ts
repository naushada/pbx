/**
 * Build-time configuration.
 *
 * `CLOUD_BASE_URL` is the onprem-pbx cloud control plane the app talks
 * to (REST + the /sip-ws tunnel). Override per environment at build
 * time; this default points at the shared cloud deployment.
 */
export const CLOUD_BASE_URL = 'https://pabx-5fbf3550f938.herokuapp.com';
