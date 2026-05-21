/**
 * DialScreen — TDD layer M1.c placeholder.
 *
 * After login/registration the app lands here. For M1 it just confirms
 * who is signed in; TDD layer M2 turns it into the real dialer (enter a
 * destination flat number, press Call).
 */
import React from 'react';
import {SafeAreaView, StyleSheet, Text, View} from 'react-native';
import type {NativeStackScreenProps} from '@react-navigation/native-stack';
import type {RootStackParamList} from '../navigation/types';

type Props = NativeStackScreenProps<RootStackParamList, 'Dial'>;

export function DialScreen({route}: Props): React.JSX.Element {
  const {subscriber} = route.params.session;
  return (
    <SafeAreaView style={styles.root} testID="dial-screen">
      <View style={styles.body}>
        <Text style={styles.flat}>{subscriber.flatNumber}</Text>
        <Text style={styles.name}>{subscriber.displayName}</Text>
        <Text style={styles.note}>
          Signed in. The dialer — enter a destination flat and Call —
          arrives in TDD layer M2.
        </Text>
      </View>
    </SafeAreaView>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#0f2747'},
  body: {flex: 1, alignItems: 'center', justifyContent: 'center', padding: 24},
  flat: {color: '#ffffff', fontSize: 32, fontWeight: '700'},
  name: {color: '#cfe0f5', fontSize: 16, marginTop: 4},
  note: {color: '#7f97b5', fontSize: 13, marginTop: 20, textAlign: 'center'},
});
