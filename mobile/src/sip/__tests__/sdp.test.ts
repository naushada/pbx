/**
 * TDD layer M2.a — SDP codec ordering.
 */
import {preferOpus} from '../sdp';

const SDP = [
  'v=0',
  'm=audio 9 UDP/TLS/RTP/SAVPF 0 8 111',
  'a=rtpmap:0 PCMU/8000',
  'a=rtpmap:8 PCMA/8000',
  'a=rtpmap:111 opus/48000/2',
].join('\r\n');

describe('preferOpus', () => {
  it('moves the Opus payload type to the front of the audio m-line', () => {
    const mline = preferOpus(SDP)
      .split('\r\n')
      .find(l => l.startsWith('m=audio'));
    expect(mline).toBe('m=audio 9 UDP/TLS/RTP/SAVPF 111 0 8');
  });

  it('leaves an SDP that offers no Opus unchanged', () => {
    const noOpus = [
      'v=0',
      'm=audio 9 RTP/AVP 0 8',
      'a=rtpmap:0 PCMU/8000',
    ].join('\r\n');
    expect(preferOpus(noOpus)).toBe(noOpus);
  });

  it('is idempotent when Opus is already first', () => {
    const once = preferOpus(SDP);
    expect(preferOpus(once)).toBe(once);
  });
});
