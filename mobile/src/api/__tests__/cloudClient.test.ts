/**
 * TDD layer M1.a — CloudClient.
 *
 * Driven by a `ScriptedTransport` so each test pins the exact request
 * the client builds and the exact `ApiError` it maps a response to.
 */
import {CloudClient} from '../cloudClient';
import {HttpMethod, HttpResponse, HttpTransport, TransportError} from '../http';
import {ApiError, Session} from '../types';

interface Call {
  method: HttpMethod;
  path: string;
  body?: unknown;
}

/** Records every request and replies with a scripted response. */
class ScriptedTransport implements HttpTransport {
  readonly calls: Call[] = [];
  constructor(private readonly responder: (call: Call) => HttpResponse) {}
  async request(
    method: HttpMethod,
    path: string,
    body?: unknown,
  ): Promise<HttpResponse> {
    const call: Call = {method, path, body};
    this.calls.push(call);
    return this.responder(call);
  }
}

const SESSION: Session = {
  token: 'tok',
  subscriber: {
    societyId: 'soc_sunset',
    flatNumber: 'A-101',
    sipUsername: 'a101',
    displayName: 'Resident A101',
    role: 'resident',
  },
};

describe('CloudClient.login', () => {
  it('builds POST /api/v1/subscriber/login with society, flat, password', async () => {
    const t = new ScriptedTransport(() => ({status: 200, body: SESSION}));
    await new CloudClient(t).login('SUNSET', 'A-101', 'pw');

    expect(t.calls).toHaveLength(1);
    expect(t.calls[0].method).toBe('POST');
    expect(t.calls[0].path).toBe('/api/v1/subscriber/login');
    expect(t.calls[0].body).toEqual({
      societyName: 'SUNSET',
      flatNumber: 'A-101',
      password: 'pw',
    });
  });

  it('returns the session on a 2xx', async () => {
    const t = new ScriptedTransport(() => ({status: 200, body: SESSION}));
    expect(await new CloudClient(t).login('SUNSET', 'A-101', 'pw')).toEqual(
      SESSION,
    );
  });

  it('maps HTTP 401 to ApiError INVALID_CREDENTIALS', async () => {
    const t = new ScriptedTransport(() => ({status: 401, body: {}}));
    await expect(
      new CloudClient(t).login('SUNSET', 'A-101', 'bad'),
    ).rejects.toHaveProperty('code', 'INVALID_CREDENTIALS');
  });

  it('maps HTTP 429 to ApiError RATE_LIMITED', async () => {
    const t = new ScriptedTransport(() => ({status: 429, body: {}}));
    await expect(
      new CloudClient(t).login('SUNSET', 'A-101', 'pw'),
    ).rejects.toHaveProperty('code', 'RATE_LIMITED');
  });

  it('rejects a malformed 2xx body as a SERVER error', async () => {
    const t = new ScriptedTransport(() => ({status: 200, body: {nope: 1}}));
    await expect(
      new CloudClient(t).login('SUNSET', 'A-101', 'pw'),
    ).rejects.toHaveProperty('code', 'SERVER');
  });
});

describe('CloudClient.register', () => {
  it('omits optional mobile/email when they are not provided', async () => {
    const t = new ScriptedTransport(() => ({status: 201, body: SESSION}));
    await new CloudClient(t).register({
      societyName: 'SUNSET',
      flatNumber: 'A-101',
      residentName: 'Asha',
      password: 'longenough',
    });
    expect(t.calls[0].body).toEqual({
      societyName: 'SUNSET',
      flatNumber: 'A-101',
      residentName: 'Asha',
      password: 'longenough',
    });
  });

  it('includes optional mobile + email when provided', async () => {
    const t = new ScriptedTransport(() => ({status: 201, body: SESSION}));
    await new CloudClient(t).register({
      societyName: 'SUNSET',
      flatNumber: 'A-101',
      residentName: 'Asha',
      password: 'longenough',
      mobile: '+91-99999',
      email: 'asha@example.com',
    });
    expect(t.calls[0].body).toMatchObject({
      mobile: '+91-99999',
      email: 'asha@example.com',
    });
  });

  it('maps 404 to UNKNOWN_SOCIETY and 409 to DUPLICATE', async () => {
    const req = {
      societyName: 'X',
      flatNumber: 'A',
      residentName: 'N',
      password: 'longenough',
    };
    await expect(
      new CloudClient(
        new ScriptedTransport(() => ({status: 404, body: {}})),
      ).register(req),
    ).rejects.toHaveProperty('code', 'UNKNOWN_SOCIETY');
    await expect(
      new CloudClient(
        new ScriptedTransport(() => ({status: 409, body: {}})),
      ).register(req),
    ).rejects.toHaveProperty('code', 'DUPLICATE');
  });
});

