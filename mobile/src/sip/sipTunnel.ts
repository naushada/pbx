/**
 * SipTunnel — TDD layer M2.b.
 *
 * The WebSocket connectivity layer under SIP signalling: it opens
 * `wss://<cloud>/sip-ws?token=<token>`, delivers frames both ways, and
 * transparently reconnects with backoff when the socket drops. The SIP
 * protocol itself (REGISTER / INVITE, via sip.js's UserAgent) is bound
 * on top of this in the next slice.
 *
 * The WebSocket and the reconnect timer are both injected, so the whole
 * lifecycle — connect, drop, backoff, reconnect — is unit-testable with
 * no real socket and no real clock.
 */

export type TunnelState = 'idle' | 'connecting' | 'open' | 'closed';

/**
 * The minimal WebSocket surface SipTunnel needs. React Native's
 * `WebSocket` and the browser `WebSocket` both satisfy it; tests inject
 * a fake.
 */
export interface TunnelSocket {
  send(data: string): void;
  close(): void;
  onopen: (() => void) | null;
  onclose: (() => void) | null;
  onerror: ((err: unknown) => void) | null;
  onmessage: ((event: {data: string}) => void) | null;
}

export type SocketFactory = (url: string) => TunnelSocket;
type TimerHandle = unknown;

export interface SipTunnelOptions {
  /** Cloud origin, e.g. `wss://pabx-….herokuapp.com`. */
  baseUrl: string;
  socketFactory: SocketFactory;
  /** Reconnect backoff schedule (ms); the last entry repeats. */
  backoffMs?: number[];
  /** Injectable timer — defaults to setTimeout/clearTimeout. */
  setTimer?: (fn: () => void, ms: number) => TimerHandle;
  clearTimer?: (handle: TimerHandle) => void;
}

const DEFAULT_BACKOFF_MS = [500, 1000, 2000, 5000, 10000];

export class SipTunnel {
  private readonly baseUrl: string;
  private readonly socketFactory: SocketFactory;
  private readonly backoff: number[];
  private readonly setTimer: (fn: () => void, ms: number) => TimerHandle;
  private readonly clearTimer: (handle: TimerHandle) => void;

  private socket: TunnelSocket | null = null;
  private currentState: TunnelState = 'idle';
  private token = '';
  private deliberateClose = false;
  private reconnectAttempt = 0;
  private reconnectTimer: TimerHandle | null = null;

  private messageHandler: ((data: string) => void) | null = null;
  private openHandler: (() => void) | null = null;
  private closeHandler: (() => void) | null = null;

  constructor(opts: SipTunnelOptions) {
    this.baseUrl = opts.baseUrl.replace(/\/+$/, '');
    this.socketFactory = opts.socketFactory;
    this.backoff =
      opts.backoffMs && opts.backoffMs.length > 0
        ? opts.backoffMs
        : DEFAULT_BACKOFF_MS;
    this.setTimer =
      opts.setTimer ?? ((fn, ms) => setTimeout(fn, ms) as unknown);
    this.clearTimer =
      opts.clearTimer ?? (handle => clearTimeout(handle as never));
  }

  get state(): TunnelState {
    return this.currentState;
  }

  onMessage(cb: (data: string) => void): void {
    this.messageHandler = cb;
  }
  onOpen(cb: () => void): void {
    this.openHandler = cb;
  }
  /** Fired on an *unexpected* close (a reconnect will follow). */
  onClose(cb: () => void): void {
    this.closeHandler = cb;
  }

  /**
   * Open the tunnel. Throws synchronously if `token` is empty — the
   * cloud's `/sip-ws` bearer-token is the only SIP auth, so a tunnel
   * without one is never valid.
   */
  connect(token: string): void {
    if (!token) {
      throw new Error('SipTunnel.connect requires a session token');
    }
    this.token = token;
    this.deliberateClose = false;
    this.reconnectAttempt = 0;
    this.openSocket();
  }

  /** Close the tunnel deliberately — suppresses reconnect. */
  close(): void {
    this.deliberateClose = true;
    if (this.reconnectTimer !== null) {
      this.clearTimer(this.reconnectTimer);
      this.reconnectTimer = null;
    }
    if (this.socket) {
      this.socket.close();
      this.socket = null;
    }
    this.currentState = 'closed';
  }

  /** Send a frame. Throws if the tunnel is not currently open. */
  send(data: string): void {
    if (this.currentState !== 'open' || !this.socket) {
      throw new Error('SipTunnel.send: tunnel is not open');
    }
    this.socket.send(data);
  }

  private url(): string {
    return `${this.baseUrl}/sip-ws?token=${encodeURIComponent(this.token)}`;
  }

  private openSocket(): void {
    this.currentState = 'connecting';
    const sock = this.socketFactory(this.url());
    this.socket = sock;

    sock.onopen = () => {
      this.currentState = 'open';
      this.reconnectAttempt = 0; // a clean open resets the backoff
      this.openHandler?.();
    };
    sock.onmessage = event => {
      this.messageHandler?.(event.data);
    };
    sock.onerror = () => {
      // A failed socket always follows with onclose — handle it there.
    };
    sock.onclose = () => {
      this.socket = null;
      this.currentState = 'closed';
      if (this.deliberateClose) {
        return;
      }
      this.closeHandler?.();
      this.scheduleReconnect();
    };
  }

  private scheduleReconnect(): void {
    const idx = Math.min(this.reconnectAttempt, this.backoff.length - 1);
    const delay = this.backoff[idx];
    this.reconnectAttempt += 1;
    this.reconnectTimer = this.setTimer(() => {
      this.reconnectTimer = null;
      if (!this.deliberateClose) {
        this.openSocket();
      }
    }, delay);
  }
}
