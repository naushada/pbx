package com.onprempbx.onprem.ui;

import com.onprempbx.onprem.service.AuthService;
import com.onprempbx.onprem.service.StatusService;
import com.vaadin.flow.component.applayout.AppLayout;
import com.vaadin.flow.component.applayout.DrawerToggle;
import com.vaadin.flow.component.button.Button;
import com.vaadin.flow.component.button.ButtonVariant;
import com.vaadin.flow.component.html.H2;
import com.vaadin.flow.component.html.Span;
import com.vaadin.flow.component.orderedlayout.FlexComponent;
import com.vaadin.flow.component.orderedlayout.HorizontalLayout;
import com.vaadin.flow.component.sidenav.SideNav;
import com.vaadin.flow.component.sidenav.SideNavItem;
import com.vaadin.flow.router.BeforeEnterEvent;
import com.vaadin.flow.router.BeforeEnterObserver;
import com.vaadin.flow.router.PageTitle;
import com.vaadin.flow.theme.lumo.LumoUtility;

/**
 * Shell for every authenticated view.
 *
 * <p>Left rail: Dashboard, Societies, Subscribers. Header: logged-in
 * admin's display label + a Logout button. The {@link
 * BeforeEnterObserver} kicks the user back to {@code /login} if
 * they hit a child view without a valid session token.
 */
@PageTitle("onprem-pbx admin")
public class MainLayout extends AppLayout implements BeforeEnterObserver {

    private final AuthService auth;
    private final StatusService status;

    public MainLayout(AuthService auth, StatusService status) {
        this.auth = auth;
        this.status = status;
        addHeader();
        addConnectivityBar();
        addDrawer();
    }

    @Override
    public void beforeEnter(BeforeEnterEvent event) {
        final String token = auth.getToken();
        if (token == null || token.isBlank()) {
            event.forwardTo(LoginView.class);
        }
    }

    private void addHeader() {
        final H2 title = new H2("onprem-pbx admin");
        title.addClassNames(LumoUtility.FontSize.LARGE, LumoUtility.Margin.MEDIUM);

        final Span userLabel = new Span(currentUserLabel());
        userLabel.addClassNames(LumoUtility.Margin.End.MEDIUM);

        final Button logout = new Button("Log out", click -> {
            auth.logout();
            getUI().ifPresent(ui -> ui.navigate("login"));
        });
        logout.addThemeVariants(ButtonVariant.LUMO_TERTIARY);

        final HorizontalLayout header = new HorizontalLayout(
                new DrawerToggle(), title, userLabel, logout);
        header.setDefaultVerticalComponentAlignment(FlexComponent.Alignment.CENTER);
        header.expand(title);
        header.setWidthFull();
        header.addClassNames(LumoUtility.Padding.Horizontal.MEDIUM);

        addToNavbar(header);
    }

    /**
     * Connectivity sub-navbar — added as a second {@code addToNavbar}
     * call so it stacks under the header strip. Lives the lifetime of
     * the layout (re-attached per view navigation; polling is bounded
     * by the bar's own attach/detach hooks).
     */
    private void addConnectivityBar() {
        addToNavbar(new ConnectivityBar(status));
    }

    private void addDrawer() {
        final SideNav nav = new SideNav();
        nav.addItem(new SideNavItem("Dashboard",   "dashboard"));
        nav.addItem(new SideNavItem("Societies",   "societies"));
        nav.addItem(new SideNavItem("Subscribers", "subscribers"));
        addToDrawer(nav);
    }

    private String currentUserLabel() {
        final AuthService.CurrentUser u = auth.getCurrentUser();
        return (u == null) ? "" : u.displayLabel();
    }
}
