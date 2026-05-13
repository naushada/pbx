import { Injectable } from '@angular/core';
import { CanActivate, Router, UrlTree } from '@angular/router';

import { AuthService } from './auth.service';

// Route guard for /main and its children. Unauthenticated traffic is
// rerouted to /login via a UrlTree — Angular handles the redirect
// without the guard issuing an imperative navigation.

@Injectable({ providedIn: 'root' })
export class AuthGuard implements CanActivate {

    constructor(private auth: AuthService, private router: Router) {}

    canActivate(): true | UrlTree {
        return this.auth.isAuthenticated() ? true : this.router.parseUrl('/login');
    }
}
