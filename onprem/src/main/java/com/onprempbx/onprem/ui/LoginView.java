package com.onprempbx.onprem.ui;

import com.onprempbx.onprem.model.LoginCredentials;
import com.onprempbx.onprem.service.AuthException;
import com.onprempbx.onprem.service.AuthService;
import com.vaadin.flow.component.button.Button;
import com.vaadin.flow.component.button.ButtonVariant;
import com.vaadin.flow.component.html.H1;
import com.vaadin.flow.component.notification.Notification;
import com.vaadin.flow.component.notification.NotificationVariant;
import com.vaadin.flow.component.orderedlayout.FlexComponent;
import com.vaadin.flow.component.orderedlayout.VerticalLayout;
import com.vaadin.flow.component.textfield.PasswordField;
import com.vaadin.flow.component.textfield.TextField;
import com.vaadin.flow.data.binder.Binder;
import com.vaadin.flow.router.PageTitle;
import com.vaadin.flow.router.Route;
import com.vaadin.flow.server.auth.AnonymousAllowed;

/**
 * Public login page. Three fields — society label, flat number (defaults
 * to {@code ADMIN}), password — bound to a {@link LoginCredentials}.
 *
 * <p>On success → navigates to {@code /dashboard}. On
 * {@link AuthException} → shows a notification distinguishing 401
 * ("Invalid credentials") from 403 ("Account disabled").
 */
@Route("login")
@PageTitle("onprem-pbx admin · login")
@AnonymousAllowed
public class LoginView extends VerticalLayout {

    private final AuthService auth;

    public LoginView(AuthService auth) {
        this.auth = auth;

        setSizeFull();
        setAlignItems(FlexComponent.Alignment.CENTER);
        setJustifyContentMode(FlexComponent.JustifyContentMode.CENTER);

        final H1 title = new H1("onprem-pbx admin");

        // Form-label only. The wire field stays `societyCode` (bound to
        // LoginCredentials.societyCode, matched by the cloud's
        // handle_subscriber_login_POST). "Society label" reads friendlier
        // for non-technical operators — "code" implied a system identifier.
        final TextField societyCode = new TextField("Society label");
        societyCode.setRequiredIndicatorVisible(true);
        societyCode.setWidth("20em");

        final TextField flatNumber = new TextField("Flat number");
        flatNumber.setValue("ADMIN");
        flatNumber.setRequiredIndicatorVisible(true);
        flatNumber.setWidth("20em");

        final PasswordField password = new PasswordField("Password");
        password.setRequiredIndicatorVisible(true);
        password.setWidth("20em");

        final Binder<LoginCredentials> binder = new Binder<>(LoginCredentials.class);
        binder.forField(societyCode)
                .asRequired("Required")
                .bind(LoginCredentials::getSocietyCode, LoginCredentials::setSocietyCode);
        binder.forField(flatNumber)
                .asRequired("Required")
                .bind(LoginCredentials::getFlatNumber, LoginCredentials::setFlatNumber);
        binder.forField(password)
                .asRequired("Required")
                .bind(LoginCredentials::getPassword, LoginCredentials::setPassword);

        final Button submit = new Button("Log in", e -> {
            final LoginCredentials creds = new LoginCredentials();
            try {
                binder.writeBean(creds);
            } catch (Exception writeErr) {
                // Binder already shows per-field errors; abort the click.
                return;
            }
            try {
                auth.login(creds);
                getUI().ifPresent(ui -> ui.navigate("dashboard"));
            } catch (AuthException ae) {
                final Notification n = Notification.show(ae.getMessage(), 3000,
                        Notification.Position.MIDDLE);
                n.addThemeVariants(NotificationVariant.LUMO_ERROR);
            } catch (Exception ex) {
                // Network / 5xx — surface a generic message.
                final Notification n = Notification.show(
                        "Could not reach backend: " + ex.getMessage(),
                        5000, Notification.Position.MIDDLE);
                n.addThemeVariants(NotificationVariant.LUMO_ERROR);
            }
        });
        submit.addThemeVariants(ButtonVariant.LUMO_PRIMARY);

        add(title, societyCode, flatNumber, password, submit);
    }
}
