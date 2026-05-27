import { InjectionToken } from '@angular/core';

import type { SipUaFactory } from '../../../shared/sip-ua/sip-ua';

// Re-export every interface from the shared module so the rest of the
// Angular app keeps its existing `from 'src/common/sip-ua'` imports.
// The Angular-specific `SIP_UA_FACTORY` InjectionToken stays here.
export * from '../../../shared/sip-ua/sip-ua';

export const SIP_UA_FACTORY = new InjectionToken<SipUaFactory>('SipUaFactory');
