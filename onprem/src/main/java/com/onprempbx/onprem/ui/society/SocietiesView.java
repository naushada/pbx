package com.onprempbx.onprem.ui.society;

import com.onprempbx.onprem.model.Society;
import com.onprempbx.onprem.service.SocietyService;
import com.onprempbx.onprem.ui.MainLayout;
import com.vaadin.flow.component.UI;
import com.vaadin.flow.component.button.Button;
import com.vaadin.flow.component.button.ButtonVariant;
import com.vaadin.flow.component.dialog.Dialog;
import com.vaadin.flow.component.formlayout.FormLayout;
import com.vaadin.flow.component.grid.Grid;
import com.vaadin.flow.component.html.H2;
import com.vaadin.flow.component.notification.Notification;
import com.vaadin.flow.component.notification.NotificationVariant;
import com.vaadin.flow.component.orderedlayout.FlexComponent;
import com.vaadin.flow.component.orderedlayout.HorizontalLayout;
import com.vaadin.flow.component.orderedlayout.VerticalLayout;
import com.vaadin.flow.component.textfield.TextArea;
import com.vaadin.flow.component.textfield.TextField;
import com.vaadin.flow.router.PageTitle;
import com.vaadin.flow.router.Route;

import java.util.List;

/**
 * Lists societies the admin can administer and pops a per-row detail
 * dialog that surfaces the {@code sipRealm} + {@code turnSharedSecret}
 * — the values the operator must paste into the on-prem agent's
 * {@code .env}.
 *
 * <p>Item #11 of the Vaadin admin UI plan; backed by
 * {@link SocietyService#list()} and {@link SocietyService#get(String)}.
 */
@Route(value = "societies", layout = MainLayout.class)
@PageTitle("Societies — onprem-pbx admin")
public class SocietiesView extends VerticalLayout {

    private final SocietyService societies;
    private final Grid<Society> grid = new Grid<>(Society.class, false);

    public SocietiesView(SocietyService societies) {
        this.societies = societies;

        setSizeFull();
        setPadding(true);
        setSpacing(true);

        add(buildHeader());
        add(buildGrid());

        refresh();
    }

    private HorizontalLayout buildHeader() {
        final H2 title = new H2("Societies");

        final Button create = new Button("Create society",
                click -> UI.getCurrent().navigate("societies/new"));
        create.addThemeVariants(ButtonVariant.LUMO_PRIMARY);

        final HorizontalLayout header = new HorizontalLayout(title, create);
        header.setDefaultVerticalComponentAlignment(FlexComponent.Alignment.CENTER);
        header.setWidthFull();
        header.expand(title);
        return header;
    }

    private Grid<Society> buildGrid() {
        grid.addColumn(Society::getCode).setHeader("Code").setAutoWidth(true);
        grid.addColumn(Society::getName).setHeader("Name").setAutoWidth(true);
        grid.addColumn(Society::getAddress).setHeader("Address").setFlexGrow(1);
        grid.addColumn(Society::getSipRealm).setHeader("SIP realm").setAutoWidth(true);

        grid.addComponentColumn(s -> {
            final Button view = new Button("View details", click -> openDetails(s));
            view.addThemeVariants(ButtonVariant.LUMO_TERTIARY);
            return view;
        }).setHeader("").setAutoWidth(true);

        grid.setSizeFull();
        return grid;
    }

    private void refresh() {
        try {
            final List<Society> rows = societies.list();
            grid.setItems(rows);
        } catch (RuntimeException e) {
            final Notification n = Notification.show(
                    "Failed to load societies: " + e.getMessage(),
                    5000, Notification.Position.TOP_CENTER);
            n.addThemeVariants(NotificationVariant.LUMO_ERROR);
        }
    }

    /**
     * Re-fetches the society via {@link SocietyService#get(String)} so
     * the dialog always has the secret-bearing detail payload (the list
     * endpoint strips {@code turnSharedSecret}).
     */
    private void openDetails(Society row) {
        final Society full;
        try {
            full = societies.get(row.getId());
        } catch (RuntimeException e) {
            final Notification n = Notification.show(
                    "Failed to load society: " + e.getMessage(),
                    5000, Notification.Position.TOP_CENTER);
            n.addThemeVariants(NotificationVariant.LUMO_ERROR);
            return;
        }
        if (full == null) {
            Notification.show("Society no longer exists",
                    5000, Notification.Position.TOP_CENTER);
            return;
        }

        final Dialog dlg = new Dialog();
        dlg.setHeaderTitle("Society — " + full.getCode());
        dlg.setWidth("520px");

        final FormLayout form = new FormLayout();
        form.add(readOnlyField("Code",       full.getCode()));
        form.add(readOnlyField("Name",       full.getName()));
        form.add(readOnlyArea("Address",     full.getAddress()));
        form.add(secretField("SIP realm",          full.getSipRealm()));
        form.add(secretField("TURN shared secret", full.getTurnSharedSecret()));

        dlg.add(form);

        final Button close = new Button("Close", click -> dlg.close());
        dlg.getFooter().add(close);

        dlg.open();
    }

    private static TextField readOnlyField(String label, String value) {
        final TextField f = new TextField(label);
        f.setValue(value == null ? "" : value);
        f.setReadOnly(true);
        f.setWidthFull();
        return f;
    }

    private static TextArea readOnlyArea(String label, String value) {
        final TextArea a = new TextArea(label);
        a.setValue(value == null ? "" : value);
        a.setReadOnly(true);
        a.setWidthFull();
        return a;
    }

    /**
     * Read-only field paired with a Copy button — the dialog's primary
     * job is to let the operator copy {@code sipRealm} +
     * {@code turnSharedSecret} into the on-prem {@code .env}.
     */
    private static HorizontalLayout secretField(String label, String value) {
        final TextField f = readOnlyField(label, value);
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
