import { Component } from '@angular/core';
import { FormBuilder, FormGroup, Validators } from '@angular/forms';
import { Router } from '@angular/router';
import { HttpErrorResponse } from '@angular/common/http';
import { ClrLoadingState } from '@clr/angular';

import { HttpsvcService } from 'src/common/httpsvc.service';
import { AuthService } from 'src/common/auth.service';

// Subscriber login form. POSTs to /api/v1/subscriber/login (via
// HttpsvcService.login) and on success stores the bearer + cached
// subscriber via AuthService, then routes to /main/dashboard.

@Component({
    selector: 'app-login',
    templateUrl: './login.component.html',
    styleUrls: ['./login.component.scss'],
})
export class LoginComponent {

    loginForm: FormGroup;
    submitState: ClrLoadingState = ClrLoadingState.DEFAULT;
    errorMessage = '';

    constructor(
        private fb: FormBuilder,
        private router: Router,
        private http: HttpsvcService,
        private auth: AuthService,
    ) {
        this.loginForm = this.fb.group({
            societyCode: ['', Validators.required],
            flatNumber:  ['', Validators.required],
            password:    ['', Validators.required],
        });
    }

    onLogin(): void {
        if (this.loginForm.invalid) {
            this.loginForm.markAllAsTouched();
            return;
        }

        const { societyCode, flatNumber, password } = this.loginForm.value;
        this.submitState = ClrLoadingState.LOADING;
        this.errorMessage = '';

        this.http.login(societyCode, flatNumber, password).subscribe({
            next: (rsp) => {
                this.auth.setSession(rsp.token, rsp.subscriber);
                this.submitState = ClrLoadingState.SUCCESS;
                this.router.navigateByUrl('/main/dashboard');
            },
            error: (err: HttpErrorResponse) => {
                this.submitState = ClrLoadingState.ERROR;
                this.errorMessage = err.status === 401
                    ? 'Invalid society code, flat number, or password.'
                    : err.status === 0
                        ? 'Cannot reach the cloud. Check your connection.'
                        : `Login failed: ${err.statusText || 'unknown error'}.`;
            },
        });
    }
}
