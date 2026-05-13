import { NgModule } from '@angular/core';
import { RouterModule, Routes } from '@angular/router';

import { LoginComponent } from './login/login.component';
import { MainComponent } from './main/main.component';
import { DashboardComponent } from './dashboard/dashboard.component';

// Placeholder route table for slice 0. Subsequent slices will add
// directory, dialer, call, history, and settings as children of /main.
const routes: Routes = [
    { path: '',           redirectTo: '/login', pathMatch: 'full' },
    { path: 'login',      component: LoginComponent },
    {
        path: 'main', component: MainComponent,
        children: [
            { path: '',          redirectTo: 'dashboard', pathMatch: 'full' },
            { path: 'dashboard', component: DashboardComponent },
        ],
    },
    { path: '**', redirectTo: '/login' },
];

@NgModule({
    imports: [RouterModule.forRoot(routes)],
    exports: [RouterModule],
})
export class AppRoutingModule {}
