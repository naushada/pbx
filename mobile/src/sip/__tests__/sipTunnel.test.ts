/**
 * TDD layer M2.b — SipTunnel.
 *
 * The WebSocket and the reconnect timer are injected, so connect /
 * drop / backoff / reconnect are all driven deterministically here —
 * no real socket, no real clock.
 */
import {SipTunnel, TunnelSocket} from '../sipTunnel';

class FakeSocket implements TunnelSocket {
  sent: string[] = [];
  closed = false;
  onopen: (() => void) | null = null;
  onclose: (() => void) | null = null;
  onerror: ((err: unknown) => void) | null = null;
  onmessage: ((event: {data: string}) => void) | null = null;
  constructor(readonly url: string) {}
  send(data: string): void {
    this.sent.push(data);
  }
  close(): void {
    this.closed = true;
  }
  fireOpen(): void {
    this.onopen?.();
  }
  fireClose(): void {
    this.onclose?.();
  }
  fireMessage(data: string): void {
    this.onmessage?.({data});
  }
}

interface PendingTimer {
  fn: () => void;
  ms: number;
}

class FakeClock {
  pending: PendingTimer[] = [];
  setTimer = (fn: () => void, ms: number): PendingTimer => {
    const timer: PendingTimer = {fn, ms};
    this.pending.push(timer);
    return timer;
  };
  clearTimer = (handle: unknown): void => {
    this.pending = this.pending.filter(t => t !== handle);
  };
  /** Run and drop the oldest pending timer. */
  runNext(): void {
    const timer = this.pending.shift();
    if (timer) timer.fn();
  }
}

function setup(backoffMs: number[] = [10, 20, 40]) {
  const sockets: FakeSocket[] = [];
  const clock = new FakeClock();
  const tunnel = new SipTunnel({
    baseUrl: 'wss://cloud',
    socketFactory: url => {
      const sock = new FakeSocket(url);
      sockets.push(sock);
      return sock;
    },
    backoffMs,
    setTimer: clock.setTimer,
    clearTimer: clock.clearTimer,
  });
  return {tunnel, sockets, clock};
}

const last = (sockets: FakeSocket[]): FakeSocket =>
  sockets[sockets.length - 1];

describe('SipTunnel.connect', () => {
  it('throws synchronously when the token is empty', () => {
    const {tunnel} = setup();
    expect(() => tunnel.connect('')).toThrow(/token/i);
  });

  it('opens wss://<base>/sip-ws with the token in the query', () => {
    const {tunnel, sockets} = setup();
    tunnel.connect('tok-123');
    expect(sockets).toHaveLength(1);
    expect(last(sockets).url).toBe('wss://cloud/sip-ws?token=tok-123');
    expect(tunnel.state).toBe('connecting');
  });

  it('reaches "open" and fires onOpen once the socket opens', () => {
    const {tunnel, sockets} = setup();
    const opened = jest.fn();
    tunnel.onOpen(opened);
    tunnel.connect('tok');
    last(sockets).fireOpen();
    expect(tunnel.state).toBe('open');
    expect(opened).toHaveBeenCalledTimes(1);
  });
});

describe('SipTunnel.send', () => {
  it('throws before the tunnel is open', () => {
    const {tunnel} = setup();
    tunnel.connect('tok');
    expect(() => tunnel.send('REGISTER sip:...')).toThrow(/not open/i);
  });

  it('writes the frame to the socket once open', () => {
    const {tunnel, sockets} = setup();
    tunnel.connect('tok');
    last(sockets).fireOpen();
    tunnel.send('REGISTER sip:...');
    expect(last(sockets).sent).toEqual(['REGISTER sip:...']);
  });
});

describe('SipTunnel messages', () => {
  it('delivers inbound frames to the onMessage handler', () => {
    const {tunnel, sockets} = setup();
    const seen: string[] = [];
    tunnel.onMessage(data => seen.push(data));
    tunnel.connect('tok');
    last(sockets).fireOpen();
    last(sockets).fireMessage('SIP/2.0 200 OK');
    expect(seen).toEqual(['SIP/2.0 200 OK']);
  });
});

describe('SipTunnel reconnect', () => {
  it('reconnects after an unexpected drop, on the first backoff step', () => {
    const {tunnel, sockets, clock} = setup([10, 20, 40]);
    const dropped = jest.fn();
    tunnel.onClose(dropped);
    tunnel.connect('tok');
    last(sockets).fireOpen();

    last(sockets).fireClose(); // unexpected drop
    expect(tunnel.state).toBe('closed');
    expect(dropped).toHaveBeenCalledTimes(1);
    expect(clock.pending).toHaveLength(1);
    expect(clock.pending[0].ms).toBe(10);

    clock.runNext(); // backoff elapses → reconnect
    expect(sockets).toHaveLength(2);
    expect(last(sockets).url).toBe('wss://cloud/sip-ws?token=tok');
  });

  it('walks the backoff schedule across repeated drops', () => {
    const {tunnel, sockets, clock} = setup([10, 20, 40]);
    tunnel.connect('tok');
    last(sockets).fireClose();
    expect(clock.pending[0].ms).toBe(10);
    clock.runNext();
    last(sockets).fireClose();
    expect(clock.pending[0].ms).toBe(20);
    clock.runNext();
    last(sockets).fireClose();
    expect(clock.pending[0].ms).toBe(40);
  });

  it('a clean re-open resets the backoff', () => {
    const {tunnel, sockets, clock} = setup([10, 20, 40]);
    tunnel.connect('tok');
    last(sockets).fireClose(); // drop 1 → backoff[0]
    clock.runNext();
    last(sockets).fireOpen(); // reconnected and opened cleanly
    last(sockets).fireClose(); // next drop starts from backoff[0] again
    expect(clock.pending[0].ms).toBe(10);
  });

  it('does not reconnect after a deliberate close', () => {
    const {tunnel, sockets, clock} = setup();
    tunnel.connect('tok');
    last(sockets).fireOpen();
    tunnel.close();
    expect(tunnel.state).toBe('closed');
    expect(last(sockets).closed).toBe(true);
    last(sockets).fireClose(); // the socket's close event still arrives
    expect(clock.pending).toHaveLength(0);
  });
});
