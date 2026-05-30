/**
 * Small UA-state pill rendered in the header of every post-login
 * screen. Mirrors the web softphone's Dashboard status line:
 *
 *   - `'registered'`              → green dot + "Online"
 *   - `'starting'`/`'started'`/   → amber dot + "Connecting…"
 *     `'registering'`
 *   - `'unregistered'`/`'terminated'`/`'unknown'`
 *                                 → red dot + "Offline"
 *
 * Defaults to grey + "Offline" when there's no RegistrationProvider
 * (isolated screen tests, app boot before the engine spins up).
 */
import React from 'react';
import {StyleSheet, Text, View} from 'react-native';
import {RegistrationState, useRegistration} from '../state/registrationContext';

interface Variant {
  label: string;
  dot: object;
}

function variantOf(state: RegistrationState): Variant {
  switch (state) {
    case 'registered':
      return {label: 'Online', dot: styles.online};
    case 'starting':
    case 'started':
    case 'registering':
      return {label: 'Connecting…', dot: styles.connecting};
    case 'unregistered':
    case 'terminated':
    case 'unknown':
      return {label: 'Offline', dot: styles.offline};
  }
}

export function RegistrationStatusBadge(): React.JSX.Element {
  const {state} = useRegistration();
  const {label, dot} = variantOf(state);
  return (
    <View
      style={styles.badge}
      testID="registration-badge"
      accessibilityRole="text"
      accessibilityLabel={`SIP registration: ${label}`}>
      <View style={[styles.dot, dot]} testID={`registration-dot-${state}`} />
      <Text style={styles.label} testID="registration-label">
        {label}
      </Text>
    </View>
  );
}

const styles = StyleSheet.create({
  badge: {
    flexDirection: 'row',
    alignItems: 'center',
    paddingVertical: 4,
    paddingHorizontal: 10,
    borderRadius: 999,
    backgroundColor: '#1a3258',
    alignSelf: 'flex-start',
  },
  dot: {width: 8, height: 8, borderRadius: 4, marginRight: 6},
  online: {backgroundColor: '#34c759'},
  connecting: {backgroundColor: '#ffcc00'},
  offline: {backgroundColor: '#d6342c'},
  label: {color: '#cfe0f5', fontSize: 12, fontWeight: '600'},
});
