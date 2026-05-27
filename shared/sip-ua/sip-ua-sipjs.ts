// Production wrapper around sip.js's UserAgent + Registerer. Shared by
// the Angular web softphone (ui/) and the React Native softphone
// (mobile/). Maps sip.js's internal state events into the SipUaState
// enum so callers don't import sip.js types directly.
//
// The cloud's /sip-ws endpoint authenticates by the `?token=<bearer>`
// query param baked into wsUrl; SIP digest is not used at the UA level
// because the WS upgrade already authenticated. authorizationUsername
// is still passed so we can answer a 401 challenge if the cloud ever
// re-prompts (defensive).
//
// `sip.js/lib/platform/web`'s default SessionDescriptionHandler calls
// `RTCPeerConnection`, `RTCSessionDescription`, `MediaStream`, etc. as
// globals. In the browser those are part of the DOM; in RN the
// `mobile/src/sip/webrtcGlobals.ts` shim installs them from
// `react-native-webrtc` before any sip.js module loads.
import {
    UserAgent, Registerer, RegistererState, TransportState,
    Inviter, Invitation, Session, SessionState,
} from 'sip.js';

import {
    SipUaFactory, SipUaHandle, SipUaOpts, SipUaState, SipUaStateChange,
    SipCallHandle, SipCallState, SipCallStateChange,
    IncomingCallHandle, IncomingCallInfo,
} from './sip-ua';

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
    private incomingCb?: (h: IncomingCallHandle) => void;
    private registerer?: Registerer;

    constructor(private readonly ua: UserAgent) {
        // Transport-level errors emerge here. Once we move past 'Connected'
        // the Registerer takes over emitting state.
        this.ua.transport.stateChange.addListener((s: TransportState) => {
            if (s === TransportState.Connecting)    this.emit('starting');
            else if (s === TransportState.Connected) this.emit('started');
            else if (s === TransportState.Disconnected) this.emit('terminated', 'transport_disconnected');
        });

        // sip.js delivers inbound INVITEs through UserAgent.delegate.
        // Wrap the Invitation in an IncomingCallHandle so callers never
        // import sip.js types directly.
        this.ua.delegate = {
            onInvite: (invitation: Invitation) => this.dispatchIncoming(invitation),
        };
    }

    onStateChange(cb: (c: SipUaStateChange) => void): void {
        this.listeners.push(cb);
    }

    onIncomingCall(cb: (h: IncomingCallHandle) => void): void {
        this.incomingCb = cb;
    }

    private dispatchIncoming(invitation: Invitation): void {
        if (!this.incomingCb) {
            // Nobody to take the call — auto-busy so the caller doesn't ring forever.
            invitation.reject({ statusCode: 486 }).catch(() => {});
            return;
        }

        const fromUriObj = invitation.remoteIdentity.uri;
        const info: IncomingCallInfo = {
            fromUri:  fromUriObj.toString(),
            fromFlat: fromUriObj.user || '',
            callId:   invitation.request.callId,
        };

        const handle: IncomingCallHandle = {
            info,
            accept: async (): Promise<SipCallHandle> => {
                await invitation.accept({
                    sessionDescriptionHandlerOptions: {
                        constraints: { audio: true, video: false },
                    },
                });
                return new SipJsCallHandle(invitation);
            },
            reject: async (cause?: 'busy' | 'declined'): Promise<void> => {
                const statusCode = cause === 'declined' ? 603 : 486;
                try { await invitation.reject({ statusCode }); } catch { /* logged elsewhere */ }
            },
        };

        this.incomingCb(handle);
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
        inviter.invite().catch(err => handle.emitFailure(errorReason(err)));
        return handle;
    }

    private emit(state: SipUaState, detail?: string): void {
        const change: SipUaStateChange = detail ? { state, detail } : { state };
        for (const cb of this.listeners) cb(change);
    }
}

export class SipJsCallHandle implements SipCallHandle {

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

    // Returned object is sip.js's platform/web SDH `remoteMediaStream`
    // — a DOM MediaStream in the browser, a react-native-webrtc
    // MediaStream in RN. Typed loosely here so this module compiles in
    // both environments; UI callers (call-panel) cast to MediaStream.
    getRemoteStream(): unknown {
        const sdh = this.session.sessionDescriptionHandler as
            { remoteMediaStream?: unknown } | undefined;
        return sdh?.remoteMediaStream;
    }

    /** Called by the UA when the INVITE pre-state cannot be established. */
    emitFailure(detail: string): void {
        this.emit('failed', detail);
    }

    private emit(state: SipCallState, detail?: string): void {
        const change: SipCallStateChange = detail ? { state, detail } : { state };
        for (const cb of this.listeners) cb(change);
    }

    private peerConnection(): RTCPeerConnection | undefined {
        const sdh = this.session.sessionDescriptionHandler as unknown as
            { peerConnection?: RTCPeerConnection } | undefined;
        return sdh?.peerConnection;
    }

    private terminalStatusCode(): number | undefined {
        // sip.js stashes the final response on the inviter; the typed
        // surface differs between sip.js versions so we widen and probe.
        const anySess = this.session as unknown as {
            replyCode?: number;
            incomingByeRequest?: { message?: { statusCode?: number } };
            outgoingInviteRequest?: { delegate?: { statusCode?: number } };
        };
        return anySess.replyCode
            ?? anySess.incomingByeRequest?.message?.statusCode
            ?? anySess.outgoingInviteRequest?.delegate?.statusCode
            ?? undefined;
    }
}

function errorReason(e: unknown): string {
    if (e instanceof Error) return e.message;
    if (typeof e === 'string') return e;
    return 'unknown';
}
