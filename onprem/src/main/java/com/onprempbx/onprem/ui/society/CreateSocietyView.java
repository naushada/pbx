package com.onprempbx.onprem.ui.society;

import com.onprempbx.onprem.model.Society;
import com.onprempbx.onprem.service.SocietyService;
import com.onprempbx.onprem.ui.MainLayout;
import com.vaadin.flow.component.UI;
import com.vaadin.flow.component.button.Button;
import com.vaadin.flow.component.button.ButtonVariant;
import com.vaadin.flow.component.dialog.Dialog;
import com.vaadin.flow.component.formlayout.FormLayout;
import com.vaadin.flow.component.html.H2;
import com.vaadin.flow.component.html.Paragraph;
import com.vaadin.flow.component.notification.Notification;
import com.vaadin.flow.component.notification.NotificationVariant;
import com.vaadin.flow.component.orderedlayout.FlexComponent;
import com.vaadin.flow.component.orderedlayout.HorizontalLayout;
import com.vaadin.flow.component.orderedlayout.VerticalLayout;
import com.vaadin.flow.component.textfield.TextArea;
import com.vaadin.flow.component.textfield.TextField;
import com.vaadin.flow.data.binder.Binder;
import com.vaadin.flow.data.validator.StringLengthValidator;
import com.vaadin.flow.router.PageTitle;
import com.vaadin.flow.router.Route;
import org.springframework.http.HttpStatus;
import org.springframework.web.client.HttpStatusCodeException;

/**
 * Create-society form.
 *
 * <p>Submits to {@link SocietyService#create(Society)} and pops a
 * one-shot dialog showing the cloud-derived {@code sipRealm} +
 * {@code turnSharedSecret} so the operator can paste them into the
 * on-prem agent's {@code .env}. Item #11 of the Vaadin admin UI plan.
 */
@Route(value = "societies/new", layout = MainLayout.class)
@PageTitle("Create society — onprem-pbx admin")
public class CreateSocietyView extends VerticalLayout {

    private final SocietyService societies;

    private final TextField code    = new TextField("Code");
    private final TextField name    = new TextField("Name");
    private final TextArea  address = new TextArea("Address");
    private final Binder<Society> binder = new Binder<>(Society.class);

    public CreateSocietyView(SocietyService societies) {
        this.societies = societies;

        setPadding(true);
        setSpacing(true);
        setMaxWidth("640px");

        add(new H2("Create society"));
        add(buildForm());
        add(buildActions());

        bind();
    }

    private FormLayout buildForm() {
        code.setRequiredIndicatorVisible(true);
        code.setHelperText("2–10 chars, used in the SIP realm");
        code.setMaxLength(10);

        name.setRequiredIndicatorVisible(true);

        address.setHelperText("Optional");

        final FormLayout form = new FormLayout();
        form.add(code, name, address);
        form.setColspan(address, 2);
        return form;
    }

    private HorizontalLayout buildActions() {
        final Button submit = new Button("Create", click -> submit());
        submit.addThemeVariants(ButtonVariant.LUMO_PRIMARY);

        final Button cancel = new Button("Cancel",
                click -> UI.getCurrent().navigate("societies"));

        final HorizontalLayout row = new HorizontalLayout(submit, cancel);
        row.setDefaultVerticalComponentAlignment(FlexComponent.Alignment.CENTER);
        return row;
    }

    private void bind() {
        binder.forField(code)
                .asRequired("Code is required")
                .withValidator(new StringLengthValidator(
                        "Code must be 2–10 characters", 2, 10))
                .bind(Society::getCode, Society::setCode);

        binder.forField(name)
                .asRequired("Name is required")
                .bind(Society::getName, Society::setName);

        binder.forField(address)
                .bind(Society::getAddress, Society::setAddress);
    }

    private void submit() {
        final Society draft = new Society();
        if (!binder.writeBeanIfValid(draft)) {
            return;
        }

        final Society created;
        try {
            created = societies.create(draft);
        } catch (HttpStatusCodeException e) {
            if (e.getStatusCode().value() == HttpStatus.CONFLICT.value()) {
                // Keep the form populated — operator should tweak the code, not retype everything.
                final Notification n = Notification.show(
                        "A society with code " + draft.getCode() + " already exists",
                        5000, Notification.Position.TOP_CENTER);
                n.addThemeVariants(NotificationVariant.LUMO_ERROR);
                return;
            }
            final Notification n = Notification.show(
                    "Create failed (HTTP " + e.getStatusCode().value() + ")",
                    5000, Notification.Position.TOP_CENTER);
            n.addThemeVariants(NotificationVariant.LUMO_ERROR);
            return;
        } catch (RuntimeException e) {
            final Notification n = Notification.show(
                    "Create failed: " + e.getMessage(),
                    5000, Notification.Position.TOP_CENTER);
            n.addThemeVariants(NotificationVariant.LUMO_ERROR);
            return;
        }

        if (created == null) {
            final Notification n = Notification.show(
                    "Cloud accepted the request but returned no body",
                    5000, Notification.Position.TOP_CENTER);
            n.addThemeVariants(NotificationVariant.LUMO_ERROR);
            return;
        }

        openSecretsDialog(created);
    }

    /**
     * One-shot dialog showing the cloud-minted SIP realm + TURN shared
     * secret. The values are surfaced ONCE on create — the operator can
     * re-fetch later via the {@link SocietiesView} detail dialog (the
     * cloud detail endpoint also surfaces the secret), so the copy
     * here is "shown ONCE", consistent with the messaging.
     */
    private void openSecretsDialog(Society created) {
        final Dialog dlg = new Dialog();
        dlg.setHeaderTitle("Society created — " + created.getCode());
        dlg.setCloseOnEsc(false);
        dlg.setCloseOnOutsideClick(false);
        dlg.setWidth("560px");

        final Paragraph hint = new Paragraph(
                "Add these to your on-prem .env: SIP_REALM=... and "
                + "TURN_SHARED_SECRET=.... They're shown ONCE — re-fetch "
                + "via the Societies list if you lose them.");

        final FormLayout form = new FormLayout();
        form.add(secretField("SIP realm",          created.getSipRealm()));
        form.add(secretField("TURN shared secret", created.getTurnSharedSecret()));

        final VerticalLayout body = new VerticalLayout(hint, form);
        body.setPadding(false);
        body.setSpacing(true);
        dlg.add(body);

        final Button done = new Button("Done", click -> {
            dlg.close();
            UI.getCurrent().navigate("societies");
        });
        done.addThemeVariants(ButtonVariant.LUMO_PRIMARY);
        dlg.getFooter().add(done);

        dlg.open();
    }

    private static HorizontalLayout secretField(String label, String value) {
        final TextField f = new TextField(label);
        f.setValue(value == null ? "" : value);
        f.setReadOnly(true);
        f.setWidthFull();

        final Button copy = new Button("Copy", click -> {
            final String v = (value == null) ? "" : value;
            UI.getCurrent().getPage().executeJs(
                    "navigator.clipboard && navigator.clipboard.writeText($0)", v);
            Notification.show(label + " copied", 2000, Notification.Position.BOTTOM_END);
        });
        copy.addThemeVariants(ButtonVariant.LUMO_TERTIARY);

        final HorizontalLayout row = new HorizontalLayout(f, copy);
        row.setDefaultVerticalComponentAlignment(FlexComponent.Alignment.END);
        row.setWidthFull();
        row.expand(f);
        return row;
    }
}
