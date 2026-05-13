import { ComponentFixture, TestBed, fakeAsync, tick } from '@angular/core/testing';
import { ReactiveFormsModule } from '@angular/forms';
import { Router } from '@angular/router';
import { RouterTestingModule } from '@angular/router/testing';
import { HttpClientTestingModule, HttpTestingController } from '@angular/common/http/testing';
import { ClarityModule } from '@clr/angular';
import { NoopAnimationsModule } from '@angular/platform-browser/animations';

import { LoginComponent } from './login.component';
import { AuthService } from 'src/common/auth.service';
import { UriMap } from 'src/common/app-globals';

describe('LoginComponent', () => {

    let fixture: ComponentFixture<LoginComponent>;
    let comp:    LoginComponent;
    let backend: HttpTestingController;
    let auth:    AuthService;
    let router:  Router;

    const loginUrl = UriMap.get('from_web_subscriber_login') as string;

    beforeEach(async () => {
        localStorage.clear();
        await TestBed.configureTestingModule({
            declarations: [LoginComponent],
            imports: [
                ReactiveFormsModule, HttpClientTestingModule,
                ClarityModule, NoopAnimationsModule,
                RouterTestingModule.withRoutes([
                    { path: 'main/dashboard', children: [] },
                    { path: 'login',          children: [] },
                ]),
            ],
        }).compileComponents();

        fixture = TestBed.createComponent(LoginComponent);
        comp    = fixture.componentInstance;
        backend = TestBed.inject(HttpTestingController);
        auth    = TestBed.inject(AuthService);
        router  = TestBed.inject(Router);
        fixture.detectChanges();
    });

    afterEach(() => backend.verify());

    it('does not submit when the form is invalid', () => {
        comp.onLogin();
        backend.expectNone(loginUrl);
        expect(comp.errorMessage).toBe('');
    });

    it('on success stores session and navigates to /main/dashboard', fakeAsync(() => {
        const navSpy = spyOn(router, 'navigateByUrl');
        comp.loginForm.setValue({
            societyCode: 'greenwoods', flatNumber: 'A-204', password: 'secret',
        });
        comp.onLogin();

        const req = backend.expectOne(loginUrl);
        expect(req.request.method).toBe('POST');
        req.flush({
            token:      'tok-xyz',
            subscriber: {
                societyId: 's1', flatNumber: 'A-204', displayName: 'Alice',
                sipUser: 'A-204', role: 'resident',
            },
        });
        tick();

        expect(auth.isAuthenticated()).toBeTrue();
        expect(auth.getToken()).toBe('tok-xyz');
        expect(navSpy).toHaveBeenCalledWith('/main/dashboard');
    }));

    it('on 401 surfaces an invalid-credentials message and does not navigate', fakeAsync(() => {
        const navSpy = spyOn(router, 'navigateByUrl');
        comp.loginForm.setValue({
            societyCode: 'greenwoods', flatNumber: 'A-204', password: 'wrong',
        });
        comp.onLogin();

        backend.expectOne(loginUrl).flush('bad', {
            status: 401, statusText: 'Unauthorized',
        });
        tick();

        expect(comp.errorMessage).toMatch(/invalid/i);
        expect(auth.isAuthenticated()).toBeFalse();
        expect(navSpy).not.toHaveBeenCalled();
    }));

    it('on network failure (status 0) surfaces a connectivity message', fakeAsync(() => {
        comp.loginForm.setValue({
            societyCode: 'greenwoods', flatNumber: 'A-204', password: 'x',
        });
        comp.onLogin();

        backend.expectOne(loginUrl).error(new ProgressEvent('error'), {
            status: 0, statusText: '',
        });
        tick();

        expect(comp.errorMessage).toMatch(/cannot reach/i);
    }));
});
