/**
 * LoginScreen — TDD layer M1.c.
 *
 * Society / Flat / Password → `CloudClient.login`. On success the
 * session is persisted and the app routes to Dial; on failure the
 * typed `ApiError`'s message is shown inline.
 */
import React, {useState} from 'react';
import {
  ActivityIndicator,
  Pressable,
  ScrollView,
  StyleSheet,
  Text,
} from 'react-native';
import type {NativeStackScreenProps} from '@react-navigation/native-stack';
import type {RootStackParamList} from '../navigation/types';
import {FormField} from './FormField';
import {FieldErrors, validateLoginForm} from '../validation/forms';
import {ApiError} from '../api/types';
import {useDeps} from '../state/deps';

type Props = NativeStackScreenProps<RootStackParamList, 'Login'>;

export function LoginScreen({navigation}: Props): React.JSX.Element {
  const {client, sessionStore} = useDeps();
  const [society, setSociety] = useState('');
  const [flat, setFlat] = useState('');
  const [password, setPassword] = useState('');
  const [errors, setErrors] = useState<FieldErrors>({});
  const [serverError, setServerError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  async function onSubmit(): Promise<void> {
    setServerError(null);
    const result = validateLoginForm({society, flat, password});
    if (!result.ok) {
      setErrors(result.errors);
      return;
    }
    setErrors({});
    setBusy(true);
    try {
      const session = await client.login(society.trim(), flat.trim(), password);
      await sessionStore.save(session);
      navigation.replace('Dial', {session});
    } catch (e) {
      setServerError(
        e instanceof ApiError
          ? e.userMessage
          : 'Something went wrong. Please try again.',
      );
    } finally {
      setBusy(false);
    }
  }

  return (
    <ScrollView
      style={styles.root}
      contentContainerStyle={styles.content}
      testID="login-screen"
      keyboardShouldPersistTaps="handled">
      <Text style={styles.brand}>Society Softphone</Text>
      <Text style={styles.heading}>Log in</Text>

      <FormField
        label="Society"
        testID="login-society"
        value={society}
        onChangeText={setSociety}
        error={errors.society}
        autoCapitalize="characters"
        placeholder="e.g. SUNSET"
      />
      <FormField
        label="Flat number"
        testID="login-flat"
        value={flat}
        onChangeText={setFlat}
        error={errors.flat}
        placeholder="e.g. A-101"
      />
      <FormField
        label="Password"
        testID="login-password"
        value={password}
        onChangeText={setPassword}
        error={errors.password}
        secureTextEntry
      />

      {serverError ? (
        <Text testID="login-error" style={styles.serverError}>
          {serverError}
        </Text>
      ) : null}

      <Pressable
        testID="login-submit"
        accessibilityRole="button"
        disabled={busy}
        onPress={onSubmit}
        style={({pressed}) => [
          styles.button,
          (busy || pressed) && styles.buttonDim,
        ]}>
        {busy ? (
          <ActivityIndicator color="#ffffff" />
        ) : (
          <Text style={styles.buttonText}>Log in</Text>
        )}
      </Pressable>

      <Pressable
        testID="login-to-register"
        accessibilityRole="link"
        onPress={() => navigation.navigate('Register')}>
        <Text style={styles.link}>New here?  Create an account</Text>
      </Pressable>
    </ScrollView>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#0f2747'},
  content: {padding: 24, paddingTop: 64},
  brand: {color: '#9bb3d1', fontSize: 14, fontWeight: '600'},
  heading: {
    color: '#ffffff',
    fontSize: 26,
    fontWeight: '700',
    marginTop: 4,
    marginBottom: 24,
  },
  serverError: {
    color: '#f1889a',
    fontSize: 13,
    marginBottom: 12,
  },
  button: {
    backgroundColor: '#2f6fd0',
    borderRadius: 8,
    paddingVertical: 14,
    alignItems: 'center',
    marginTop: 4,
  },
  buttonDim: {opacity: 0.6},
  buttonText: {color: '#ffffff', fontSize: 16, fontWeight: '700'},
  link: {
    color: '#7fb0ef',
    fontSize: 14,
    textAlign: 'center',
    marginTop: 20,
  },
});
