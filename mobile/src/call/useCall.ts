/**
 * useCall — TDD layer M2.d.
 *
 * Subscribes a component to a `CallController`'s call-state changes and
 * returns the current `Call`, re-rendering on every transition.
 */
import {useEffect, useState} from 'react';
import {Call} from '../sip/callState';
import {CallController} from './callController';

export function useCall(controller: CallController): Call {
  const [call, setCall] = useState<Call>(() => controller.getCall());
  useEffect(() => controller.subscribe(setCall), [controller]);
  return call;
}
