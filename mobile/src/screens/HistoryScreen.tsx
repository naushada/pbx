/**
 * HistoryScreen — call history with newest-first ordering.
 *
 * Closes the parity gap with the web softphone's `HistoryComponent`
 * (`ui/src/app/history/history.component.ts`). Fetches
 * `GET /api/v1/cdr?societyId=…` on mount and renders the rows that
 * involve the signed-in flat (`fromFlat === self || toFlat === self`).
 *
 * Note: the web softphone does NOT filter client-side today — it
 * shows every CDR in the society. Mobile does the right thing
 * because the cloud's `handle_cdr_GET` ignores the `?flat=` param
 * the web sends; server-side filtering is a future improvement.
 *
 * The reusable bits are mirrored from `DirectoryScreen`:
 *   - useMemo runs before the early-return for InCallPanel so the
 *     Rules-of-Hooks count is stable.
 *   - Tap-to-call dispatches `callController.placeCall(peerFlat)`
 *     — same call path as Directory and DialScreen.
 */
import React, {useEffect, useMemo, useState} from 'react';
import {
  ActivityIndicator,
  FlatList,
  Pressable,
  StyleSheet,
  Text,
  View,
} from 'react-native';
import type {NativeStackScreenProps} from '@react-navigation/native-stack';
import type {RootStackParamList} from '../navigation/types';
import {CallRecord, HangupCause} from '../api/types';
import {useDeps} from '../state/deps';
import {useCall} from '../call/useCall';
import {isCallActive} from '../sip/callState';
import {InCallPanel} from './InCallPanel';

type Props = NativeStackScreenProps<RootStackParamList, 'History'>;

export function HistoryScreen({navigation, route}: Props): React.JSX.Element {
  const {client, callController} = useDeps();
  const {subscriber} = route.params.session;
  const call = useCall(callController);

  const [rows, setRows] = useState<CallRecord[] | null>(null);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    let cancelled = false;
    client
      .getCallHistory(subscriber.societyId)
      .then(all => {
        if (cancelled) return;
        // Only rows involving the signed-in flat — the cloud's
        // `handle_cdr_GET` is society-scoped and doesn't filter.
        const mine = all.filter(
          r =>
            r.fromFlat === subscriber.flatNumber ||
            r.toFlat === subscriber.flatNumber,
        );
        setRows(mine);
      })
      .catch((e: unknown) => {
        if (cancelled) return;
        setError(e instanceof Error ? e.message : 'Couldn’t load history');
        setRows([]);
      });
    return () => {
      cancelled = true;
    };
  }, [client, subscriber.societyId, subscriber.flatNumber]);

  // Newest-first sort — runs every render so hook count stays stable
  // across the InCallPanel handover below.
  const sorted = useMemo(() => {
    if (rows === null) return null;
    return rows
      .slice()
      .sort((a, b) => b.startedAt.localeCompare(a.startedAt));
  }, [rows]);

  if (isCallActive(call)) {
    return <InCallPanel call={call} controller={callController} />;
  }

  return (
    <View style={styles.root} testID="history-screen">
      <View style={styles.header}>
        <Pressable
          testID="history-back"
          accessibilityRole="button"
          accessibilityLabel="Back to dialer"
          onPress={() => navigation.goBack()}
          style={({pressed}) => [styles.back, pressed && styles.backDim]}>
          <Text style={styles.backText}>← Dial</Text>
        </Pressable>
        <Text style={styles.title}>Call history</Text>
      </View>

      {sorted === null ? (
        <View style={styles.center}>
          <ActivityIndicator color="#9bb3d1" />
          <Text style={styles.status}>Loading…</Text>
        </View>
      ) : sorted.length === 0 ? (
        <View style={styles.center}>
          <Text style={styles.status}>
            {error ?? 'No calls yet — your history will show up here.'}
          </Text>
        </View>
      ) : (
        <FlatList
          testID="history-list"
          data={sorted}
          keyExtractor={r => r.callId}
          renderItem={({item}) => (
            <HistoryRow
              row={item}
              myFlat={subscriber.flatNumber}
              onCall={peer => callController.placeCall(peer)}
            />
          )}
          contentContainerStyle={styles.listContent}
        />
      )}
    </View>
  );
}

interface RowProps {
  row: CallRecord;
  myFlat: string;
  onCall: (peerFlat: string) => void;
}

function HistoryRow({row, myFlat, onCall}: RowProps): React.JSX.Element {
  const peer = row.fromFlat === myFlat ? row.toFlat : row.fromFlat;
  const direction = row.fromFlat === myFlat ? 'outbound' : 'inbound';
  return (
    <View style={styles.row} testID={`history-row-${row.callId}`}>
      <Text
        style={[
          styles.arrow,
          direction === 'inbound' ? styles.inbound : styles.outbound,
        ]}>
        {direction === 'inbound' ? '↘' : '↗'}
      </Text>
      <View style={styles.rowText}>
        <Text style={styles.rowFlat}>{peer}</Text>
        <Text style={styles.rowMeta}>
          {humanWhen(row.startedAt)} · {durationLabel(row.durationSec)}
          {row.hangupCause !== 'normal' ? ` · ${row.hangupCause}` : ''}
        </Text>
      </View>
      <Pressable
        testID={`history-call-${row.callId}`}
        accessibilityRole="button"
        accessibilityLabel={`Call ${peer} back`}
        onPress={() => onCall(peer)}
        style={({pressed}) => [styles.call, pressed && styles.callDim]}>
        <Text style={styles.callText}>Call</Text>
      </Pressable>
    </View>
  );
}

/** mm:ss duration label; "—" for zero-duration (never-answered) rows. */
function durationLabel(seconds: number): string {
  if (!seconds || seconds <= 0) return '—';
  const mm = Math.floor(seconds / 60).toString().padStart(2, '0');
  const ss = (seconds % 60).toString().padStart(2, '0');
  return `${mm}:${ss}`;
}

/**
 * Short timestamp label — date OR time depending on age. Same shape
 * iMessage / WhatsApp use; users skim by recency, not absolute date.
 */
function humanWhen(iso: string): string {
  // Parsing happens at render so a malformed timestamp shows the raw
  // string instead of throwing.
  const t = Date.parse(iso);
  if (Number.isNaN(t)) return iso;
  const now = Date.now();
  const ageMs = now - t;
  const d = new Date(t);
  if (ageMs < 24 * 3600_000) {
    return d.toLocaleTimeString(undefined, {
      hour: 'numeric',
      minute: '2-digit',
    });
  }
  return d.toLocaleDateString();
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
  arrow: {width: 20, fontSize: 18, fontWeight: '600', marginRight: 8},
  inbound: {color: '#34c759'},
  outbound: {color: '#7fb0ef'},
  rowText: {flex: 1},
  rowFlat: {color: '#ffffff', fontSize: 17, fontWeight: '700'},
  rowMeta: {color: '#9bb3d1', fontSize: 13, marginTop: 2},
  call: {
    paddingVertical: 8,
    paddingHorizontal: 16,
    borderRadius: 999,
    backgroundColor: '#2f7d4f',
  },
  callDim: {opacity: 0.7},
  callText: {color: '#ffffff', fontSize: 14, fontWeight: '700'},
});

// `HangupCause` is imported as part of the API contract — kept in scope so
// future row renderers can branch on it without re-importing.
export type {HangupCause};
