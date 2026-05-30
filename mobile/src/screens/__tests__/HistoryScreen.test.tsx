/**
 * HistoryScreen tests — call history with newest-first ordering.
 *
 * Rendered in isolation with a scripted `CloudClient.getCallHistory`
 * and a `FakeCallController` to assert tap-to-call.
 */
import React from 'react';
import {
  act,
  fireEvent,
  render,
  screen,
  waitFor,
} from '@testing-library/react-native';
import {HistoryScreen} from '../HistoryScreen';
import {DepsProvider} from '../../state/deps';
import {FakeCallController} from '../../test/fakeCallController';
import {callReducer, idleCall} from '../../sip/callState';
import {CallRecord, Session} from '../../api/types';
import type {CloudClient} from '../../api/cloudClient';
import type {SessionStoreApi} from '../../session/sessionStore';

const SESSION: Session = {
  token: 'tok',
  subscriber: {
    societyId: 'soc_sunset',
    flatNumber: 'A-101',
    sipUsername: 'a101',
    displayName: 'Resident A101',
    role: 'resident',
  },
};

function cdr(o: Partial<CallRecord>): CallRecord {
  return {
    callId: 'c-x',
    societyId: 'soc_sunset',
    fromFlat: 'A-101',
    toFlat: 'B-204',
    direction: 'outbound',
    type: 'p2p',
    startedAt: '2026-05-29T10:00:00Z',
    endedAt: '2026-05-29T10:01:00Z',
    durationSec: 60,
    hangupCause: 'normal',
    ...o,
  };
}

interface RenderOpts {
  rows?: CallRecord[];
  rejectWith?: unknown;
}

function renderHistory(opts: RenderOpts = {}) {
  const getCallHistory = jest.fn().mockImplementation(() => {
    if (opts.rejectWith) return Promise.reject(opts.rejectWith);
    return Promise.resolve(opts.rows ?? []);
  });
  const client = {getCallHistory} as unknown as CloudClient;
  const sessionStore = {
    save: jest.fn(),
    load: jest.fn(),
    clear: jest.fn(),
  } as unknown as SessionStoreApi;
  const callController = new FakeCallController();
  const goBack = jest.fn();
  const navigation = {goBack};
  render(
    <DepsProvider value={{client, sessionStore, callController}}>
      <HistoryScreen
        navigation={navigation as never}
        route={
          {key: 'h', name: 'History', params: {session: SESSION}} as never
        }
      />
    </DepsProvider>,
  );
  return {client, getCallHistory, callController, goBack};
}

describe('HistoryScreen', () => {
  it('fetches CDRs for the signed-in society on mount', async () => {
    const {getCallHistory} = renderHistory();

    await waitFor(() => expect(getCallHistory).toHaveBeenCalledTimes(1));
    expect(getCallHistory).toHaveBeenCalledWith('soc_sunset');
  });

  it('filters to rows involving the signed-in flat', async () => {
    renderHistory({
      rows: [
        cdr({callId: 'mine-out', fromFlat: 'A-101', toFlat: 'B-204'}),
        cdr({callId: 'mine-in', fromFlat: 'C-301', toFlat: 'A-101'}),
        cdr({callId: 'theirs', fromFlat: 'C-301', toFlat: 'D-402'}),
      ],
    });

    await waitFor(() => screen.getByTestId('history-row-mine-out'));
    expect(screen.queryByTestId('history-row-mine-in')).toBeTruthy();
    expect(screen.queryByTestId('history-row-theirs')).toBeNull();
  });

  it('sorts the visible rows newest-first by startedAt', async () => {
    renderHistory({
      rows: [
        cdr({callId: 'older', startedAt: '2026-05-29T08:00:00Z'}),
        cdr({callId: 'newer', startedAt: '2026-05-29T12:00:00Z'}),
      ],
    });

    await waitFor(() => screen.getByTestId('history-row-newer'));
    const list = screen.getByTestId('history-list');
    // FlatList passes data through `data=` — readable via props.
    const sortedIds = (list.props as {data: CallRecord[]}).data.map(
      r => r.callId,
    );
    expect(sortedIds).toEqual(['newer', 'older']);
  });

  it('tapping Call dispatches placeCall with the peer flat', async () => {
    const {callController} = renderHistory({
      rows: [
        cdr({callId: 'out', fromFlat: 'A-101', toFlat: 'B-204'}),
        cdr({
          callId: 'in',
          startedAt: '2026-05-29T11:00:00Z',
          fromFlat: 'C-301',
          toFlat: 'A-101',
        }),
      ],
    });

    await waitFor(() => screen.getByTestId('history-call-out'));

    // Outbound row → peer is `toFlat`.
    fireEvent.press(screen.getByTestId('history-call-out'));
    expect(callController.placeCall).toHaveBeenLastCalledWith('B-204');

    // Inbound row → peer is `fromFlat`.
    fireEvent.press(screen.getByTestId('history-call-in'));
    expect(callController.placeCall).toHaveBeenLastCalledWith('C-301');
  });

  it('hands the screen over to the in-call panel while a call is active', async () => {
    const {callController} = renderHistory({
      rows: [cdr({callId: 'r1', fromFlat: 'A-101', toFlat: 'B-204'})],
    });
    await waitFor(() => screen.getByTestId('history-screen'));

    act(() => {
      callController.emit(
        callReducer(idleCall, {type: 'dial', target: 'sip:B-204@pbx.local'}),
      );
    });

    expect(screen.queryByTestId('history-screen')).toBeNull();
    expect(screen.getByTestId('incall-panel')).toBeTruthy();
  });

  it('back link goes back to the dialer', async () => {
    const {goBack} = renderHistory();
    await waitFor(() => screen.getByTestId('history-back'));

    fireEvent.press(screen.getByTestId('history-back'));

    expect(goBack).toHaveBeenCalledTimes(1);
  });

  it('shows an empty-state when no CDRs involve the signed-in flat', async () => {
    renderHistory({rows: [cdr({callId: 'them', fromFlat: 'C-301', toFlat: 'D-402'})]});

    await waitFor(() =>
      expect(screen.queryByText(/no calls yet/i)).toBeTruthy(),
    );
  });

  it('shows an error message if getCallHistory rejects', async () => {
    renderHistory({rejectWith: new Error('cdr down')});

    await waitFor(() => expect(screen.queryByText(/cdr down/)).toBeTruthy());
  });
});
