/**
 * InCallPanel — TDD layer M2.d.
 *
 * The in-call UI: who you're talking to, a status line (a ringing
 * label while `calling`, an elapsed mm:ss timer once `connected`), and
 * mute / hang-up controls. All actions go through the `CallController`.
 */
import React, {useEffect, useState} from 'react';
import {Pressable, SafeAreaView, StyleSheet, Text, View} from 'react-native';
import {Call} from '../sip/callState';
import {CallController} from '../call/callController';

interface Props {
  call: Call;
  controller: CallController;
}

function mmss(total: number): string {
  const m = Math.floor(total / 60);
  const s = total % 60;
  return `${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}`;
}

/** `sip:<flat>@<realm>` → `<flat>` for display. */
function flatOf(target: string | null): string {
  if (!target) return '';
  const m = /^sip:([^@]+)@/.exec(target);
  return m ? m[1] : target;
}

export function InCallPanel({call, controller}: Props): React.JSX.Element {
  const [seconds, setSeconds] = useState(0);
  const [muted, setMuted] = useState(controller.isMuted());

  useEffect(() => {
    if (call.state !== 'connected') return undefined;
    const id = setInterval(() => setSeconds(s => s + 1), 1000);
    return () => clearInterval(id);
  }, [call.state]);

  function toggleMute(): void {
    const next = !muted;
    setMuted(next);
    controller.setMuted(next);
  }

  return (
    <SafeAreaView style={styles.root} testID="incall-panel">
      <View style={styles.body}>
        <Text style={styles.target} testID="incall-target">
          {flatOf(call.target)}
        </Text>
        <Text style={styles.status} testID="incall-status">
          {call.state === 'connected' ? mmss(seconds) : 'Calling…'}
        </Text>
      </View>

      <View style={styles.actions}>
        <Pressable
          testID="incall-mute"
          accessibilityRole="button"
          onPress={toggleMute}
          style={[styles.btn, muted && styles.btnActive]}>
          <Text style={styles.btnText}>{muted ? 'Unmute' : 'Mute'}</Text>
        </Pressable>
        <Pressable
          testID="incall-hangup"
          accessibilityRole="button"
          onPress={() => controller.hangup()}
          style={[styles.btn, styles.hangup]}>
          <Text style={styles.btnText}>Hang up</Text>
        </Pressable>
      </View>
    </SafeAreaView>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#0f2747'},
  body: {flex: 1, alignItems: 'center', justifyContent: 'center'},
  target: {color: '#ffffff', fontSize: 30, fontWeight: '700'},
  status: {color: '#9bb3d1', fontSize: 16, marginTop: 8},
  actions: {
    flexDirection: 'row',
    justifyContent: 'center',
    paddingBottom: 40,
    gap: 16,
  },
  btn: {
    backgroundColor: '#1d3f6e',
    borderRadius: 8,
    paddingVertical: 14,
    paddingHorizontal: 28,
  },
  btnActive: {backgroundColor: '#2f6fd0'},
  hangup: {backgroundColor: '#c0392b'},
  btnText: {color: '#ffffff', fontSize: 15, fontWeight: '700'},
});
