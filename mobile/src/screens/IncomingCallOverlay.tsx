/**
 * IncomingCallOverlay — foreground ring UI.
 *
 * Subscribes to `IncomingCallController` and renders a full-screen
 * modal with caller identity + Accept / Decline buttons whenever the
 * controller is in `'ringing'` state. Mounted at the App root so it
 * paints over whatever screen is active.
 *
 * The real CallKit / PushKit / ConnectionService UI lands in a
 * follow-up; until then this overlay is the in-app fallback that
 * makes foreground-to-foreground inbound calls actually usable.
 */
import React, {useEffect, useState} from 'react';
import {Modal, StyleSheet, Text, TouchableOpacity, View} from 'react-native';
import {IncomingCallController} from '../call/incomingCallController';
import {IncomingCall} from '../sip/incomingCall';

interface Props {
  controller: IncomingCallController;
}

export function IncomingCallOverlay({controller}: Props): React.JSX.Element {
  const [call, setCall] = useState<IncomingCall | null>(controller.getCall());

  useEffect(() => controller.subscribe(setCall), [controller]);

  const ringing = call !== null && call.state === 'ringing';

  return (
    <Modal
      animationType="fade"
      transparent
      visible={ringing}
      onRequestClose={() => controller.decline()}>
      <View style={styles.backdrop}>
        {ringing && (
          <View style={styles.card}>
            <Text style={styles.label}>Incoming call</Text>
            <Text style={styles.name}>{call!.callerName}</Text>
            <Text style={styles.flat}>Flat {call!.callerFlat}</Text>

            <View style={styles.row}>
              <TouchableOpacity
                accessibilityRole="button"
                accessibilityLabel="Decline call"
                style={[styles.btn, styles.decline]}
                onPress={() => controller.decline()}>
                <Text style={styles.btnText}>Decline</Text>
              </TouchableOpacity>

              <TouchableOpacity
                accessibilityRole="button"
                accessibilityLabel="Accept call"
                style={[styles.btn, styles.accept]}
                onPress={() => {
                  controller.accept().catch(() => {
                    /* the controller logs / state-transitions on its own */
                  });
                }}>
                <Text style={styles.btnText}>Accept</Text>
              </TouchableOpacity>
            </View>
          </View>
        )}
      </View>
    </Modal>
  );
}

const styles = StyleSheet.create({
  backdrop: {
    flex: 1,
    backgroundColor: 'rgba(0,0,0,0.7)',
    justifyContent: 'center',
    alignItems: 'center',
  },
  card: {
    width: '85%',
    padding: 32,
    borderRadius: 16,
    backgroundColor: '#1c1c1e',
    alignItems: 'center',
  },
  label: {color: '#9a9a9c', fontSize: 14, marginBottom: 8},
  name: {color: '#fff', fontSize: 26, fontWeight: '600'},
  flat: {color: '#9a9a9c', fontSize: 16, marginTop: 4, marginBottom: 32},
  row: {flexDirection: 'row', gap: 16},
  btn: {paddingVertical: 16, paddingHorizontal: 28, borderRadius: 999},
  decline: {backgroundColor: '#d6342c'},
  accept: {backgroundColor: '#34c759'},
  btnText: {color: '#fff', fontSize: 16, fontWeight: '600'},
});
