import { Component } from '@angular/core';

// Placeholder dashboard. Eventually surfaces:
//   - SIP registration status (idle / registering / registered / failed)
//   - Recent calls (last 5 CDR rows)
//   - Push-notification permission state
@Component({
    selector: 'app-dashboard',
    templateUrl: './dashboard.component.html',
    styleUrls: ['./dashboard.component.scss'],
})
export class DashboardComponent {}
