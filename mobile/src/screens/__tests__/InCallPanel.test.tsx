/**
 * TDD layer M2.d — InCallPanel.
 */
import React from 'react';
import {fireEvent, render, screen} from '@testing-library/react-native';
import {InCallPanel} from '../InCallPanel';
import {FakeCallController} from '../../test/fakeCallController';
import {Call} from '../../sip/callState';

const calling: Call = {
  state: 'calling',
  target: 'sip:B-204@pbx.local',
  endReason: null,
};
const connected: Call = {
  state: 'connected',
  target: 'sip:B-204@pbx.local',
  endReason: null,
};

describe('InCallPanel', () => {
  it('renders the target, status, mute and hang-up controls', () => {
    render(<InCallPanel call={calling} controller={new FakeCallController()} />);
    expect(screen.getByTestId('incall-target')).toBeTruthy();
    expect(screen.getByTestId('incall-status')).toBeTruthy();
    expect(screen.getByTestId('incall-mute')).toBeTruthy();
    expect(screen.getByTestId('incall-hangup')).toBeTruthy();
  });

  it('shows the destination flat extracted from the SIP URI', () => {
    render(<InCallPanel call={calling} controller={new FakeCallController()} />);
    expect(screen.getByText('B-204')).toBeTruthy();
  });

  it('shows a ringing label while calling and a timer once connected', () => {
    const {rerender} = render(
      <InCallPanel call={calling} controller={new FakeCallController()} />,
    );
    expect(screen.getByText(/calling/i)).toBeTruthy();

    rerender(
      <InCallPanel call={connected} controller={new FakeCallController()} />,
    );
    expect(screen.getByText('00:00')).toBeTruthy();
  });

  it('toggles mute through the controller', () => {
    const controller = new FakeCallController();
    render(<InCallPanel call={connected} controller={controller} />);
    fireEvent.press(screen.getByTestId('incall-mute'));
    expect(controller.setMuted).toHaveBeenCalledWith(true);
    expect(screen.getByText('Unmute')).toBeTruthy();
  });

  it('hangs up through the controller', () => {
    const controller = new FakeCallController();
    render(<InCallPanel call={connected} controller={controller} />);
    fireEvent.press(screen.getByTestId('incall-hangup'));
    expect(controller.hangup).toHaveBeenCalledTimes(1);
  });
});
