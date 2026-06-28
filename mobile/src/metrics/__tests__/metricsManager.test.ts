/**
 * MetricsManager flush/transport behaviour.
 *
 * MetricsManager is a singleton, so each test clears its buffer first and
 * installs its own sink.
 */
import {MetricsManager} from '../metricsManager';

describe('MetricsManager flush', () => {
  const m = MetricsManager.getInstance();

  beforeEach(() => {
    m.clearMetrics();
  });

  it('sends buffered events via the sink and drops them on success', async () => {
    const sink = jest.fn().mockResolvedValue(undefined);
    m.setSink(sink);

    m.trackEvent('user_login', {societyId: 's1', flatNumber: 'A-1'});
    m.trackEvent('outbound_call', {destinationFlat: 'B-2'});
    await m.flush();

    expect(sink).toHaveBeenCalledTimes(1);
    expect(sink.mock.calls[0][0]).toHaveLength(2);
    expect(m.getMetrics()).toHaveLength(0);
  });

  it('keeps events buffered when the sink rejects (retry later)', async () => {
    const sink = jest.fn().mockRejectedValue(new Error('network down'));
    m.setSink(sink);

    m.trackEvent('app_start');
    await m.flush();

    expect(sink).toHaveBeenCalledTimes(1);
    expect(m.getMetrics()).toHaveLength(1);
  });

  it('is a no-op when there are no events', async () => {
    const sink = jest.fn().mockResolvedValue(undefined);
    m.setSink(sink);

    await m.flush();

    expect(sink).not.toHaveBeenCalled();
  });
});
