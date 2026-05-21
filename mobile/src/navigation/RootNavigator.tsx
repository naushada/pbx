/**
 * Root navigator — TDD layer M1.c.
 *
 * The React Navigation native-stack: Login → Register → Dial. This is
 * thin wiring — the screens are unit-tested in isolation; the navigator
 * itself is exercised by Detox (TDD layer M4).
 */
import React from 'react';
import {NavigationContainer} from '@react-navigation/native';
import {createNativeStackNavigator} from '@react-navigation/native-stack';
import type {RootStackParamList} from './types';
import {LoginScreen} from '../screens/LoginScreen';
import {RegisterScreen} from '../screens/RegisterScreen';
import {DialScreen} from '../screens/DialScreen';

const Stack = createNativeStackNavigator<RootStackParamList>();

export function RootNavigator(): React.JSX.Element {
  return (
    <NavigationContainer>
      <Stack.Navigator
        initialRouteName="Login"
        screenOptions={{headerShown: false}}>
        <Stack.Screen name="Login" component={LoginScreen} />
        <Stack.Screen name="Register" component={RegisterScreen} />
        <Stack.Screen name="Dial" component={DialScreen} />
      </Stack.Navigator>
    </NavigationContainer>
  );
}
