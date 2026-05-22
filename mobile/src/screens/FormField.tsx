/**
 * FormField — a labelled text input with an inline error, shared by the
 * Login and Create-account screens (TDD layer M1.c).
 *
 * Pass `testID`; the input gets it, and the error message (when shown)
 * gets `${testID}-error` so component tests can target both.
 */
import React from 'react';
import {StyleSheet, Text, TextInput, TextInputProps, View} from 'react-native';

export interface FormFieldProps extends TextInputProps {
  label: string;
  error?: string;
  optional?: boolean;
}

export function FormField({
  label,
  error,
  optional,
  ...inputProps
}: FormFieldProps): React.JSX.Element {
  const tid = inputProps.testID;
  return (
    <View style={styles.field}>
      <Text style={styles.label}>
        {label}
        {optional ? <Text style={styles.optional}>  ·  optional</Text> : null}
      </Text>
      <TextInput
        style={[styles.input, error ? styles.inputError : null]}
        placeholderTextColor="#7f97b5"
        autoCapitalize="none"
        autoCorrect={false}
        {...inputProps}
      />
      {error ? (
        <Text testID={tid ? `${tid}-error` : undefined} style={styles.error}>
          {error}
        </Text>
      ) : null}
    </View>
  );
}

const styles = StyleSheet.create({
  field: {marginBottom: 16},
  label: {color: '#cfe0f5', fontSize: 13, marginBottom: 6, fontWeight: '600'},
  optional: {color: '#7f97b5', fontSize: 12, fontWeight: '400'},
  input: {
    backgroundColor: '#15325a',
    borderColor: '#2b4871',
    borderWidth: 1,
    borderRadius: 8,
    color: '#ffffff',
    fontSize: 16,
    paddingHorizontal: 12,
    paddingVertical: 10,
  },
  inputError: {borderColor: '#e0556b'},
  error: {color: '#f1889a', fontSize: 12, marginTop: 4},
});
