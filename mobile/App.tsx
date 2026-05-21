/**
 * App root.
 *
 * Thin shell: a safe-area provider, the status bar, and the navigator.
 * The screens' deps (cloud client, session store) come from the
 * default `DepsContext` value — no provider needed in production.
 */
import React from 'react';
import {StatusBar} from 'react-native';
import {SafeAreaProvider} from 'react-native-safe-area-context';
import {RootNavigator} from './src/navigation/RootNavigator';

export default function App(): React.JSX.Element {
  return (
    <SafeAreaProvider>
      <StatusBar barStyle="light-content" />
      <RootNavigator />
    </SafeAreaProvider>
  );
}
