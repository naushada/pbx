/**
 * Placeholder screen — TDD layer M0.
 *
 * Exists only so the app has something to render and the smoke test
 * has something to assert on. M1 deletes this and adds the real
 * Login / Create-account / Dial screens.
 */
import React from 'react';
import {SafeAreaView, Text, View, StyleSheet} from 'react-native';

export function PlaceholderScreen(): React.JSX.Element {
  return (
    <SafeAreaView style={styles.root} testID="app-root">
      <View style={styles.body}>
        <Text style={styles.title}>Society Softphone</Text>
        <Text style={styles.subtitle}>scaffold — M0</Text>
      </View>
    </SafeAreaView>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#0f2747'},
  body: {flex: 1, alignItems: 'center', justifyContent: 'center'},
  title: {color: '#ffffff', fontSize: 22, fontWeight: '700'},
  subtitle: {color: '#9bb3d1', fontSize: 13, marginTop: 6},
});
