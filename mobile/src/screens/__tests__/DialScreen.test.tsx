/**
 * TDD layer M2.d — DialScreen.
 *
 * Rendered in isolation with a fake CallController; a call going active
 * is simulated via `controller.emit(...)`.
 */
import React from 'react';
import {act, fireEvent, render, screen, waitFor} from '@testing-library/react-native';
import {DialScreen} from '../DialScreen';
import {DepsProvider} from '../../state/deps';
import {AuthProvider} from '../../state/authContext';
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

function renderDial(opts?: {clearError?: unknown}) {
  const callController = new FakeCallController();
  const client = {} as unknown as CloudClient;
  const sessionStore = {
    save: jest.fn(),
    load: jest.fn(),
    clear: jest.fn().mockImplementation(
      opts?.clearError
        ? () => Promise.reject(opts.clearError)
        : () => Promise.resolve(),
    ),
  } as unknown as SessionStoreApi;
  const setSession = jest.fn();
  const navigation = {reset: jest.fn()} as unknown as never;
  render(
    <AuthProvider value={{session: SESSION, setSession}}>
      <DepsProvider value={{client, sessionStore, callController}}>
        <DialScreen
          navigation={navigation}
          route={
            {key: 'd', name: 'Dial', params: {session: SESSION}} as never
          }
        />
      </DepsProvider>
    </AuthProvider>,
  );
  return {callController, sessionStore, setSession, navigation};
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

  describe('sign out', () => {
    it('renders the sign-out button on the idle dialer', () => {
      renderDial();
      expect(screen.getByTestId('dial-signout')).toBeTruthy();
    });

    it('clears the session store, signals AuthContext, and resets nav to Login', async () => {
      const {sessionStore, setSession, navigation} = renderDial();

      fireEvent.press(screen.getByTestId('dial-signout'));

      await waitFor(() => expect(sessionStore.clear).toHaveBeenCalledTimes(1));
      expect(setSession).toHaveBeenCalledWith(null);
      expect(navigation.reset).toHaveBeenCalledWith({
        index: 0,
        routes: [{name: 'Login'}],
      });
    });

    it('still signs out locally if sessionStore.clear() rejects', async () => {
      const {setSession, navigation} = renderDial({
        clearError: new Error('keychain unavailable'),
      });

      fireEvent.press(screen.getByTestId('dial-signout'));

      await waitFor(() => expect(setSession).toHaveBeenCalledWith(null));
      expect(navigation.reset).toHaveBeenCalledWith({
        index: 0,
        routes: [{name: 'Login'}],
      });
    });
  });
});
