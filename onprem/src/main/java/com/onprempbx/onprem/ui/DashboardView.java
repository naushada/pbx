package com.onprempbx.onprem.ui;

import com.onprempbx.onprem.service.AuthService;
import com.vaadin.flow.component.html.H2;
import com.vaadin.flow.component.html.Paragraph;
import com.vaadin.flow.component.orderedlayout.VerticalLayout;
import com.vaadin.flow.router.PageTitle;
import com.vaadin.flow.router.Route;

/**
 * Placeholder landing page so post-login navigation has somewhere to go.
 *
 * <p>The real dashboard (society at-a-glance, online presence count,
 * recent CDR slice) lands in item #10 — see the
 * {@code project_vaadin_admin_ui} memory.
 */
@Route(value = "dashboard", layout = MainLayout.class)
@PageTitle("onprem-pbx admin · dashboard")
public class DashboardView extends VerticalLayout {

    public DashboardView(AuthService auth) {
        final AuthService.CurrentUser u = auth.getCurrentUser();
        final String label = (u == null) ? "(unknown)" : u.displayLabel();

        add(new H2("Welcome, " + label));
        add(new Paragraph("This is a placeholder. The real dashboard ships in PR #10."));
    }
}
