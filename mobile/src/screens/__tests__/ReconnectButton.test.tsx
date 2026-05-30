/**
 * ReconnectButton tests — visible only when the UA is offline, taps
 * call the context's `reconnect`, and the busy guard blocks double-
 * presses while the previous reconnect is in flight.
 */
import React from 'react';
import {fireEvent, render, screen, waitFor} from '@testing-library/react-native';
import {ReconnectButton} from '../ReconnectButton';
import {
  RegistrationProvider,
  RegistrationState,
} from '../../state/registrationContext';

interface RenderOpts {
  state: RegistrationState;
  reconnect?: () => Promise<void>;
}

function renderWith({state, reconnect}: RenderOpts) {
  const fn = jest.fn(reconnect ?? (async () => {}));
  render(
    <RegistrationProvider value={{state, reconnect: fn}}>
      <ReconnectButton />
    </RegistrationProvider>,
  );
  return {reconnect: fn};
}

describe('ReconnectButton', () => {
  it.each(['unregistered', 'terminated', 'unknown'] as RegistrationState[])(
    'renders for offline state %s',
    state => {
      renderWith({state});
      expect(screen.queryByTestId('reconnect-button')).toBeTruthy();
    },
  );

  it.each([
    'registered',
    'starting',
    'started',
    'registering',
  ] as RegistrationState[])('renders nothing for non-offline state %s', state => {
    renderWith({state});
    expect(screen.queryByTestId('reconnect-button')).toBeNull();
  });

  it('tapping the button invokes the context reconnect once', async () => {
    const {reconnect} = renderWith({state: 'terminated'});

    fireEvent.press(screen.getByTestId('reconnect-button'));

    await waitFor(() => expect(reconnect).toHaveBeenCalledTimes(1));
  });

  it('shows the busy label while reconnect is in flight', async () => {
    let resolve!: () => void;
    const reconnect = jest.fn(
      () => new Promise<void>(r => {
        resolve = r;
      }),
    );
    render(
      <RegistrationProvider value={{state: 'terminated', reconnect}}>
        <ReconnectButton />
      </RegistrationProvider>,
    );

    fireEvent.press(screen.getByTestId('reconnect-button'));

    // Label flips to the busy variant synchronously after onPress
    // (setState in the button's onPress handler).
    expect(screen.queryByText('Reconnecting…')).toBeTruthy();

    // Double-tap during busy is a no-op — predicate guarded.
    fireEvent.press(screen.getByTestId('reconnect-button'));
    expect(reconnect).toHaveBeenCalledTimes(1);

    resolve();
    await waitFor(() => expect(screen.queryByText('Reconnect')).toBeTruthy());
  });
});