describe('CloudClient.resolveSociety', () => {
  it('returns the societyId on success', async () => {
    const t = new ScriptedTransport(() => ({
      status: 200,
      body: {societyId: 'soc_sunset'},
    }));
    expect(await new CloudClient(t).resolveSociety('SUNSET')).toBe('soc_sunset');
  });

  it('rejects UNKNOWN_SOCIETY when the society is not found', async () => {
    const t = new ScriptedTransport(() => ({status: 404, body: {}}));
    await expect(
      new CloudClient(t).resolveSociety('NOWHERE'),
    ).rejects.toHaveProperty('code', 'UNKNOWN_SOCIETY');
  });
});

describe('CloudClient — transport failure', () => {
  it('maps a TransportError to a typed ApiError NETWORK', async () => {
    const failing: HttpTransport = {
      request: async () => {
        throw new TransportError('offline');
      },
    };
    const rejection = await new CloudClient(failing)
      .login('S', 'A', 'p')
      .catch((e: unknown) => e);
    expect(rejection).toBeInstanceOf(ApiError);
    expect((rejection as ApiError).code).toBe('NETWORK');
  });
});

describe('CloudClient.registerDevice', () => {
  it('POSTs /api/v1/push/device with sipUsername, platform and token', async () => {
    const t = new ScriptedTransport(() => ({status: 204, body: null}));
    await new CloudClient(t).registerDevice({
      sipUsername: 'a101',
      platform: 'ios',
      token: 'apns-tok',
    });
    expect(t.calls[0].method).toBe('POST');
    expect(t.calls[0].path).toBe('/api/v1/push/device');
    expect(t.calls[0].body).toEqual({
      sipUsername: 'a101',
      platform: 'ios',
      token: 'apns-tok',
    });
  });

  it('resolves on a 2xx and maps a non-2xx to a typed ApiError', async () => {
    await expect(
      new CloudClient(
        new ScriptedTransport(() => ({status: 200, body: null})),
      ).registerDevice({sipUsername: 'a101', platform: 'android', token: 't'}),
    ).resolves.toBeUndefined();

    await expect(
      new CloudClient(
        new ScriptedTransport(() => ({status: 429, body: {}})),
      ).registerDevice({sipUsername: 'a101', platform: 'android', token: 't'}),
    ).rejects.toHaveProperty('code', 'RATE_LIMITED');
  });
});

