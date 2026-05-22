/**
 * HTTP transport seam — TDD layer M1.
 *
 * `CloudClient` depends on the `HttpTransport` interface, not on
 * `fetch` directly. Production wires `FetchTransport`; tests inject a
 * scripted transport (to assert request shape) or a FakeCloud-backed
 * one (`src/test/fakeCloudTransport.ts`) — no global `fetch` mocking.
 */

export interface HttpResponse {
  status: number;
  body: unknown;
}

export type HttpMethod = 'GET' | 'POST';

export interface HttpTransport {
  request(
    method: HttpMethod,
    path: string,
    body?: unknown,
  ): Promise<HttpResponse>;
}

/** Thrown by a transport when the request never reached a server. */
export class TransportError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'TransportError';
  }
}

/** Production transport — talks to the real cloud over `fetch`. */
export class FetchTransport implements HttpTransport {
  constructor(
    private readonly baseUrl: string,
    private readonly fetchImpl: typeof fetch = fetch,
  ) {}

  async request(
    method: HttpMethod,
    path: string,
    body?: unknown,
  ): Promise<HttpResponse> {
    let res: Response;
    try {
      res = await this.fetchImpl(this.baseUrl + path, {
        method,
        headers: {'Content-Type': 'application/json'},
        body: body === undefined ? undefined : JSON.stringify(body),
      });
    } catch (e) {
      // DNS / connection / TLS failure — never reached a server.
      throw new TransportError(e instanceof Error ? e.message : String(e));
    }

    const text = await res.text();
    let parsed: unknown;
    if (text) {
      try {
        parsed = JSON.parse(text);
      } catch {
        parsed = text;
      }
    }
    return {status: res.status, body: parsed};
  }
}
