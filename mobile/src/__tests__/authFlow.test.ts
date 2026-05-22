/**
 * TDD layer M1.d — auth flow integration (logic level).
 *
 * A real `CloudClient` driven against the `FakeCloud` backend through
 * `fakeCloudTransport`, plus the real `SessionStore`. This exercises
 * request building, status→ApiError mapping, and persistence together.
 *
 * The *screen-level* auth flow (form → submit → navigate) lands with
 * the M1.c screen work.
 */
import {FakeCloud} from '../test/fakeCloud';
import {fakeCloudTransport} from '../test/fakeCloudTransport';
import {CloudClient} from '../api/cloudClient';
import {SessionStore} from '../session/sessionStore';

function seededCloud(): {cloud: FakeCloud; client: CloudClient} {
  const cloud = new FakeCloud();
  cloud.seedSociety('SUNSET', 'soc_sunset');
  cloud.seedSubscriber(
    {
      societyId: 'soc_sunset',
      flatNumber: 'A-101',
      sipUsername: 'a101',
      displayName: 'Resident A101',
      role: 'resident',
    },
    'pw-a101',
  );
  return {cloud, client: new CloudClient(fakeCloudTransport(cloud))};
}

beforeEach(async () => {
  await SessionStore.clear();
});

describe('auth flow — login', () => {
  it('logs in against the cloud and persists the session', async () => {
    const {client} = seededCloud();
    const session = await client.login('SUNSET', 'A-101', 'pw-a101');
    await SessionStore.save(session);

    const restored = await SessionStore.load();
    expect(restored?.subscriber.sipUsername).toBe('a101');
    expect(restored?.token).toBeTruthy();
  });

  it('surfaces UNKNOWN_SOCIETY for a society that does not exist', async () => {
    const {client} = seededCloud();
    await expect(
      client.login('NOWHERE', 'A-101', 'pw-a101'),
    ).rejects.toHaveProperty('code', 'UNKNOWN_SOCIETY');
  });

  it('surfaces INVALID_CREDENTIALS for a wrong password', async () => {
    const {client} = seededCloud();
    await expect(
      client.login('SUNSET', 'A-101', 'wrong'),
    ).rejects.toHaveProperty('code', 'INVALID_CREDENTIALS');
  });
});

describe('auth flow — registration (ungated)', () => {
  it('registers a new flat and the account can immediately log in', async () => {
    const {cloud, client} = seededCloud();

    const registered = await client.register({
      societyName: 'SUNSET',
      flatNumber: 'B-204',
      residentName: 'Asha Rao',
      password: 'pw-asha-204',
    });
    expect(registered.token).toBeTruthy(); // auto-login, no approval step
    expect(cloud.subscriberCount()).toBe(2); // seeded A-101 + new B-204

    const relogin = await client.login('SUNSET', 'B-204', 'pw-asha-204');
    expect(relogin.subscriber.flatNumber).toBe('B-204');
  });

  it('rejects registration into a society that does not exist', async () => {
    const {client} = seededCloud();
    await expect(
      client.register({
        societyName: 'NOWHERE',
        flatNumber: 'B-204',
        residentName: 'Asha Rao',
        password: 'pw-asha-204',
      }),
    ).rejects.toHaveProperty('code', 'UNKNOWN_SOCIETY');
  });
});
