/**
 * DialScreen — TDD layer M2.d.
 *
 * The dialer: shows who is signed in, takes a destination flat number,
 * and places the call through the `CallController`. While a call is
 * active it hands the screen over to `InCallPanel`.
 */
import React, {useState} from 'react';
import {Pressable, ScrollView, StyleSheet, Text, View} from 'react-native';
import type {NativeStackScreenProps} from '@react-navigation/native-stack';
import type {RootStackParamList} from '../navigation/types';
import {FormField} from './FormField';
import {InCallPanel} from './InCallPanel';
import {useDeps} from '../state/deps';
import {useAuth} from '../state/authContext';
import {useCall} from '../call/useCall';
import {isCallActive} from '../sip/callState';
import {isDialableFlat} from '../sip/sipUri';

type Props = NativeStackScreenProps<RootStackParamList, 'Dial'>;

export function DialScreen({navigation, route}: Props): React.JSX.Element {
  const {callController, sessionStore} = useDeps();
  const {setSession} = useAuth();
  const {subscriber} = route.params.session;
  const call = useCall(callController);
  const [flat, setFlat] = useState('');

  // A call in flight takes over the whole screen.
  if (isCallActive(call)) {
    return <InCallPanel call={call} controller={callController} />;
  }

  async function onSignOut(): Promise<void> {
    // Wipe the keychain first; if it throws we still flip the in-memory
    // session so the user gets out of the authed UI. App.tsx's
    // `useEffect([session])` tears down the sip.js UA when session
    // becomes null — no UA leaks across the logout.
    try {
      await sessionStore.clear();
    } catch {
      /* swallow — best-effort wipe */
    }
    setSession(null);
    navigation.reset({index: 0, routes: [{name: 'Login'}]});
  }

  const canDial = isDialableFlat(flat);
  return (
    <ScrollView
      style={styles.root}
      contentContainerStyle={styles.content}
      testID="dial-screen"
      keyboardShouldPersistTaps="handled">
      <View style={styles.identity} testID="dial-identity">
        <View style={styles.identityText}>
          <Text style={styles.flat}>{subscriber.flatNumber}</Text>
          <Text style={styles.name}>{subscriber.displayName}</Text>
        </View>
        <Pressable
          testID="dial-signout"
          accessibilityRole="button"
          accessibilityLabel="Sign out"
          onPress={onSignOut}
          style={({pressed}) => [styles.signOut, pressed && styles.signOutDim]}>
          <Text style={styles.signOutText}>Sign out</Text>
        </Pressable>
      </View>

      <Text style={styles.heading}>Call a flat</Text>
      <FormField
        label="Destination flat"
        testID="dial-flat"
        value={flat}
        onChangeText={setFlat}
        placeholder="e.g. A-101"
        autoCapitalize="characters"
      />

      <Pressable
        testID="dial-call"
        accessibilityRole="button"
        disabled={!canDial}
        onPress={() => {
          // Guard in addition to `disabled` so the call is never placed
          // with an invalid flat, however the press is delivered.
          if (canDial) callController.placeCall(flat.trim());
        }}
        style={({pressed}) => [
          styles.call,
          (!canDial || pressed) && styles.callDim,
        ]}>
        <Text style={styles.callText}>Call</Text>
      </Pressable>

      <View style={styles.shortcuts}>
        <Pressable
          testID="dial-directory"
          accessibilityRole="button"
          accessibilityLabel="Open directory"
          onPress={() =>
            navigation.navigate('Directory', {session: route.params.session})
          }
          style={({pressed}) => [
            styles.shortcut,
            pressed && styles.shortcutDim,
          ]}>
          <Text style={styles.shortcutText}>Directory</Text>
        </Pressable>
        <Pressable
          testID="dial-history"
          accessibilityRole="button"
          accessibilityLabel="Open call history"
          onPress={() =>
            navigation.navigate('History', {session: route.params.session})
          }
          style={({pressed}) => [
            styles.shortcut,
            pressed && styles.shortcutDim,
          ]}>
          <Text style={styles.shortcutText}>History</Text>
        </Pressable>
      </View>

      {call.state === 'ended' && call.endReason ? (
        <Text testID="dial-last-call" style={styles.ended}>
          Last call ended: {call.endReason}
        </Text>
      ) : null}
    </ScrollView>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#0f2747'},
  content: {padding: 24, paddingTop: 56},
  identity: {
    marginBottom: 32,
    flexDirection: 'row',
    alignItems: 'flex-start',
    justifyContent: 'space-between',
  },
  identityText: {flex: 1, marginRight: 12},
  flat: {color: '#ffffff', fontSize: 30, fontWeight: '700'},
  name: {color: '#cfe0f5', fontSize: 15, marginTop: 2},
  signOut: {
    paddingVertical: 6,
    paddingHorizontal: 10,
    borderRadius: 6,
    borderWidth: 1,
    borderColor: '#3a567c',
  },
  signOutDim: {opacity: 0.5},
  signOutText: {color: '#9bb3d1', fontSize: 13, fontWeight: '600'},
  heading: {color: '#9bb3d1', fontSize: 14, fontWeight: '600', marginBottom: 8},
  call: {
    backgroundColor: '#2f7d4f',
    borderRadius: 8,
    paddingVertical: 14,
    alignItems: 'center',
    marginTop: 4,
  },
  callDim: {opacity: 0.5},
  callText: {color: '#ffffff', fontSize: 16, fontWeight: '700'},
  shortcuts: {flexDirection: 'row', gap: 12, marginTop: 12},
  shortcut: {
    flex: 1,
    borderRadius: 8,
    paddingVertical: 12,
    alignItems: 'center',
    borderWidth: 1,
    borderColor: '#3a567c',
  },
  shortcutDim: {opacity: 0.5},
  shortcutText: {color: '#9bb3d1', fontSize: 14, fontWeight: '600'},
  ended: {color: '#7f97b5', fontSize: 13, marginTop: 18, textAlign: 'center'},
});
