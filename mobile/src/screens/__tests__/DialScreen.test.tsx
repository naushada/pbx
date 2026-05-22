/**
 * TDD layer M2.d — DialScreen.
 *
 * Rendered in isolation with a fake CallController; a call going active
 * is simulated via `controller.emit(...)`.
 */
import React from 'react';
import {act, fireEvent, render, screen} from '@testing-library/react-native';
import {DialScreen} from '../DialScreen';
import {DepsProvider} from '../../state/deps';
import {FakeCallController} from '../../test/fakeCallController';
import {callReducer, idleCall} from '../../sip/callState';
import {Session} from '../../api/types';
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

function renderDial() {
  const callController = new FakeCallController();
  const client = {} as unknown as CloudClient;
  const sessionStore = {
    save: jest.fn(),
    load: jest.fn(),
    clear: jest.fn(),
  } as unknown as SessionStoreApi;
  render(
    <DepsProvider value={{client, sessionStore, callController}}>
      <DialScreen
        navigation={{} as never}
        route={
          {key: 'd', name: 'Dial', params: {session: SESSION}} as never
        }
      />
    </DepsProvider>,
  );
  return {callController};
}

describe('DialScreen', () => {
  it('shows the signed-in flat and resident name', () => {
    renderDial();
    expect(screen.getByText('A-101')).toBeTruthy();
    expect(screen.getByText('Resident A101')).toBeTruthy();
  });

  it('does not place a call until a dialable flat is entered', () => {
    const {callController} = renderDial();
    fireEvent.press(screen.getByTestId('dial-call')); // flat is empty
    expect(callController.placeCall).not.toHaveBeenCalled();

    fireEvent.changeText(screen.getByTestId('dial-flat'), 'B-204');
    fireEvent.press(screen.getByTestId('dial-call'));
    expect(callController.placeCall).toHaveBeenCalledWith('B-204');
  });

  it('hands the screen over to the in-call panel while a call is active', () => {
    const {callController} = renderDial();
    expect(screen.getByTestId('dial-screen')).toBeTruthy();

    act(() => {
      callController.emit(
        callReducer(idleCall, {type: 'dial', target: 'sip:B-204@pbx.local'}),
      );
    });

    expect(screen.queryByTestId('dial-screen')).toBeNull();
    expect(screen.getByTestId('incall-panel')).toBeTruthy();
  });
});
