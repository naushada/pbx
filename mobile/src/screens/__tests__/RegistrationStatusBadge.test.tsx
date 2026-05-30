/**
 * RegistrationStatusBadge tests — pure variant mapping.
 *
 * The badge has three visual states (Online / Connecting… / Offline)
 * keyed off the RegistrationContext value. Each `SipUaState` plus the
 * `'unknown'` sentinel is exercised here so a future SipUaState
 * addition surfaces a compile + test failure together.
 */
import React from 'react';
import {render, screen} from '@testing-library/react-native';
import {RegistrationStatusBadge} from '../RegistrationStatusBadge';
import {
  RegistrationProvider,
  RegistrationState,
} from '../../state/registrationContext';

function renderWith(state: RegistrationState) {
  return render(
    <RegistrationProvider value={state}>
      <RegistrationStatusBadge />
    </RegistrationProvider>,
  );
}

describe('RegistrationStatusBadge', () => {
  it("renders 'Online' when the UA is registered", () => {
    renderWith('registered');

    expect(screen.getByTestId('registration-label').props.children).toBe(
      'Online',
    );
    expect(screen.getByTestId('registration-dot-registered')).toBeTruthy();
  });

  it.each(['starting', 'started', 'registering'] as RegistrationState[])(
    "renders 'Connecting…' for the in-flight state %s",
    state => {
      renderWith(state);
      expect(screen.getByTestId('registration-label').props.children).toBe(
        'Connecting…',
      );
    },
  );

  it.each(['unregistered', 'terminated', 'unknown'] as RegistrationState[])(
    "renders 'Offline' for the inactive state %s",
    state => {
      renderWith(state);
      expect(screen.getByTestId('registration-label').props.children).toBe(
        'Offline',
      );
    },
  );

  it('falls back to Offline when there is no RegistrationProvider', () => {
    // Context default is 'unknown' which maps to Offline. Important so
    // an isolated screen test that doesn't wrap doesn't crash on a
    // missing provider.
    render(<RegistrationStatusBadge />);

    expect(screen.getByTestId('registration-label').props.children).toBe(
      'Offline',
    );
  });

  it("exposes a screen-reader label describing the current state", () => {
    renderWith('registered');

    const badge = screen.getByTestId('registration-badge');
    expect(badge.props.accessibilityLabel).toBe('SIP registration: Online');
  });
});
