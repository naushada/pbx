/**
 * TDD layer M1.c — LoginScreen.
 *
 * Rendered in isolation with fake deps (DepsProvider) and a fake
 * navigation object — no navigator, so the test stays robust and fast.
 */
import React from 'react';
import {fireEvent, render, screen, waitFor} from '@testing-library/react-native';
import {LoginScreen} from '../LoginScreen';
import {DepsProvider} from '../../state/deps';
import {ApiError, Session} from '../../api/types';
import {StubCallController} from '../../call/callController';
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

function renderLogin(login: jest.Mock = jest.fn()) {
  const client = {login} as unknown as CloudClient;
  const sessionStore: SessionStoreApi = {
    save: jest.fn(async () => {}),
    load: jest.fn(async () => null),
    clear: jest.fn(async () => {}),
  };
  const navigation = {navigate: jest.fn(), replace: jest.fn()};
  render(
    <DepsProvider
      value={{client, sessionStore, callController: new StubCallController()}}>
      <LoginScreen
        navigation={navigation as never}
        route={{key: 'l', name: 'Login', params: undefined} as never}
      />
    </DepsProvider>,
  );
  return {login, sessionStore, navigation};
}

function fillLogin(society = 'SUNSET', flat = 'A-101', password = 'pw-a101') {
  fireEvent.changeText(screen.getByTestId('login-society'), society);
  fireEvent.changeText(screen.getByTestId('login-flat'), flat);
  fireEvent.changeText(screen.getByTestId('login-password'), password);
}

describe('LoginScreen', () => {
  it('renders the society, flat and password fields plus the actions', () => {
    renderLogin();
    expect(screen.getByTestId('login-society')).toBeTruthy();
    expect(screen.getByTestId('login-flat')).toBeTruthy();
    expect(screen.getByTestId('login-password')).toBeTruthy();
    expect(screen.getByTestId('login-submit')).toBeTruthy();
    expect(screen.getByTestId('login-to-register')).toBeTruthy();
  });

  it('blocks submit and shows field errors when required fields are empty', () => {
    const {login} = renderLogin();
    fireEvent.press(screen.getByTestId('login-submit'));
    expect(login).not.toHaveBeenCalled();
    expect(screen.getByTestId('login-society-error')).toBeTruthy();
    expect(screen.getByTestId('login-flat-error')).toBeTruthy();
    expect(screen.getByTestId('login-password-error')).toBeTruthy();
  });

  it('submits the typed credentials to the cloud', async () => {
    const login = jest.fn(async () => SESSION);
    renderLogin(login);
    fillLogin();
    fireEvent.press(screen.getByTestId('login-submit'));
    await waitFor(() =>
      expect(login).toHaveBeenCalledWith('SUNSET', 'A-101', 'pw-a101'),
    );
  });

  it('persists the session and navigates to Dial on success', async () => {
    const login = jest.fn(async () => SESSION);
    const {sessionStore, navigation} = renderLogin(login);
    fillLogin();
    fireEvent.press(screen.getByTestId('login-submit'));
    await waitFor(() => expect(sessionStore.save).toHaveBeenCalledWith(SESSION));
    expect(navigation.replace).toHaveBeenCalledWith('Dial', {session: SESSION});
  });

  it('shows the error message and does not navigate when login fails', async () => {
    const login = jest.fn(async () => {
      throw new ApiError('INVALID_CREDENTIALS', 'x');
    });
    const {navigation} = renderLogin(login);
    fillLogin('SUNSET', 'A-101', 'wrong');
    fireEvent.press(screen.getByTestId('login-submit'));
    await waitFor(() => expect(screen.getByTestId('login-error')).toBeTruthy());
    expect(screen.getByText(/wrong flat number/i)).toBeTruthy();
    expect(navigation.replace).not.toHaveBeenCalled();
  });

  it('routes to Create account', () => {
    const {navigation} = renderLogin();
    fireEvent.press(screen.getByTestId('login-to-register'));
    expect(navigation.navigate).toHaveBeenCalledWith('Register');
  });
});
