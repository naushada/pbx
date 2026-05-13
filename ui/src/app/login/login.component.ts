import { Component } from '@angular/core';
import { FormBuilder, FormGroup, Validators } from '@angular/forms';
import { Router } from '@angular/router';
import { HttpErrorResponse } from '@angular/common/http';

import { HttpsvcService } from 'src/common/httpsvc.service';
import { AuthService } from 'src/common/auth.service';

// Plain HTML form (no Clarity directives) — the Clarity form
// components misbehave outside a clr-main-container ancestor and we
// intentionally don't wrap the login page in one.

@Component({
    selector: 'app-login',
    templateUrl: './login.component.html',
    styleUrls: ['./login.component.scss'],
})
export class LoginComponent {

    loginForm: FormGroup;
    loading = false;
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
        this.loading = true;
        this.errorMessage = '';

        this.http.login(societyCode, flatNumber, password).subscribe({
            next: (rsp) => {
                this.auth.setSession(rsp.token, rsp.subscriber);
                this.loading = false;
                this.router.navigateByUrl('/main/dashboard');
            },
            error: (err: HttpErrorResponse) => {
                this.loading = false;
                this.errorMessage = err.status === 401
                    ? 'Invalid society code, flat number, or password.'
                    : err.status === 0
                        ? 'Cannot reach the cloud. Check your connection.'
                        : `Login failed: ${err.statusText || 'unknown error'}.`;
            },
        });
    }
}
