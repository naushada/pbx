// Softphone domain types + REST URL map.
//
// Mirrors xpmile/ui/src/common/app-globals.ts in shape (UriMap +
// domain interfaces). Endpoints match the cloud routes registered by
// MicroServicePbx.dispatch_pbx_routes (see modules/module/pbx/README.md).

// ─── Subscriber (= one resident's account) ──────────────────────────
export interface Subscriber {
    societyId:    string;     // Mongo ObjectId of the society document
    flatNumber:   string;     // "A-204", "B-12", etc. — unique within society
    displayName:  string;     // shown in directory + INVITE From-header
    sipUser:      string;     // SIP digest username (often == flatNumber)
    role?:        'admin' | 'resident' | 'guard';
    // Guard-only kiosk flag: when true, SipService auto-accepts every
    // inbound INVITE without ringing or showing the accept dialog
    // (DESIGN.md §9). Ignored for non-guard roles — defence in depth so
    // a stray flag on a resident's account can't silently auto-answer.
    autoAnswer?:  boolean;
}

// ─── Login response (POST /api/v1/subscriber/login) ────────────────
export interface LoginResponse {
    subscriber: Subscriber;
    token:      string;       // bearer for subsequent REST + /sip-ws upgrade
}

// ─── Directory search result (GET /api/v1/subscriber?society=…&flat=…)
export interface DirectoryEntry {
    flatNumber:  string;
    displayName: string;
    sipUri:      string;      // "sip:A-204@pbx.<societyId>"
    online:      boolean;     // last-known REGISTER status from the agent
}

// ─── CDR row (GET /api/v1/cdr?subscriber=…) ────────────────────────
export interface CallRecord {
    callId:        string;
    societyId:     string;
    fromFlat:      string;
    toFlat:        string;
    direction:     'inbound' | 'outbound';
    type:          'p2p' | 'conference';
    startedAt:     string;    // ISO-8601 from server
    answeredAt?:   string;
    endedAt:       string;
    durationSec:   number;
    hangupCause:   'normal' | 'busy' | 'noanswer' | 'rejected' | 'failed';
    conferenceBridge?: string;
}

// ─── TURN credentials (GET /api/v1/turn-credentials) ────────────────
export interface TurnCredentials {
    urls:       string[];     // ["turn:turn.pbx.local:3478?transport=udp", ...]
    username:   string;       // <unix_ts>:<sipUser> per RFC 5766 §5
    credential: string;       // HMAC-SHA1(turnSharedSecret, username)
    ttlSec:     number;
}

// ─── Web Push subscription (POST /api/v1/push-subscribe) ────────────
export interface PushSubscriptionPayload {
    endpoint: string;
    keys: {
        p256dh: string;       // base64url
        auth:   string;       // base64url
    };
}

// ─── Cloud route map. Mirrors xpmile's UriMap pattern. ─────────────
export const UriMap = new Map<string, string>([
    ['from_web_subscriber_login',    '/api/v1/subscriber/login'],
    ['from_web_subscriber',          '/api/v1/subscriber'],
    ['from_web_society',             '/api/v1/society'],
    ['from_web_directory',           '/api/v1/subscriber'],
    ['from_web_cdr',                 '/api/v1/cdr'],
    ['from_web_push_subscribe',      '/api/v1/push-subscribe'],
    ['from_web_push_vapid_key',      '/api/v1/push-vapid-key'],
    ['from_web_turn_credentials',    '/api/v1/turn-credentials'],
    ['from_web_ping',                '/api/v1/ping'],
]);
