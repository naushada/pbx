import { NgModule } from '@angular/core';
import { RouterModule, Routes } from '@angular/router';

import { LoginComponent } from './login/login.component';
import { MainComponent } from './main/main.component';
import { DashboardComponent } from './dashboard/dashboard.component';
import { DirectoryComponent } from './directory/directory.component';
import { AuthGuard } from 'src/common/auth.guard';

// /main and its children require a valid session. Slice-1 surface is
// just `dashboard`; directory / dialer / call / history land as
// additional children in later slices.
const routes: Routes = [
    { path: '',           redirectTo: '/login', pathMatch: 'full' },
    { path: 'login',      component: LoginComponent },
    {
        path: 'main', component: MainComponent, canActivate: [AuthGuard],
        children: [
            { path: '',          redirectTo: 'dashboard', pathMatch: 'full' },
            { path: 'dashboard', component: DashboardComponent },
            { path: 'directory', component: DirectoryComponent },
        ],
    },
    { path: '**', redirectTo: '/login' },
];

@NgModule({
    imports: [RouterModule.forRoot(routes)],
    exports: [RouterModule],
})
export class AppRoutingModule {}