describe('CloudClient.getDirectory', () => {
  const TWO_ROWS = [
    {
      flatNumber: 'A-101',
      displayName: 'Alice',
      sipUri: 'sip:A-101@pbx.local',
      online: true,
    },
    {
      flatNumber: 'B-204',
      displayName: 'Bob',
      sipUri: 'sip:B-204@pbx.local',
      online: false,
    },
  ];

  it('builds GET /api/v1/subscriber with the URL-encoded societyId', async () => {
    const t = new ScriptedTransport(() => ({status: 200, body: TWO_ROWS}));

    await new CloudClient(t).getDirectory('soc sunset');

    expect(t.calls[0].method).toBe('GET');
    expect(t.calls[0].path).toBe('/api/v1/subscriber?societyId=soc%20sunset');
    expect(t.calls[0].body).toBeUndefined();
  });

  it('appends flatPrefix when provided and trims whitespace', async () => {
    const t = new ScriptedTransport(() => ({status: 200, body: TWO_ROWS}));

    await new CloudClient(t).getDirectory('soc', '  A  ');

    expect(t.calls[0].path).toBe('/api/v1/subscriber?societyId=soc&flatPrefix=A');
  });

  it('omits flatPrefix when undefined or empty-after-trim', async () => {
    const t = new ScriptedTransport(() => ({status: 200, body: TWO_ROWS}));
    const c = new CloudClient(t);

    await c.getDirectory('soc');
    await c.getDirectory('soc', '');
    await c.getDirectory('soc', '   ');

    expect(t.calls[0].path).toBe('/api/v1/subscriber?societyId=soc');
    expect(t.calls[1].path).toBe('/api/v1/subscriber?societyId=soc');
    expect(t.calls[2].path).toBe('/api/v1/subscriber?societyId=soc');
  });

  it('returns the array body on a 2xx', async () => {
    const t = new ScriptedTransport(() => ({status: 200, body: TWO_ROWS}));

    expect(await new CloudClient(t).getDirectory('soc')).toEqual(TWO_ROWS);
  });

  it('rejects a non-array body as a SERVER ApiError', async () => {
    const t = new ScriptedTransport(() => ({status: 200, body: {oops: true}}));

    await expect(new CloudClient(t).getDirectory('soc')).rejects.toBeInstanceOf(
      ApiError,
    );
    await expect(
      new CloudClient(t).getDirectory('soc'),
    ).rejects.toHaveProperty('code', 'SERVER');
  });

  it('maps a transport failure to ApiError NETWORK', async () => {
    const t = new ScriptedTransport(() => {
      throw new TransportError('dns');
    });

    await expect(
      new CloudClient(t).getDirectory('soc'),
    ).rejects.toHaveProperty('code', 'NETWORK');
  });
});

describe('CloudClient.getCallHistory', () => {
  const TWO_CDRS = [
    {
      callId: 'c-1',
      societyId: 'soc',
      fromFlat: 'A-101',
      toFlat: 'B-204',
      direction: 'outbound',
      type: 'p2p',
      startedAt: '2026-05-29T10:00:00Z',
      endedAt: '2026-05-29T10:01:00Z',
      durationSec: 60,
      hangupCause: 'normal',
    },
    {
      callId: 'c-2',
      societyId: 'soc',
      fromFlat: 'C-301',
      toFlat: 'D-402',
      direction: 'outbound',
      type: 'p2p',
      startedAt: '2026-05-29T11:00:00Z',
      endedAt: '2026-05-29T11:02:00Z',
      durationSec: 120,
      hangupCause: 'normal',
    },
  ];

  it('builds GET /api/v1/cdr with the URL-encoded societyId', async () => {
    const t = new ScriptedTransport(() => ({status: 200, body: TWO_CDRS}));

    await new CloudClient(t).getCallHistory('soc with space');

    expect(t.calls[0].method).toBe('GET');
    expect(t.calls[0].path).toBe('/api/v1/cdr?societyId=soc%20with%20space');
    expect(t.calls[0].body).toBeUndefined();
  });

  it('returns the array body on a 2xx', async () => {
    const t = new ScriptedTransport(() => ({status: 200, body: TWO_CDRS}));

    expect(await new CloudClient(t).getCallHistory('soc')).toEqual(TWO_CDRS);
  });

  it('returns an empty array when the cloud reports no CDRs', async () => {
    const t = new ScriptedTransport(() => ({status: 200, body: []}));

    expect(await new CloudClient(t).getCallHistory('soc')).toEqual([]);
  });

  it('rejects a non-array body as a SERVER ApiError', async () => {
    const t = new ScriptedTransport(() => ({status: 200, body: {oops: true}}));

    await expect(
      new CloudClient(t).getCallHistory('soc'),
    ).rejects.toBeInstanceOf(ApiError);
    await expect(
      new CloudClient(t).getCallHistory('soc'),
    ).rejects.toHaveProperty('code', 'SERVER');
  });

  it('maps a transport failure to ApiError NETWORK', async () => {
    const t = new ScriptedTransport(() => {
      throw new TransportError('dns');
    });

    await expect(
      new CloudClient(t).getCallHistory('soc'),
    ).rejects.toHaveProperty('code', 'NETWORK');
  });

  it('maps a non-2xx response to a typed ApiError', async () => {
    const t = new ScriptedTransport(() => ({status: 429, body: {}}));

    await expect(
      new CloudClient(t).getCallHistory('soc'),
    ).rejects.toHaveProperty('code', 'RATE_LIMITED');
  });
});
