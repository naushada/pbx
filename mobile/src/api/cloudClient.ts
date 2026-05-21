/**
 * CloudClient — TDD layer M1.a.
 *
 * The app's single door to the onprem-pbx cloud REST API. Every method
 * resolves to a typed value or rejects with `ApiError` — HTTP status
 * codes and transport failures never leak past this class.
 */
import {ApiError, Session} from './types';
import {HttpResponse, HttpTransport, TransportError} from './http';

export interface RegisterRequest {
  societyName: string;
  flatNumber: string;
  residentName: string;
  password: string;
  mobile?: string;
  email?: string;
}

export class CloudClient {
  constructor(private readonly transport: HttpTransport) {}

  /** POST /api/v1/subscriber/login */
  async login(
    societyName: string,
    flatNumber: string,
    password: string,
  ): Promise<Session> {
    const res = await this.send('POST', '/api/v1/subscriber/login', {
      societyName,
      flatNumber,
      password,
    });
    return this.sessionFrom(res);
  }

  /**
   * POST /api/v1/subscriber/register — ungated; the returned session
   * auto-logs-in the new account. Optional fields are sent only when
   * non-empty.
   */
  async register(req: RegisterRequest): Promise<Session> {
    const body: Record<string, unknown> = {
      societyName: req.societyName,
      flatNumber: req.flatNumber,
      residentName: req.residentName,
      password: req.password,
    };
    if (req.mobile && req.mobile.trim()) {
      body.mobile = req.mobile.trim();
    }
    if (req.email && req.email.trim()) {
      body.email = req.email.trim();
    }
    const res = await this.send('POST', '/api/v1/subscriber/register', body);
    return this.sessionFrom(res);
  }

  /** GET /api/v1/society/resolve — society name/code → societyId. */
  async resolveSociety(name: string): Promise<string> {
    const res = await this.send(
      'GET',
      `/api/v1/society/resolve?name=${encodeURIComponent(name)}`,
    );
    if (isOk(res)) {
      const id = (res.body as {societyId?: string} | null)?.societyId;
      if (typeof id === 'string' && id) {
        return id;
      }
      throw new ApiError('SERVER', 'malformed society-resolve response');
    }
    throw errorFor(res);
  }

  private async send(
    method: 'GET' | 'POST',
    path: string,
    body?: unknown,
  ): Promise<HttpResponse> {
    try {
      return await this.transport.request(method, path, body);
    } catch (e) {
      if (e instanceof TransportError) {
        throw new ApiError('NETWORK', e.message);
      }
      throw e;
    }
  }

  private sessionFrom(res: HttpResponse): Session {
    if (isOk(res)) {
      const s = res.body as Session | null;
      if (s && typeof s.token === 'string' && s.subscriber) {
        return s;
      }
      throw new ApiError('SERVER', 'malformed session response');
    }
    throw errorFor(res);
  }
}

function isOk(res: HttpResponse): boolean {
  return res.status >= 200 && res.status < 300;
}

function errorFor(res: HttpResponse): ApiError {
  switch (res.status) {
    case 401:
      return new ApiError('INVALID_CREDENTIALS', 'invalid credentials');
    case 404:
      return new ApiError('UNKNOWN_SOCIETY', 'society not found');
    case 409:
      return new ApiError('DUPLICATE', 'account already exists');
    case 429:
      return new ApiError('RATE_LIMITED', 'rate limited');
    default:
      return new ApiError('SERVER', `server returned ${res.status}`);
  }
}
