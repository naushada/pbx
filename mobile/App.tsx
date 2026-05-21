/**
 * App root.
 *
 * TDD layer M0: this is the minimal root that makes the smoke test
 * (`src/__tests__/smoke.test.tsx`) pass — it renders `RootNavigator`,
 * nothing more.
 *
 * M1 wraps this in `SafeAreaProvider` and turns `RootNavigator` into a
 * real React Navigation stack (Login / Create-account / Dial). See
 * docs/design/mobile-app-tdd.md.
 */
import React from 'react';
import {RootNavigator} from './src/navigation/RootNavigator';

export default function App(): React.JSX.Element {
  return <RootNavigator />;
}
