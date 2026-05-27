import { Injectable } from '@angular/core';

import { SipJsUaFactory as SharedSipJsUaFactory }
    from '../../../shared/sip-ua/sip-ua-sipjs';

// Angular-injectable thin subclass — the implementation lives in the
// shared module so the React Native softphone reuses the same wiring.
// `useClass: SipJsUaFactory` in app.module.ts is the production binding.
@Injectable({ providedIn: 'root' })
export class SipJsUaFactory extends SharedSipJsUaFactory {}
