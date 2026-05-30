/**
 * DirectoryScreen tests — TDD layer M2.d follow-up.
 *
 * Society directory + tap-to-call (parity with the web softphone's
 * `DirectoryComponent`). Rendered in isolation with:
 *
 *   - a fake `CloudClient` whose `getDirectory` is scripted,
 *   - a `FakeCallController` to assert `placeCall` is invoked,
 *   - a mocked `navigation.goBack` to verify the back link.
 */
import React from 'react';
import {
  act,
  fireEvent,
  render,
  screen,
  waitFor,
} from '@testing-library/react-native';
import {DirectoryScreen} from '../DirectoryScreen';
import {DepsProvider} from '../../state/deps';
import {FakeCallController} from '../../test/fakeCallController';
import {callReducer, idleCall} from '../../sip/callState';
import {DirectoryEntry, Session} from '../../api/types';
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

const TWO_OTHERS: DirectoryEntry[] = [
  {
    flatNumber: 'B-204',
    displayName: 'Bob',
    sipUri: 'sip:B-204@pbx.local',
    online: true,
  },
  {
    flatNumber: 'C-301',
    displayName: 'Chitra',
    sipUri: 'sip:C-301@pbx.local',
    online: false,
  },
];

interface RenderOpts {
  rows?: DirectoryEntry[];
  rejectWith?: unknown;
}

function renderDirectory(opts: RenderOpts = {}) {
  const getDirectory = jest.fn().mockImplementation(() => {
    if (opts.rejectWith) return Promise.reject(opts.rejectWith);
    // Default includes the current user's own flat — DirectoryScreen
    // should filter it out, mirroring web's directory.component.
    return Promise.resolve(
      opts.rows ?? [
        {
          flatNumber: 'A-101',
          displayName: 'Resident A101',
          sipUri: 'sip:A-101@pbx.local',
          online: true,
        },
        ...TWO_OTHERS,
      ],
    );
  });
  const client = {getDirectory} as unknown as CloudClient;
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
      <DirectoryScreen
        navigation={navigation as never}
        route={
          {
            key: 'd',
            name: 'Directory',
            params: {session: SESSION},
          } as never
        }
      />
    </DepsProvider>,
  );
  return {client, getDirectory, callController, goBack};
}

describe('DirectoryScreen', () => {
  it('fetches the directory for the signed-in society on mount', async () => {
    const {getDirectory} = renderDirectory();
    await waitFor(() => expect(getDirectory).toHaveBeenCalledTimes(1));
    expect(getDirectory).toHaveBeenCalledWith('soc_sunset');
  });

  it('renders rows for every entry returned by the cloud (minus self)', async () => {
    renderDirectory();

    // Self ('A-101' / 'Resident A101') must be filtered out.
    await waitFor(() => expect(screen.queryByText('Bob')).toBeTruthy());
    expect(screen.queryByText('Chitra')).toBeTruthy();
    expect(screen.queryByText('Resident A101')).toBeNull();
  });

  it('shows an online dot for online entries and disables Call for offline', async () => {
    renderDirectory({rows: TWO_OTHERS});

    await waitFor(() => screen.getByTestId('directory-call-B-204'));

    // Both rows render. The online one (Bob) is enabled; the offline
    // one (Chitra) is disabled.
    expect(screen.getByTestId('directory-call-B-204').props.accessibilityState)
      .toMatchObject({disabled: false});
    expect(screen.getByTestId('directory-call-C-301').props.accessibilityState)
      .toMatchObject({disabled: true});
  });

  it('tapping Call dispatches placeCall(flatNumber) on the controller', async () => {
    const {callController} = renderDirectory({rows: TWO_OTHERS});
    await waitFor(() => screen.getByTestId('directory-call-B-204'));

    fireEvent.press(screen.getByTestId('directory-call-B-204'));

    expect(callController.placeCall).toHaveBeenCalledWith('B-204');
  });

  it('filters by flat prefix client-side', async () => {
    renderDirectory({rows: TWO_OTHERS});
    await waitFor(() => screen.getByTestId('directory-list'));

    fireEvent.changeText(screen.getByTestId('directory-filter'), 'B');

    expect(screen.queryByText('Bob')).toBeTruthy();
    expect(screen.queryByText('Chitra')).toBeNull();
  });

  it('hands the screen over to the in-call panel while a call is active', async () => {
    const {callController} = renderDirectory({rows: TWO_OTHERS});
    await waitFor(() => screen.getByTestId('directory-screen'));

    act(() => {
      callController.emit(
        callReducer(idleCall, {type: 'dial', target: 'sip:B-204@pbx.local'}),
      );
    });

    expect(screen.queryByTestId('directory-screen')).toBeNull();
    expect(screen.getByTestId('incall-panel')).toBeTruthy();
  });

  it('back link goes back to the dialer', async () => {
    const {goBack} = renderDirectory({rows: TWO_OTHERS});
    await waitFor(() => screen.getByTestId('directory-back'));

    fireEvent.press(screen.getByTestId('directory-back'));

    expect(goBack).toHaveBeenCalledTimes(1);
  });

  it('shows an error message if getDirectory rejects', async () => {
    renderDirectory({rejectWith: new Error('network down')});

    await waitFor(() => expect(screen.queryByText(/network down/)).toBeTruthy());
  });

  it('shows an empty-state when the society has no other flats', async () => {
    renderDirectory({rows: []});

    await waitFor(() =>
      expect(
        screen.queryByText(/no other flats in this society yet/i),
      ).toBeTruthy(),
    );
  });
});
