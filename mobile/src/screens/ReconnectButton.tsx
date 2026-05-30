/**
 * Manual reconnect — visible only when the UA is not registered.
 *
 * Web softphone's Dashboard has a Connect button (matches the
 * idempotent `SipService.connect()`); mobile mirrors it here so a
 * user whose network flapped + left the UA stuck in `terminated`
 * doesn't have to sign out + back in to recover.
 *
 * Pure consumer of `useRegistration()` — no UA refs leak into the
 * UI layer. `reconnect()` swallows errors internally; the badge
 * surfaces the result via the state stream.
 */
import React, {useState} from 'react';
import {Pressable, StyleSheet, Text} from 'react-native';
import {useRegistration} from '../state/registrationContext';

export function ReconnectButton(): React.JSX.Element | null {
  const {state, reconnect} = useRegistration();
  const [busy, setBusy] = useState(false);

  // Visible whenever we're NOT registered and NOT mid-handshake.
  // While 'starting'/'started'/'registering' the badge already tells
  // the user "Connecting…" — a Reconnect button there would be noise.
  const offlineStates = new Set(['unregistered', 'terminated', 'unknown']);
  if (!offlineStates.has(state)) return null;

  async function onPress(): Promise<void> {
    if (busy) return;
    setBusy(true);
    try {
      await reconnect();
    } finally {
      setBusy(false);
    }
  }

  return (
    <Pressable
      testID="reconnect-button"
      accessibilityRole="button"
      accessibilityLabel="Reconnect to the cloud"
      disabled={busy}
      onPress={onPress}
      style={({pressed}) => [
        styles.btn,
        (busy || pressed) && styles.btnDim,
      ]}>
      <Text style={styles.text}>{busy ? 'Reconnecting…' : 'Reconnect'}</Text>
    </Pressable>
  );
}

const styles = StyleSheet.create({
  btn: {
    paddingVertical: 4,
    paddingHorizontal: 10,
    borderRadius: 999,
    borderWidth: 1,
    borderColor: '#7fb0ef',
    marginLeft: 8,
  },
  btnDim: {opacity: 0.5},
  text: {color: '#7fb0ef', fontSize: 12, fontWeight: '600'},
});
