/**
 * SDP codec ordering — TDD layer M2.a.
 *
 * The on-prem Asterisk image ships no Opus transcoder (RELEASE-NOTES.md
 * / PR #120): every leg of a call must speak Opus or the bridge passes
 * no audio. `preferOpus` re-orders the audio m-line so the Opus payload
 * type is offered first — making Opus the negotiated codec whenever the
 * far end supports it.
 */

/** Move the Opus payload type to the front of the `m=audio` m-line. */
export function preferOpus(sdp: string): string {
  const lines = sdp.split(/\r\n|\n/);

  // Opus' payload type is whatever number its `a=rtpmap` line declares.
  let opusPt: string | null = null;
  for (const line of lines) {
    const m = /^a=rtpmap:(\d+)\s+opus\//i.exec(line);
    if (m) {
      opusPt = m[1];
      break;
    }
  }
  if (opusPt === null) {
    return sdp; // no Opus offered — nothing to reorder.
  }

  const opus = opusPt;
  return lines
    .map(line => {
      if (!line.startsWith('m=audio ')) return line;
      // m=audio <port> <proto> <pt> <pt> ...
      const parts = line.split(' ');
      if (parts.length <= 3 || !parts.slice(3).includes(opus)) return line;
      const head = parts.slice(0, 3);
      const pts = parts.slice(3);
      return [...head, opus, ...pts.filter(pt => pt !== opus)].join(' ');
    })
    .join('\r\n');
}
