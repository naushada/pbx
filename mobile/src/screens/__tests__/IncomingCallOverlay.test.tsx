/**
 * IncomingCallOverlay tests — renders a ringing modal that drives the
 * M3.b IncomingCallController.
 */
import React from 'react';
import {fireEvent, render, screen} from '@testing-library/react-native';
import {IncomingCallOverlay} from '../IncomingCallOverlay';
import {IncomingCallController} from '../../call/incomingCallController';

function buildController(): IncomingCallController {
  // Build a real IncomingCallController with jest.fn deps — exposes
  // subscribe / accept / decline + drives the ring reducer.
  const callKit = {displayIncomingCall: jest.fn(), endCall: jest.fn()};
  const signaling = {
    answer: jest.fn(async () => {}),
    reject: jest.fn(),
  };
  return new IncomingCallController({
    callKit,
    signaling,
    ensureConnected: jest.fn(async () => {}),
    connectMedia: jest.fn(async () => {}),
    logMissedCall: jest.fn(),
  });
}

const validPush = {
  type: 'incoming-call',
  callId: 'call-7',
  callerFlat: 'B-204',
  callerName: 'Asha Rao',
};

describe('IncomingCallOverlay', () => {
  it('renders nothing visible while no call is ringing', () => {
    const controller = buildController();

    render(<IncomingCallOverlay controller={controller} />);

    // The Modal's visible=false hides everything; the caller name must
    // not be queryable.
    expect(screen.queryByText('Asha Rao')).toBeNull();
  });

  it('shows the caller identity once the controller reports a ring', () => {
    const controller = buildController();
    render(<IncomingCallOverlay controller={controller} />);

    controller.reportPush(validPush);

    expect(screen.getByText('Asha Rao')).toBeTruthy();
    expect(screen.getByText('Flat B-204')).toBeTruthy();
    expect(screen.getByText('Incoming call')).toBeTruthy();
  });

  it('tapping Accept asks the controller to accept', async () => {
    const controller = buildController();
    const acceptSpy = jest.spyOn(controller, 'accept');
    render(<IncomingCallOverlay controller={controller} />);
    controller.reportPush(validPush);

    fireEvent.press(screen.getByLabelText('Accept call'));

    expect(acceptSpy).toHaveBeenCalledTimes(1);
  });

  it('tapping Decline asks the controller to decline', () => {
    const controller = buildController();
    const declineSpy = jest.spyOn(controller, 'decline');
    render(<IncomingCallOverlay controller={controller} />);
    controller.reportPush(validPush);

    fireEvent.press(screen.getByLabelText('Decline call'));

    expect(declineSpy).toHaveBeenCalledTimes(1);
  });

  it('hides itself when the controller leaves ringing (accept / decline / timeout)', () => {
    const controller = buildController();
    render(<IncomingCallOverlay controller={controller} />);
    controller.reportPush(validPush);
    expect(screen.queryByText('Asha Rao')).toBeTruthy();

    controller.decline();

    expect(screen.queryByText('Asha Rao')).toBeNull();
  });

  describe('alerter', () => {
    const makeAlerter = () => ({start: jest.fn(), stop: jest.fn()});

    it('does not pulse before any call rings', () => {
      const controller = buildController();
      const alerter = makeAlerter();

      render(<IncomingCallOverlay controller={controller} alerter={alerter} />);

      expect(alerter.start).not.toHaveBeenCalled();
      expect(alerter.stop).not.toHaveBeenCalled();
    });

    it('starts the alerter when the controller transitions to ringing', () => {
      const controller = buildController();
      const alerter = makeAlerter();
      render(<IncomingCallOverlay controller={controller} alerter={alerter} />);

      controller.reportPush(validPush);

      expect(alerter.start).toHaveBeenCalledTimes(1);
      expect(alerter.stop).not.toHaveBeenCalled();
    });

    it('stops the alerter when the user declines', () => {
      const controller = buildController();
      const alerter = makeAlerter();
      render(<IncomingCallOverlay controller={controller} alerter={alerter} />);
      controller.reportPush(validPush);
      expect(alerter.start).toHaveBeenCalledTimes(1);

      controller.decline();

      expect(alerter.stop).toHaveBeenCalledTimes(1);
    });

    it('stops the alerter on unmount even while still ringing', () => {
      const controller = buildController();
      const alerter = makeAlerter();
      const {unmount} = render(
        <IncomingCallOverlay controller={controller} alerter={alerter} />,
      );
      controller.reportPush(validPush);
      expect(alerter.start).toHaveBeenCalledTimes(1);

      unmount();

      expect(alerter.stop).toHaveBeenCalledTimes(1);
    });
  });
});
