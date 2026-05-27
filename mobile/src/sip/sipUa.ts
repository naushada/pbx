/**
 * SIP UA seam — kept in sync with the Angular web softphone via the
 * shared `shared/sip-ua/sip-ua.ts` module. This file is a re-export so
 * a single source defines the interfaces consumed by both apps.
 *
 * Production wires `SipJsUaFactory` (sipJsUaFactory.ts); tests wire a
 * fake factory (see `__tests__/sipCallController.test.ts`).
 */
export * from '../../../shared/sip-ua/sip-ua';
