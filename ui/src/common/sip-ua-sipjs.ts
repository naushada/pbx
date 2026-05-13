import { Injectable } from '@angular/core';
import { UserAgent, Registerer, RegistererState, TransportState } from 'sip.js';

import {
    SipUaFactory, SipUaHandle, SipUaOpts, SipUaState, SipUaStateChange,
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

    private emit(state: SipUaState, detail?: string): void {
        const change: SipUaStateChange = detail ? { state, detail } : { state };
        for (const cb of this.listeners) cb(change);
    }
}
