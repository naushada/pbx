import { TestBed } from '@angular/core/testing';
import { Router, UrlTree } from '@angular/router';
import { RouterTestingModule } from '@angular/router/testing';

import { AuthGuard } from './auth.guard';
import { AuthService } from './auth.service';

describe('AuthGuard', () => {

    let guard:  AuthGuard;
    let auth:   AuthService;
    let router: Router;

    beforeEach(() => {
        localStorage.clear();
        TestBed.configureTestingModule({
            imports: [
                RouterTestingModule.withRoutes([{ path: 'login', children: [] }]),
            ],
        });
        guard  = TestBed.inject(AuthGuard);
        auth   = TestBed.inject(AuthService);
        router = TestBed.inject(Router);
    });

    it('allows navigation when authenticated', () => {
        auth.setSession('tok', {
            societyId: 's', flatNumber: 'A-1', displayName: 'A',
            sipUser: 'A-1', role: 'resident',
        });
        expect(guard.canActivate()).toBeTrue();
    });

    it('returns a UrlTree pointing at /login when not authenticated', () => {
        const result = guard.canActivate();
        expect(result).toEqual(jasmine.any(UrlTree));
        expect(router.serializeUrl(result as UrlTree)).toBe('/login');
    });
});
