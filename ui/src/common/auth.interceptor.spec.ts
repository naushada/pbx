import { TestBed } from '@angular/core/testing';
import { HttpClient, HTTP_INTERCEPTORS, HttpClientModule } from '@angular/common/http';
import {
    HttpClientTestingModule, HttpTestingController,
} from '@angular/common/http/testing';
import { Router } from '@angular/router';
import { RouterTestingModule } from '@angular/router/testing';

import { AuthInterceptor } from './auth.interceptor';
import { AuthService } from './auth.service';
import { UriMap } from './app-globals';

describe('AuthInterceptor', () => {

    let http:    HttpClient;
    let backend: HttpTestingController;
    let auth:    AuthService;
    let router:  Router;

    const subscriber = {
        societyId:   's1', flatNumber: 'A-1', displayName: 'A',
        sipUser:     'A-1', role: 'resident' as const,
    };

    beforeEach(() => {
        localStorage.clear();
        TestBed.configureTestingModule({
            imports: [
                HttpClientTestingModule,
                RouterTestingModule.withRoutes([{ path: 'login', children: [] }]),
            ],
            providers: [
                { provide: HTTP_INTERCEPTORS, useClass: AuthInterceptor, multi: true },
            ],
        });
        http    = TestBed.inject(HttpClient);
        backend = TestBed.inject(HttpTestingController);
        auth    = TestBed.inject(AuthService);
        router  = TestBed.inject(Router);
    });

    afterEach(() => backend.verify());

    it('attaches Authorization header when a token is set', () => {
        auth.setSession('tok-abc', subscriber);

        http.get('/api/v1/cdr').subscribe();
        const req = backend.expectOne('/api/v1/cdr');
        expect(req.request.headers.get('Authorization')).toBe('Bearer tok-abc');
        req.flush({});
    });

    it('omits Authorization header when no token is set', () => {
        http.get('/api/v1/cdr').subscribe();
        const req = backend.expectOne('/api/v1/cdr');
        expect(req.request.headers.has('Authorization')).toBeFalse();
        req.flush({});
    });

    it('does not attach Authorization to the login endpoint', () => {
        auth.setSession('tok-abc', subscriber);

        const loginUrl = UriMap.get('from_web_subscriber_login') as string;
        http.post(loginUrl, {}).subscribe();
        const req = backend.expectOne(loginUrl);
        expect(req.request.headers.has('Authorization')).toBeFalse();
        req.flush({});
    });

    it('clears session and routes to /login on 401', (done) => {
        auth.setSession('tok-abc', subscriber);
        const spy = spyOn(router, 'navigateByUrl');

        http.get('/api/v1/cdr').subscribe({
            error: () => {
                expect(auth.isAuthenticated()).toBeFalse();
                expect(spy).toHaveBeenCalledWith('/login');
                done();
            },
        });
        backend.expectOne('/api/v1/cdr').flush('nope', {
            status: 401, statusText: 'Unauthorized',
        });
    });
});
