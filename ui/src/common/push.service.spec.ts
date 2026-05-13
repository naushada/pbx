import { TestBed, fakeAsync, tick } from '@angular/core/testing';
import { HttpClientTestingModule, HttpTestingController } from '@angular/common/http/testing';

import { PushService } from './push.service';
import { UriMap } from './app-globals';

// Browser globals (Notification / navigator.serviceWorker / PushManager)
// vary by host — these tests stub the surfaces PushService touches.

interface FakeSubscription {
    endpoint: string;
    getKey(name: 'p256dh' | 'auth'): ArrayBuffer | null;
    unsubscribe(): Promise<boolean>;
}

class FakePushManager {
    public current: FakeSubscription | null = null;
    public subscribeOpts?: { applicationServerKey?: BufferSource; userVisibleOnly?: boolean };
    async getSubscription(): Promise<FakeSubscription | null> { return this.current; }
    async subscribe(opts: any): Promise<FakeSubscription> {
        this.subscribeOpts = opts;
        const sub: FakeSubscription = {
            endpoint: 'https://push.example/sub-1',
            getKey: (k) => k === 'p256dh' ? new Uint8Array([1,2,3]).buffer : new Uint8Array([4,5]).buffer,
            unsubscribe: async () => { this.current = null; return true; },
        };
        this.current = sub;
        return sub;
    }
}

class FakeSwReg {
    public pushManager = new FakePushManager();
}

function installSwMock(reg: FakeSwReg | undefined): void {
    Object.defineProperty(navigator, 'serviceWorker', {
        value: {
            register:        async () => reg ?? new FakeSwReg(),
            getRegistration: async () => reg,
        },
        configurable: true,
    });
    (window as any).PushManager = FakePushManager;
}

function uninstallSwMock(): void {
    // Re-defining as undefined so `'serviceWorker' in navigator` is false
    // and the property still has `configurable: true` for the next test.
    Object.defineProperty(navigator, 'serviceWorker', {
        value: undefined, configurable: true,
    });
    delete (navigator as any).serviceWorker;
    delete (window as any).PushManager;
}

function setPermission(p: NotificationPermission): void {
    Object.defineProperty(globalThis, 'Notification', {
        value: {
            permission: p,
            requestPermission: async () => 'granted',
        },
        configurable: true,
    });
}

const vapidUrl    = UriMap.get('from_web_push_vapid_key') as string;
const subscribeUrl = UriMap.get('from_web_push_subscribe') as string;

describe('PushService', () => {

    let backend: HttpTestingController;

    beforeEach(() => {
        TestBed.configureTestingModule({ imports: [HttpClientTestingModule] });
        backend = TestBed.inject(HttpTestingController);
    });

    afterEach(() => {
        uninstallSwMock();
        backend.verify();
    });

    it('refresh() reports unsupported when serviceWorker is missing', async () => {
        uninstallSwMock();
        const svc = TestBed.inject(PushService);
        expect(await svc.refresh()).toBe('unsupported');
    });

    it('refresh() reports denied when Notification permission is denied', async () => {
        installSwMock(new FakeSwReg());
        setPermission('denied');
        const svc = TestBed.inject(PushService);
        expect(await svc.refresh()).toBe('denied');
    });

    it('refresh() reports disabled when there is no existing subscription', async () => {
        installSwMock(new FakeSwReg());
        setPermission('default');
        const svc = TestBed.inject(PushService);
        expect(await svc.refresh()).toBe('disabled');
    });

    it('enable() subscribes with the cloud VAPID key and POSTs the payload', fakeAsync(() => {
        const reg = new FakeSwReg();
        installSwMock(reg);
        setPermission('granted');
        const svc = TestBed.inject(PushService);

        let result: string | undefined;
        svc.enable().then(r => result = r);

        tick();
        const vapidReq = backend.expectOne(vapidUrl);
        vapidReq.flush({ key: 'BNcRdreALRFXTkOOUHK1EtK2wtaz5Ry4YfYCL_k7m9X' });

        tick();
        const subReq = backend.expectOne(subscribeUrl);
        expect(subReq.request.method).toBe('POST');
        const body = subReq.request.body;
        expect(body.endpoint).toBe('https://push.example/sub-1');
        expect(body.keys.p256dh).toMatch(/^[A-Za-z0-9_-]+$/);
        expect(body.keys.auth).toMatch(/^[A-Za-z0-9_-]+$/);
        subReq.flush({ ok: true });

        tick();
        expect(result).toBe('enabled');
        expect(reg.pushManager.subscribeOpts?.userVisibleOnly).toBeTrue();
    }));

    it('enable() short-circuits when already subscribed (re-posts to cloud)', fakeAsync(() => {
        const reg = new FakeSwReg();
        reg.pushManager.current = {
            endpoint: 'https://push.example/existing',
            getKey: (k) => k === 'p256dh' ? new Uint8Array([9]).buffer : new Uint8Array([8]).buffer,
            unsubscribe: async () => true,
        };
        installSwMock(reg);
        setPermission('granted');
        const svc = TestBed.inject(PushService);

        let result: string | undefined;
        svc.enable().then(r => result = r);

        tick();
        // No VAPID fetch because subscription already exists.
        backend.expectNone(vapidUrl);

        const subReq = backend.expectOne(subscribeUrl);
        expect(subReq.request.body.endpoint).toBe('https://push.example/existing');
        subReq.flush({ ok: true });

        tick();
        expect(result).toBe('enabled');
    }));

    it('disable() removes the subscription and reports disabled', async () => {
        const reg = new FakeSwReg();
        reg.pushManager.current = {
            endpoint: 'https://push.example/old',
            getKey: () => new Uint8Array([1]).buffer,
            unsubscribe: jasmine.createSpy('unsubscribe').and.returnValue(Promise.resolve(true)),
        };
        installSwMock(reg);
        setPermission('granted');
        const svc = TestBed.inject(PushService);

        const final = await svc.disable();
        expect(final).toBe('disabled');
        expect(reg.pushManager.current?.unsubscribe).toHaveBeenCalled();
    });
});
