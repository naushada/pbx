import { Component } from '@angular/core';

// Placeholder for slice 0. Slice 1 wires the actual subscriber-login
// form against POST /api/v1/subscriber/login (see HttpsvcService.login).
@Component({
    selector: 'app-login',
    templateUrl: './login.component.html',
    styleUrls: ['./login.component.scss'],
})
export class LoginComponent {}
