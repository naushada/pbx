/**
 * TDD layer M1.c — RegisterScreen.
 *
 * Rendered in isolation with fake deps + fake navigation, same as the
 * LoginScreen test.
 */
import React from 'react';
import {fireEvent, render, screen, waitFor} from '@testing-library/react-native';
import {RegisterScreen} from '../RegisterScreen';
import {DepsProvider} from '../../state/deps';
import {ApiError, Session} from '../../api/types';
import {StubCallController} from '../../call/callController';
import type {CloudClient} from '../../api/cloudClient';
import type {SessionStoreApi} from '../../session/sessionStore';

const SESSION: Session = {
  token: 'tok',
  subscriber: {
    societyId: 'soc_sunset',
    flatNumber: 'B-204',
    sipUsername: 'soc-sunset-b-204-1',
    displayName: 'Asha Rao',
    role: 'resident',
  },
};

function renderRegister(register: jest.Mock = jest.fn()) {
  const client = {register} as unknown as CloudClient;
  const sessionStore: SessionStoreApi = {
    save: jest.fn(async () => {}),
    load: jest.fn(async () => null),
    clear: jest.fn(async () => {}),
  };
  const navigation = {navigate: jest.fn(), replace: jest.fn()};
  render(
    <DepsProvider
      value={{client, sessionStore, callController: new StubCallController()}}>
      <RegisterScreen
        navigation={navigation as never}
        route={{key: 'r', name: 'Register', params: undefined} as never}
      />
    </DepsProvider>,
  );
  return {register, sessionStore, navigation};
}

/** Fills the four required fields; leaves mobile + email blank. */
function fillRequired() {
  fireEvent.changeText(screen.getByTestId('register-society'), 'SUNSET');
  fireEvent.changeText(screen.getByTestId('register-flat'), 'B-204');
  fireEvent.changeText(screen.getByTestId('register-name'), 'Asha Rao');
  fireEvent.changeText(screen.getByTestId('register-password'), 'pw-asha-204');
}

describe('RegisterScreen', () => {
  it('renders the required fields and marks mobile + email optional', () => {
    renderRegister();
    expect(screen.getByTestId('register-society')).toBeTruthy();
    expect(screen.getByTestId('register-flat')).toBeTruthy();
    expect(screen.getByTestId('register-name')).toBeTruthy();
    expect(screen.getByTestId('register-mobile')).toBeTruthy();
    expect(screen.getByTestId('register-email')).toBeTruthy();
    expect(screen.getByTestId('register-password')).toBeTruthy();
    // mobile + email labels carry the "optional" marker.
    expect(screen.getAllByText(/optional/i).length).toBeGreaterThanOrEqual(2);
  });

  it('registers with mobile + email left blank', async () => {
    const register = jest.fn(async () => SESSION);
    renderRegister(register);
    fillRequired();
    fireEvent.press(screen.getByTestId('register-submit'));
    await waitFor(() => expect(register).toHaveBeenCalledTimes(1));
    expect(register.mock.calls[0][0]).toMatchObject({
      societyName: 'SUNSET',
      flatNumber: 'B-204',
      residentName: 'Asha Rao',
      password: 'pw-asha-204',
    });
  });

  it('auto-logs-in straight to Dial on success (ungated registration)', async () => {
    const register = jest.fn(async () => SESSION);
    const {sessionStore, navigation} = renderRegister(register);
    fillRequired();
    fireEvent.press(screen.getByTestId('register-submit'));
    await waitFor(() => expect(sessionStore.save).toHaveBeenCalledWith(SESSION));
    expect(navigation.replace).toHaveBeenCalledWith('Dial', {session: SESSION});
  });

  it('surfaces a duplicate-account error and does not navigate', async () => {
    const register = jest.fn(async () => {
      throw new ApiError('DUPLICATE', 'x');
    });
    const {navigation} = renderRegister(register);
    fillRequired();
    fireEvent.press(screen.getByTestId('register-submit'));
    await waitFor(() => expect(screen.getByTestId('register-error')).toBeTruthy());
    expect(screen.getByText(/already exists/i)).toBeTruthy();
    expect(navigation.replace).not.toHaveBeenCalled();
  });

  it('rejects a malformed email before calling the API', () => {
    const register = jest.fn();
    renderRegister(register);
    fillRequired();
    fireEvent.changeText(screen.getByTestId('register-email'), 'not-an-email');
    fireEvent.press(screen.getByTestId('register-submit'));
    expect(register).not.toHaveBeenCalled();
    expect(screen.getByTestId('register-email-error')).toBeTruthy();
  });
});
