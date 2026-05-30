/**
 * CloudClient — TDD layer M1.a.
 *
 * The app's single door to the onprem-pbx cloud REST API. Every method
 * resolves to a typed value or rejects with `ApiError` — HTTP status
 * codes and transport failures never leak past this class.
 */
import {ApiError, CallRecord, DirectoryEntry, Session} from './types';
import {HttpResponse, HttpTransport, TransportError} from './http';

export interface RegisterRequest {
  societyName: string;
  flatNumber: string;
  residentName: string;
  password: string;
  mobile?: string;
  email?: string;
}

export interface DeviceRegistration {
  sipUsername: string;
  platform: 'ios' | 'android';
  /** APNs (PushKit) or FCM device token. */
  token: string;
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

  /**
   * GET /api/v1/subscriber?societyId=…[&flatPrefix=…] — society-scoped
   * directory listing. Same endpoint the web softphone's
   * `DirectoryComponent` uses. Returns an empty array if the cloud has
   * no DB configured (the handler's safe default).
   */
  async getDirectory(
    societyId: string,
    flatPrefix?: string,
  ): Promise<DirectoryEntry[]> {
    let path = `/api/v1/subscriber?societyId=${encodeURIComponent(societyId)}`;
    if (flatPrefix && flatPrefix.trim()) {
      path += `&flatPrefix=${encodeURIComponent(flatPrefix.trim())}`;
    }
    const res = await this.send('GET', path);
    if (!isOk(res)) {
      throw errorFor(res);
    }
    const body = res.body;
    if (!Array.isArray(body)) {
      throw new ApiError('SERVER', 'malformed directory response');
    }
    return body as DirectoryEntry[];
  }

  /**
   * GET /api/v1/cdr?societyId=… — society-scoped CDR list. The cloud
   * does not filter by flat today, so HistoryScreen filters client-
   * side to rows where `fromFlat === self || toFlat === self`. Returns
   * an empty array if the cloud has no DB configured (the handler's
   * safe default).
   */
  async getCallHistory(societyId: string): Promise<CallRecord[]> {
    const res = await this.send(
      'GET',
      `/api/v1/cdr?societyId=${encodeURIComponent(societyId)}`,
    );
    if (!isOk(res)) {
      throw errorFor(res);
    }
    const body = res.body;
    if (!Array.isArray(body)) {
      throw new ApiError('SERVER', 'malformed call-history response');
    }
    return body as CallRecord[];
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

  /**
   * POST /api/v1/push/device — register this device's APNs/FCM token so
   * the cloud can wake the app for an incoming call.
   */
  async registerDevice(reg: DeviceRegistration): Promise<void> {
    const res = await this.send('POST', '/api/v1/push/device', {
      sipUsername: reg.sipUsername,
      platform: reg.platform,
      token: reg.token,
    });
    if (isOk(res)) return;
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
