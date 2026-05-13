import { Injectable } from '@angular/core';
import {
    HttpEvent, HttpHandler, HttpInterceptor, HttpRequest, HttpErrorResponse,
} from '@angular/common/http';
import { Router } from '@angular/router';
import { Observable, throwError } from 'rxjs';
import { catchError } from 'rxjs/operators';

import { AuthService } from './auth.service';
import { UriMap } from './app-globals';

// Attaches the bearer token to every outbound request except the
// login endpoint itself (which doesn't have a token yet). On a 401
// from any other endpoint, clears the session and bounces the user
// back to /login so they can re-authenticate.

const LOGIN_PATH = UriMap.get('from_web_subscriber_login') ?? '';

@Injectable()
export class AuthInterceptor implements HttpInterceptor {

    constructor(private auth: AuthService, private router: Router) {}

    intercept(req: HttpRequest<unknown>, next: HttpHandler): Observable<HttpEvent<unknown>> {
        // Skip the login call — no token yet, and we don't want a 401
        // from POST /login to bounce the user away from the form.
        const skipAuth = req.url.endsWith(LOGIN_PATH);
        const token    = this.auth.getToken();

        const outbound = (skipAuth || !token)
            ? req
            : req.clone({ setHeaders: { Authorization: `Bearer ${token}` } });

        return next.handle(outbound).pipe(
            catchError((err: HttpErrorResponse) => {
                if (err.status === 401 && !skipAuth) {
                    this.auth.clearSession();
                    this.router.navigateByUrl('/login');
                }
                return throwError(() => err);
            }),
        );
    }
}
