/**
 * fakeCloudTransport — TDD test infrastructure (M1.d).
 *
 * Adapts a `FakeCloud` (the in-memory backend double, M0.3) to the
 * `HttpTransport` interface, so a *real* `CloudClient` can be driven
 * end-to-end against it in integration tests — exercising the client's
 * request building and status→ApiError mapping for real.
 */
import {FakeCloud, FakeCloudError, FakeCloudErrorCode} from './fakeCloud';
import {HttpResponse, HttpTransport} from '../api/http';

export function fakeCloudTransport(cloud: FakeCloud): HttpTransport {
  return {
    async request(method, path, body): Promise<HttpResponse> {
      try {
        if (method === 'POST' && path === '/api/v1/subscriber/login') {
          const b = body as {
            societyName: string;
            flatNumber: string;
            password: string;
          };
          return {
            status: 200,
            body: cloud.login(b.societyName, b.flatNumber, b.password),
          };
        }
        if (method === 'POST' && path === '/api/v1/subscriber/register') {
          const b = body as {
            societyName: string;
            flatNumber: string;
            residentName: string;
            password: string;
            mobile?: string;
            email?: string;
          };
          return {status: 201, body: cloud.register(b)};
        }
        if (method === 'GET' && path.startsWith('/api/v1/society/resolve')) {
          const name = decodeURIComponent(
            path.split('name=')[1] ?? '',
          );
          return {status: 200, body: {societyId: cloud.resolveSociety(name)}};
        }
        return {status: 404, body: {error: 'no such route'}};
      } catch (e) {
        if (e instanceof FakeCloudError) {
          return {status: statusFor(e.code), body: {error: e.code}};
        }
        throw e;
      }
    },
  };
}

function statusFor(code: FakeCloudErrorCode): number {
  switch (code) {
    case 'UNKNOWN_SOCIETY':
      return 404;
    case 'INVALID_CREDENTIALS':
      return 401;
    default:
      return 500;
  }
}
