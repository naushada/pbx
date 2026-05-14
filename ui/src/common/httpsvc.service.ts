import { Injectable } from '@angular/core';
import { HttpClient, HttpHeaders, HttpParams } from '@angular/common/http';
import { Observable } from 'rxjs';
import { environment } from 'src/environments/environment';
import {
    Subscriber, LoginResponse, DirectoryEntry, CallRecord,
    TurnCredentials, PushSubscriptionPayload, UriMap,
} from './app-globals';

// Cloud REST wrapper. Same shape as xpmile/ui/src/common/httpsvc.service.ts
// — `UriMap` key → resolved URL, single `HttpClient`, one method per
// endpoint. Auth: bearer token set by the login flow (added in slice 1
// via an HttpInterceptor).

@Injectable({ providedIn: 'root' })
export class HttpsvcService {

    private readonly origin = environment.cloudOrigin;
    private readonly jsonHeaders = {
        headers: new HttpHeaders({ 'Content-Type': 'application/json' }),
    };

    constructor(private http: HttpClient) {}

    /** Resolve a UriMap key to an absolute (or relative) URL. */
    getUri(key: string): string {
        const path = UriMap.get(key);
        if (!path) {
            throw new Error(`HttpsvcService: unknown UriMap key '${key}'`);
        }
        return this.origin.length > 0 ? this.origin + path : path;
    }

    // ─── Auth ────────────────────────────────────────────────────────
    // Login takes the human-typed society slug (`societyCode`), not the
    // internal id — the cloud handler resolves slug → id and includes
    // both in the LoginResponse. Other endpoints below take `societyId`.
    login(societyCode: string, flatNumber: string, password: string): Observable<LoginResponse> {
        return this.http.post<LoginResponse>(
            this.getUri('from_web_subscriber_login'),
            { societyCode, flatNumber, password },
            this.jsonHeaders,
        );
    }

    // ─── Subscriber CRUD (admin) ─────────────────────────────────────
    getSubscribers(societyId: string): Observable<Subscriber[]> {
        const params = new HttpParams().set('societyId', societyId);
        return this.http.get<Subscriber[]>(this.getUri('from_web_subscriber'), { params });
    }

    // ─── Directory (any logged-in subscriber) ────────────────────────
    searchDirectory(societyId: string, flatPrefix: string): Observable<DirectoryEntry[]> {
        const params = new HttpParams()
            .set('societyId', societyId)
            .set('flatPrefix', flatPrefix);
        return this.http.get<DirectoryEntry[]>(this.getUri('from_web_directory'), { params });
    }

    // ─── Call history ────────────────────────────────────────────────
    getCallHistory(subscriberFlat: string): Observable<CallRecord[]> {
        const params = new HttpParams().set('flat', subscriberFlat);
        return this.http.get<CallRecord[]>(this.getUri('from_web_cdr'), { params });
    }

    // ─── Push / VAPID ────────────────────────────────────────────────
    getVapidPublicKey(): Observable<{ key: string }> {
        return this.http.get<{ key: string }>(this.getUri('from_web_push_vapid_key'));
    }

    registerPushSubscription(payload: PushSubscriptionPayload): Observable<{ ok: boolean }> {
        return this.http.post<{ ok: boolean }>(
            this.getUri('from_web_push_subscribe'),
            payload,
            this.jsonHeaders,
        );
    }

    // ─── TURN credentials (short-lived) ──────────────────────────────
    getTurnCredentials(): Observable<TurnCredentials> {
        return this.http.get<TurnCredentials>(this.getUri('from_web_turn_credentials'));
    }

    // ─── Keep-alive heartbeat ────────────────────────────────────────
    ping(): Observable<{ ok: boolean; ts: number }> {
        return this.http.get<{ ok: boolean; ts: number }>(this.getUri('from_web_ping'));
    }
}
