import { LOCALE_ID, NgModule } from '@angular/core';
import { BrowserModule } from '@angular/platform-browser';
import { BrowserAnimationsModule } from '@angular/platform-browser/animations';
import { HTTP_INTERCEPTORS, HttpClientModule } from '@angular/common/http';
import { FormsModule, ReactiveFormsModule } from '@angular/forms';
import { ClarityModule } from '@clr/angular';
import { CdsModule } from '@cds/angular';
import { DatePipe } from '@angular/common';

import { AppRoutingModule } from './app-routing.module';
import { AppComponent } from './app.component';
import { LoginComponent } from './login/login.component';
import { MainComponent } from './main/main.component';
import { DashboardComponent } from './dashboard/dashboard.component';
import { DirectoryComponent } from './directory/directory.component';
import { CallPanelComponent } from './call-panel/call-panel.component';
import { AuthInterceptor } from 'src/common/auth.interceptor';
import { SIP_UA_FACTORY } from 'src/common/sip-ua';
import { SipJsUaFactory } from 'src/common/sip-ua-sipjs';

@NgModule({
    declarations: [
        AppComponent,
        LoginComponent,
        MainComponent,
        DashboardComponent,
        DirectoryComponent,
        CallPanelComponent,
    ],
    imports: [
        BrowserModule,
        BrowserAnimationsModule,
        HttpClientModule,
        FormsModule,
        ReactiveFormsModule,
        ClarityModule,
        CdsModule,
        AppRoutingModule,
    ],
    providers: [
        { provide: LOCALE_ID, useValue: 'en-US' },
        { provide: DatePipe },
        { provide: HTTP_INTERCEPTORS, useClass: AuthInterceptor, multi: true },
        { provide: SIP_UA_FACTORY,    useClass: SipJsUaFactory },
    ],
    bootstrap: [AppComponent],
})
export class AppModule {}
