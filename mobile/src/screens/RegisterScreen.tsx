/**
 * RegisterScreen — TDD layer M1.c.
 *
 * Self-registration: Society / Flat / Resident name / Password
 * (required) + Mobile / Email (optional) → `CloudClient.register`.
 * Registration is ungated, so a success returns a session and the app
 * auto-logs-in straight to Dial (no separate login step).
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
import {FieldErrors, validateRegisterForm} from '../validation/forms';
import {ApiError} from '../api/types';
import {useDeps} from '../state/deps';
import {useAuth} from '../state/authContext';

type Props = NativeStackScreenProps<RootStackParamList, 'Register'>;

export function RegisterScreen({navigation}: Props): React.JSX.Element {
  const {client, sessionStore} = useDeps();
  const {setSession} = useAuth();
  const [society, setSociety] = useState('');
  const [flat, setFlat] = useState('');
  const [residentName, setResidentName] = useState('');
  const [mobile, setMobile] = useState('');
  const [email, setEmail] = useState('');
  const [password, setPassword] = useState('');
  const [errors, setErrors] = useState<FieldErrors>({});
  const [serverError, setServerError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  async function onSubmit(): Promise<void> {
    setServerError(null);
    const result = validateRegisterForm({
      society,
      flat,
      residentName,
      password,
      mobile,
      email,
    });
    if (!result.ok) {
      setErrors(result.errors);
      return;
    }
    setErrors({});
    setBusy(true);
    try {
      const session = await client.register({
        societyName: society.trim(),
        flatNumber: flat.trim(),
        residentName: residentName.trim(),
        password,
        mobile,
        email,
      });
      // Ungated registration — the response is a live session.
      await sessionStore.save(session);
      // Signal the App shell so it builds the sip.js call engine
      // before DialScreen mounts (default useAuth() is a no-op so
      // existing isolated screen tests don't need to wrap).
      setSession(session);
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
      testID="register-screen"
      keyboardShouldPersistTaps="handled">
      <Text style={styles.heading}>Create account</Text>

      <FormField
        label="Society"
        testID="register-society"
        value={society}
        onChangeText={setSociety}
        error={errors.society}
        autoCapitalize="characters"
        placeholder="e.g. SUNSET"
      />
      <FormField
        label="Flat number"
        testID="register-flat"
        value={flat}
        onChangeText={setFlat}
        error={errors.flat}
        placeholder="e.g. A-101"
      />
      <FormField
        label="Resident name"
        testID="register-name"
        value={residentName}
        onChangeText={setResidentName}
        error={errors.residentName}
        autoCapitalize="words"
      />
      <FormField
        label="Mobile number"
        testID="register-mobile"
        value={mobile}
        onChangeText={setMobile}
        optional
        keyboardType="phone-pad"
      />
      <FormField
        label="Email"
        testID="register-email"
        value={email}
        onChangeText={setEmail}
        error={errors.email}
        optional
        keyboardType="email-address"
      />
      <FormField
        label="Password"
        testID="register-password"
        value={password}
        onChangeText={setPassword}
        error={errors.password}
        secureTextEntry
      />

      {serverError ? (
        <Text testID="register-error" style={styles.serverError}>
          {serverError}
        </Text>
      ) : null}

      <Pressable
        testID="register-submit"
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
          <Text style={styles.buttonText}>Create account</Text>
        )}
      </Pressable>

      <Pressable
        testID="register-to-login"
        accessibilityRole="link"
        onPress={() => navigation.navigate('Login')}>
        <Text style={styles.link}>Already have an account?  Log in</Text>
      </Pressable>
    </ScrollView>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#0f2747'},
  content: {padding: 24, paddingTop: 48},
  heading: {
    color: '#ffffff',
    fontSize: 26,
    fontWeight: '700',
    marginBottom: 24,
  },
  serverError: {color: '#f1889a', fontSize: 13, marginBottom: 12},
  button: {
    backgroundColor: '#2f6fd0',
    borderRadius: 8,
    paddingVertical: 14,
    alignItems: 'center',
    marginTop: 4,
  },
  buttonDim: {opacity: 0.6},
  buttonText: {color: '#ffffff', fontSize: 16, fontWeight: '700'},
  link: {color: '#7fb0ef', fontSize: 14, textAlign: 'center', marginTop: 20},
});
