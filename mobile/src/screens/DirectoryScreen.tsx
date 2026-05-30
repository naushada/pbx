/**
 * DirectoryScreen — society-scoped subscriber list with tap-to-call.
 *
 * Closes the parity gap with the web softphone's `DirectoryComponent`
 * (`ui/src/app/directory/directory.component.ts`). Fetches
 * `GET /api/v1/subscriber?societyId=…` on mount, lets the user filter
 * by typing into the same flat-prefix box the web has, and dials the
 * tapped entry via the existing `CallController` seam.
 *
 * UX choices:
 *   - The current user's own flat is filtered out — same as web's
 *     directory.component which compares against the auth subscriber.
 *   - An offline dot (•) is shown for `online: false` entries; the
 *     online dot is green. `online` is the last-known REGISTER state
 *     from the agent's presence cache.
 *   - The Call button is disabled for offline entries (the web's
 *     same gate via `isOnline(entry)`).
 *   - When a call goes active the same `InCallPanel` takes over, just
 *     like on `DialScreen`. The user returns to the directory list on
 *     hangup, not to the dial screen.
 */
import React, {useEffect, useMemo, useState} from 'react';
import {
  ActivityIndicator,
  FlatList,
  Pressable,
  StyleSheet,
  Text,
  TextInput,
  View,
} from 'react-native';
import type {NativeStackScreenProps} from '@react-navigation/native-stack';
import type {RootStackParamList} from '../navigation/types';
import {DirectoryEntry} from '../api/types';
import {useDeps} from '../state/deps';
import {useCall} from '../call/useCall';
import {isCallActive} from '../sip/callState';
import {InCallPanel} from './InCallPanel';

type Props = NativeStackScreenProps<RootStackParamList, 'Directory'>;

export function DirectoryScreen({navigation, route}: Props): React.JSX.Element {
  const {client, callController} = useDeps();
  const {subscriber} = route.params.session;
  const call = useCall(callController);

  const [entries, setEntries] = useState<DirectoryEntry[] | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [filter, setFilter] = useState('');

  useEffect(() => {
    let cancelled = false;
    client
      .getDirectory(subscriber.societyId)
      .then(rows => {
        if (cancelled) return;
        // Same self-filter as the web softphone — don't list the user's
        // own flat in their own directory.
        setEntries(rows.filter(r => r.flatNumber !== subscriber.flatNumber));
      })
      .catch((e: unknown) => {
        if (cancelled) return;
        setError(e instanceof Error ? e.message : 'Lookup failed');
        setEntries([]);
      });
    return () => {
      cancelled = true;
    };
  }, [client, subscriber.societyId, subscriber.flatNumber]);

  // Hooks block first — useMemo must always run on every render,
  // otherwise React's hook-count check fires when a call goes active
  // and the early-return below skips it.
  const visible = useMemo(() => {
    if (entries === null) return null;
    const q = filter.trim().toUpperCase();
    if (!q) return entries;
    return entries.filter(
      r =>
        r.flatNumber.toUpperCase().startsWith(q) ||
        r.displayName.toUpperCase().includes(q),
    );
  }, [entries, filter]);

  // A call in flight takes over the whole screen.
  if (isCallActive(call)) {
    return <InCallPanel call={call} controller={callController} />;
  }

  return (
    <View style={styles.root} testID="directory-screen">
      <View style={styles.header}>
        <Pressable
          testID="directory-back"
          accessibilityRole="button"
          accessibilityLabel="Back to dialer"
          onPress={() => navigation.goBack()}
          style={({pressed}) => [styles.back, pressed && styles.backDim]}>
          <Text style={styles.backText}>← Dial</Text>
        </Pressable>
        <Text style={styles.title}>Directory</Text>
      </View>

      <TextInput
        testID="directory-filter"
        value={filter}
        onChangeText={setFilter}
        placeholder="Filter by flat or name"
        placeholderTextColor="#7f97b5"
        autoCapitalize="characters"
        style={styles.filter}
      />

      {visible === null ? (
        <View style={styles.center}>
          <ActivityIndicator color="#9bb3d1" />
          <Text style={styles.status}>Loading…</Text>
        </View>
      ) : visible.length === 0 ? (
        <View style={styles.center}>
          <Text style={styles.status}>
            {error
              ? error
              : entries && entries.length === 0
              ? 'No other flats in this society yet.'
              : 'No matches.'}
          </Text>
        </View>
      ) : (
        <FlatList
          testID="directory-list"
          data={visible}
          keyExtractor={r => r.flatNumber}
          renderItem={({item}) => (
            <DirectoryRow
              entry={item}
              onCall={() => callController.placeCall(item.flatNumber)}
            />
          )}
          contentContainerStyle={styles.listContent}
        />
      )}
    </View>
  );
}

interface RowProps {
  entry: DirectoryEntry;
  onCall: () => void;
}

function DirectoryRow({entry, onCall}: RowProps): React.JSX.Element {
  return (
    <View style={styles.row} testID={`directory-row-${entry.flatNumber}`}>
      <View
        style={[styles.dot, entry.online ? styles.online : styles.offline]}
        testID={`directory-presence-${entry.flatNumber}`}
      />
      <View style={styles.rowText}>
        <Text style={styles.rowFlat}>{entry.flatNumber}</Text>
        <Text style={styles.rowName}>{entry.displayName}</Text>
      </View>
      <Pressable
        testID={`directory-call-${entry.flatNumber}`}
        accessibilityRole="button"
        accessibilityLabel={`Call ${entry.flatNumber}`}
        disabled={!entry.online}
        onPress={onCall}
        style={({pressed}) => [
          styles.call,
          (!entry.online || pressed) && styles.callDim,
        ]}>
        <Text style={styles.callText}>Call</Text>
      </Pressable>
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#0f2747', paddingTop: 56},
  header: {
    flexDirection: 'row',
    alignItems: 'center',
    paddingHorizontal: 16,
    marginBottom: 16,
  },
  back: {paddingVertical: 6, paddingHorizontal: 10, marginRight: 8},
  backDim: {opacity: 0.5},
  backText: {color: '#7fb0ef', fontSize: 15, fontWeight: '600'},
  title: {color: '#ffffff', fontSize: 22, fontWeight: '700'},
  filter: {
    marginHorizontal: 16,
    marginBottom: 8,
    paddingHorizontal: 14,
    paddingVertical: 12,
    backgroundColor: '#1a3258',
    color: '#ffffff',
    borderRadius: 8,
    fontSize: 16,
  },
  center: {flex: 1, alignItems: 'center', justifyContent: 'center', padding: 24},
  status: {color: '#9bb3d1', fontSize: 14, marginTop: 8, textAlign: 'center'},
  listContent: {paddingHorizontal: 16, paddingBottom: 24},
  row: {
    flexDirection: 'row',
    alignItems: 'center',
    paddingVertical: 12,
    borderBottomWidth: 1,
    borderBottomColor: '#1a3258',
  },
  dot: {width: 10, height: 10, borderRadius: 5, marginRight: 12},
  online: {backgroundColor: '#34c759'},
  offline: {backgroundColor: '#3a567c'},
  rowText: {flex: 1},
  rowFlat: {color: '#ffffff', fontSize: 17, fontWeight: '700'},
  rowName: {color: '#cfe0f5', fontSize: 13, marginTop: 2},
  call: {
    paddingVertical: 8,
    paddingHorizontal: 16,
    borderRadius: 999,
    backgroundColor: '#2f7d4f',
  },
  callDim: {opacity: 0.5},
  callText: {color: '#ffffff', fontSize: 14, fontWeight: '700'},
});
