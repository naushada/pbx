import { Component, OnInit } from '@angular/core';
import { NavigationEnd, Router } from '@angular/router';
import { filter, take } from 'rxjs/operators';

import { AuthService } from 'src/common/auth.service';

@Component({
    selector: 'app-root',
    templateUrl: './app.component.html',
    styleUrls: ['./app.component.scss'],
})
export class AppComponent implements OnInit {
    title = 'pbxui';

    constructor(private router: Router, private auth: AuthService) {}

    // The Router's initial navigation honors the URL the user typed
    // (and AuthGuard on /main gates unauthenticated traffic). The only
    // case left to handle is the bare-root entry point: if we settle on
    // /login but the session is still valid, jump to the dashboard.
    // Deep links to /main/<anything> are left alone.
    ngOnInit(): void {
        this.router.events.pipe(
            filter(e => e instanceof NavigationEnd),
            take(1),
        ).subscribe(() => {
            if (this.router.url === '/login' && this.auth.isAuthenticated()) {
                this.router.navigateByUrl('/main/dashboard');
            }
        });
    }
}
