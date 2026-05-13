import { Component, OnInit } from '@angular/core';
import { Router } from '@angular/router';

import { AuthService } from 'src/common/auth.service';

@Component({
    selector: 'app-root',
    templateUrl: './app.component.html',
    styleUrls: ['./app.component.scss'],
})
export class AppComponent implements OnInit {
    title = 'pbxui';

    constructor(private router: Router, private auth: AuthService) {}

    // On a fresh load, decide once: do we have a usable session? If
    // yes, dashboard; if not, login. Later navigations inside /main are
    // policed by AuthGuard.
    ngOnInit(): void {
        const target = this.auth.isAuthenticated() ? '/main/dashboard' : '/login';
        this.router.navigateByUrl(target);
    }
}
