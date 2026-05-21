/**
 * Root navigator.
 *
 * TDD layer M0: a placeholder root component — enough for the smoke
 * test to prove the Jest + Testing Library pipeline renders the app.
 *
 * M1 replaces the body with a React Navigation native-stack:
 *   Login → Create account → Dial
 * (`@react-navigation/native` + `@react-navigation/native-stack`,
 * already declared in package.json).
 */
import React from 'react';
import {PlaceholderScreen} from '../screens/PlaceholderScreen';

export function RootNavigator(): React.JSX.Element {
  return <PlaceholderScreen />;
}
