import { Component, OnInit } from '@angular/core';
import { FormBuilder, FormGroup, Validators } from '@angular/forms';
import { ActivatedRoute, Router } from '@angular/router';
import { HttpErrorResponse } from '@angular/common/http';

import { HttpsvcService }   from 'src/common/httpsvc.service';
import { AuthService }      from 'src/common/auth.service';

// Plain HTML form (no Clarity directives) — the Clarity form
// components misbehave outside a clr-main-container ancestor and we
// intentionally don't wrap the login page in one.

@Component({
    selector: 'app-login',
    templateUrl: './login.component.html',
    styleUrls: ['./login.component.scss'],
})
export class LoginComponent implements OnInit {

    loginForm: FormGroup;
    loading = false;
    errorMessage = '';
    timedOut = false;

    constructor(
        private fb: FormBuilder,
        private router: Router,
        private route: ActivatedRoute,
        private http: HttpsvcService,
        private auth: AuthService,
    ) {
        // Prefill the form with whatever the user last typed
        // successfully. Password is never stored.
        const last = this.auth.getLastCredentials();
        this.loginForm = this.fb.group({
            societyCode: [last?.societyCode ?? '', Validators.required],
            flatNumber:  [last?.flatNumber  ?? '', Validators.required],
            password:    ['',                       Validators.required],
        });
    }

    ngOnInit(): void {
        // If we got bounced here by the keep-alive watchdog, show a
        // banner so the user knows why the form reappeared.
        this.timedOut = this.route.snapshot.queryParamMap.get('reason') === 'timeout';
    }

    onLogin(): void {
        if (this.loginForm.invalid) {
            this.loginForm.markAllAsTouched();
            return;
        }

        const { societyCode, flatNumber, password } = this.loginForm.value;
        this.loading = true;
        this.errorMessage = '';
        this.timedOut = false;

        this.http.login(societyCode, flatNumber, password).subscribe({
            next: (rsp) => {
                this.auth.setSession(rsp.token, rsp.subscriber);
                this.auth.setLastCredentials(societyCode, flatNumber);
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
