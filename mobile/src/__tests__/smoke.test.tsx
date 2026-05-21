/**
 * TDD layer M0.1 — smoke test.
 *
 * The first test of the project: it proves the Jest + Testing Library
 * pipeline renders the app at all. Everything else builds on a green
 * run here.
 */
import React from 'react';
import {render, screen} from '@testing-library/react-native';
import App from '../../App';

describe('App — M0 smoke', () => {
  it('renders the app root without crashing', () => {
    render(<App />);
    expect(screen.getByTestId('app-root')).toBeTruthy();
  });

  it('shows the app name', () => {
    render(<App />);
    expect(screen.getByText('Society Softphone')).toBeTruthy();
  });
});
