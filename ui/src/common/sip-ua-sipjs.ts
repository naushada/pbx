import { Injectable } from '@angular/core';
import {
    UserAgent, Registerer, RegistererState, TransportState,
    Inviter, Session, SessionState,
} from 'sip.js';
import { SessionDescriptionHandler } from 'sip.js/lib/platform/web';

import {
    SipUaFactory, SipUaHandle, SipUaOpts, SipUaState, SipUaStateChange,
    SipCallHandle, SipCallState, SipCallStateChange,
} from './sip-ua';

// Production wrapper around SIP.js's UserAgent + Registerer. Maps
// SIP.js's internal state events into the SipUaState enum so SipService
// (and its tests) don't import sip.js directly.
//
// The cloud's /sip-ws endpoint authenticates by the `?token=<bearer>`
// query param baked into wsUrl; SIP digest is not used at the UA level
// because the WS upgrade already authenticated. authorizationUsername
// is still passed so we can answer a 401 challenge if the cloud ever
// re-prompts (defensive).

@Injectable({ providedIn: 'root' })
export class SipJsUaFactory implements SipUaFactory {

    create(opts: SipUaOpts): SipUaHandle {
        const uri = UserAgent.makeURI(opts.uri);
        if (!uri) {
            throw new Error(`SipJsUaFactory: invalid SIP URI: ${opts.uri}`);
        }

        const ua = new UserAgent({
            uri,
            displayName:          opts.displayName,
            authorizationUsername: opts.authUser,
            authorizationPassword: '',
            transportOptions: {
                server:   opts.wsUrl,
                traceSip: false,
            },
        });

        return new SipJsUaHandle(ua);
    }
}

class SipJsUaHandle implements SipUaHandle {

    private listeners: Array<(c: SipUaStateChange) => void> = [];
    private registerer?: Registerer;

    constructor(private readonly ua: UserAgent) {
        // Transport-level errors emerge here. Once we move past 'Connected'
        // the Registerer takes over emitting state.
        this.ua.transport.stateChange.addListener((s: TransportState) => {
            if (s === TransportState.Connecting)    this.emit('starting');
            else if (s === TransportState.Connected) this.emit('started');
            else if (s === TransportState.Disconnected) this.emit('terminated', 'transport_disconnected');
        });
    }

    onStateChange(cb: (c: SipUaStateChange) => void): void {
        this.listeners.push(cb);
    }

    async start(): Promise<void> {
        await this.ua.start();

        this.registerer = new Registerer(this.ua);
        this.registerer.stateChange.addListener((s: RegistererState) => {
            switch (s) {
                case RegistererState.Initial:      break;
                case RegistererState.Registered:   this.emit('registered');   break;
                case RegistererState.Unregistered: this.emit('unregistered'); break;
                case RegistererState.Terminated:   this.emit('terminated', 'registerer_terminated'); break;
            }
        });

        this.emit('registering');
        await this.registerer.register();
    }

    async stop(): Promise<void> {
        try { await this.registerer?.unregister(); } catch { /* logged below via state */ }
        try { await this.ua.stop(); }              catch { /* idem */ }
    }

    placeCall(targetUri: string): SipCallHandle {
        const uri = UserAgent.makeURI(targetUri);
        if (!uri) throw new Error(`SipJsUaHandle: invalid target SIP URI: ${targetUri}`);

        const inviter = new Inviter(this.ua, uri, {
            sessionDescriptionHandlerOptions: {
                constraints: { audio: true, video: false },
            },
        });

        const handle = new SipJsCallHandle(inviter);

        // Fire INVITE asynchronously — caller already has the handle so
        // any state callback registered before this microtask resolves
        // sees the very first 'calling' emission.
        inviter.invite().catch(err => handle.emit('failed', errorReason(err)));
        return handle;
    }

    private emit(state: SipUaState, detail?: string): void {
        const change: SipUaStateChange = detail ? { state, detail } : { state };
        for (const cb of this.listeners) cb(change);
    }
}

class SipJsCallHandle implements SipCallHandle {

    private listeners: Array<(c: SipCallStateChange) => void> = [];

    constructor(private readonly session: Session) {
        this.session.stateChange.addListener((s: SessionState) => {
            switch (s) {
                case SessionState.Initial:
                case SessionState.Establishing: this.emit('calling');                   break;
                case SessionState.Established:  this.emit('in-call');                   break;
                case SessionState.Terminating:                                          break;
                case SessionState.Terminated: {
                    // sip.js doesn't distinguish "they answered then hung up" from
                    // "rejected"; sniff the response code stashed on the session.
                    const code  = this.terminalStatusCode();
                    const ended = code === undefined || (code >= 200 && code < 300);
                    this.emit(ended ? 'ended' : 'failed', code ? `sip_${code}` : undefined);
                    break;
                }
            }
        });
    }

    onStateChange(cb: (c: SipCallStateChange) => void): void { this.listeners.push(cb); }

    async hangup(): Promise<void> {
        switch (this.session.state) {
            case SessionState.Initial:
            case SessionState.Establishing:
                if (this.session instanceof Inviter) {
                    try { await this.session.cancel(); } catch { /* fall through */ }
                }
                break;
            case SessionState.Established:
                try { await this.session.bye(); } catch { /* idem */ }
                break;
            default:
                /* already terminating / terminated */
                break;
        }
    }

    setMute(mute: boolean): void {
        const pc = this.peerConnection();
        if (!pc) return;
        for (const sender of pc.getSenders()) {
            if (sender.track && sender.track.kind === 'audio') {
                sender.track.enabled = !mute;
            }
        }
    }

    getRemoteStream(): MediaStream | undefined {
        const sdh = this.session.sessionDescriptionHandler as
            SessionDescriptionHandler | undefined;
        return sdh?.remoteMediaStream;
    }

    /** Pulled out so SipJsUaHandle.placeCall can drive a failure pre-state. */
    emit(state: SipCallState, detail?: string): void {
        const change: SipCallStateChange = detail ? { state, detail } : { state };
        for (const cb of this.listeners) cb(change);
    }

    private peerConnection(): RTCPeerConnection | undefined {
        const sdh = this.session.sessionDescriptionHandler as
            SessionDescriptionHandler | undefined;
        return sdh?.peerConnection;
    }

    private terminalStatusCode(): number | undefined {
        // sip.js stashes the final response on the inviter; the typed
        // surface differs between SIP.js versions so we widen and probe.
        const anySess: any = this.session;
        return anySess.replyCode
            ?? anySess.incomingByeRequest?.message.statusCode
            ?? anySess.outgoingInviteRequest?.delegate?.statusCode
            ?? undefined;
    }
}

function errorReason(e: unknown): string {
    if (e instanceof Error) return e.message;
    if (typeof e === 'string') return e;
    return 'unknown';
}
