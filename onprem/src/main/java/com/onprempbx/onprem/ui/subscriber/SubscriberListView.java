package com.onprempbx.onprem.ui.subscriber;

import com.onprempbx.onprem.model.Subscriber;
import com.onprempbx.onprem.service.AuthService;
import com.onprempbx.onprem.service.SubscriberService;
import com.onprempbx.onprem.ui.MainLayout;
import com.vaadin.flow.component.UI;
import com.vaadin.flow.component.button.Button;
import com.vaadin.flow.component.button.ButtonVariant;
import com.vaadin.flow.component.confirmdialog.ConfirmDialog;
import com.vaadin.flow.component.grid.Grid;
import com.vaadin.flow.component.html.H2;
import com.vaadin.flow.component.notification.Notification;
import com.vaadin.flow.component.notification.NotificationVariant;
import com.vaadin.flow.component.orderedlayout.FlexComponent;
import com.vaadin.flow.component.orderedlayout.HorizontalLayout;
import com.vaadin.flow.component.orderedlayout.VerticalLayout;
import com.vaadin.flow.component.textfield.TextField;
import com.vaadin.flow.data.provider.ListDataProvider;
import com.vaadin.flow.data.value.ValueChangeMode;
import com.vaadin.flow.function.SerializablePredicate;
import com.vaadin.flow.router.PageTitle;
import com.vaadin.flow.router.Route;

import java.util.ArrayList;
import java.util.List;

/**
 * Admin's per-row subscriber control surface.
 *
 * <p>Lists every subscriber in the logged-in admin's society and offers
 * inline Disable/Enable + Delete actions (each confirmed via a
 * {@link ConfirmDialog}). The grid is backed by a
 * {@link ListDataProvider} whose {@link SerializablePredicate} the
 * "Filter by flat" {@link TextField} narrows — re-setting items on every
 * keystroke would clobber sort/selection state and force a full grid
 * re-render, whereas {@code ListDataProvider#setFilter} just re-applies
 * the predicate against the in-memory list.
 *
 * <p>Bulk import is item #13 of the plan; we only emit a router link
 * to {@code /subscribers/import} here so it lights up when that view
 * lands.
 */
@Route(value = "subscribers", layout = MainLayout.class)
@PageTitle("Subscribers — onprem-pbx admin")
public class SubscriberListView extends VerticalLayout {

    private final SubscriberService subscribers;
    private final String societyId;

    private final List<Subscriber> rows = new ArrayList<>();
    private final ListDataProvider<Subscriber> dataProvider =
            new ListDataProvider<>(rows);

    private final Grid<Subscriber> grid = new Grid<>(Subscriber.class, false);
    private final TextField flatFilter = new TextField();

    public SubscriberListView(AuthService auth, SubscriberService subscribers) {
        this.subscribers = subscribers;

        final AuthService.CurrentUser user = auth.getCurrentUser();
        this.societyId = (user == null) ? null : user.societyId;

        setSizeFull();
        setPadding(true);
        setSpacing(true);

        add(new H2("Subscribers"));
        add(buildHeaderBar());
        add(grid);
        configureGrid();

        if (societyId == null || societyId.isBlank()) {
            error("No society in session — please log in again.");
        } else {
            refresh();
        }
    }

    /* ---------------------------------------------------------------- */
    /* layout                                                            */
    /* ---------------------------------------------------------------- */

    private HorizontalLayout buildHeaderBar() {
        flatFilter.setPlaceholder("Filter by flat");
        flatFilter.setClearButtonVisible(true);
        flatFilter.setValueChangeMode(ValueChangeMode.LAZY);
        flatFilter.addValueChangeListener(e -> applyFlatFilter(e.getValue()));

        final Button refresh = new Button("Refresh", click -> refresh());

        // Item #13 lands at /subscribers/import. We navigate by route
        // name (rather than wrapping a RouterLink around the not-yet-
        // existent BulkSubscriberImportView class) so this view
        // compiles + ships ahead of #13.
        final Button bulkImportBtn = new Button("Bulk import",
                click -> UI.getCurrent().navigate("subscribers/import"));

        final HorizontalLayout bar = new HorizontalLayout(
                flatFilter, refresh, bulkImportBtn);
        bar.setDefaultVerticalComponentAlignment(FlexComponent.Alignment.END);
        bar.setWidthFull();

        return bar;
    }

    private void configureGrid() {
        grid.setSizeFull();
        grid.setDataProvider(dataProvider);

        grid.addColumn(Subscriber::getFlatNumber).setHeader("Flat").setAutoWidth(true);
        grid.addColumn(Subscriber::getName).setHeader("Name").setAutoWidth(true);
        grid.addColumn(Subscriber::getEmail).setHeader("Email").setAutoWidth(true);
        grid.addColumn(Subscriber::getRole).setHeader("Role").setAutoWidth(true);
        grid.addColumn(Subscriber::getStatus).setHeader("Status").setAutoWidth(true);
        grid.addColumn(Subscriber::getSipUsername).setHeader("SIP username").setAutoWidth(true);

        grid.addComponentColumn(this::buildActions)
                .setHeader("Actions")
                .setAutoWidth(true)
                .setFlexGrow(0);
    }

    private HorizontalLayout buildActions(Subscriber row) {
        final boolean disabled = "disabled".equalsIgnoreCase(row.getStatus());
        final String toggleLabel = disabled ? "Enable" : "Disable";
        final String nextStatus  = disabled ? "active" : "disabled";

        final Button toggle = new Button(toggleLabel,
                click -> confirmToggle(row, nextStatus));
        toggle.addThemeVariants(ButtonVariant.LUMO_TERTIARY);

        final Button delete = new Button("Delete", click -> confirmDelete(row));
        delete.addThemeVariants(ButtonVariant.LUMO_TERTIARY, ButtonVariant.LUMO_ERROR);

        final HorizontalLayout cell = new HorizontalLayout(toggle, delete);
        cell.setSpacing(true);
        return cell;
    }

    /* ---------------------------------------------------------------- */
    /* actions                                                           */
    /* ---------------------------------------------------------------- */

    private void confirmToggle(Subscriber row, String nextStatus) {
        final String verb = "active".equals(nextStatus) ? "Enable" : "Disable";
        final ConfirmDialog dlg = new ConfirmDialog();
        dlg.setHeader(verb + " subscriber");
        dlg.setText(verb + " " + describe(row) + "?");
        dlg.setCancelable(true);
        dlg.setConfirmText(verb);
        dlg.addConfirmListener(e -> doSetStatus(row, nextStatus));
        dlg.open();
    }

    private void doSetStatus(Subscriber row, String nextStatus) {
        try {
            subscribers.setStatus(societyId, row.getSipUsername(), nextStatus);
            Notification n = Notification.show(
                    describe(row) + " → " + nextStatus,
                    3000, Notification.Position.BOTTOM_START);
            n.addThemeVariants(NotificationVariant.LUMO_SUCCESS);
            refresh();
        } catch (RuntimeException ex) {
            error("Status update failed: " + ex.getMessage());
        }
    }

    private void confirmDelete(Subscriber row) {
        final ConfirmDialog dlg = new ConfirmDialog();
        dlg.setHeader("Delete subscriber");
        dlg.setText("This permanently removes the subscriber and drops any "
                + "live calls. Continue?");
        dlg.setCancelable(true);
        dlg.setConfirmText("Delete");
        dlg.setConfirmButtonTheme("error primary");
        dlg.addConfirmListener(e -> doDelete(row));
        dlg.open();
    }

    private void doDelete(Subscriber row) {
        try {
            subscribers.delete(societyId, row.getSipUsername());
            Notification n = Notification.show(
                    "Deleted " + describe(row),
                    3000, Notification.Position.BOTTOM_START);
            n.addThemeVariants(NotificationVariant.LUMO_SUCCESS);
            refresh();
        } catch (RuntimeException ex) {
            error("Delete failed: " + ex.getMessage());
        }
    }

    /* ---------------------------------------------------------------- */
    /* data + filter                                                     */
    /* ---------------------------------------------------------------- */

    private void refresh() {
        if (societyId == null || societyId.isBlank()) return;
        try {
            final List<Subscriber> fresh = subscribers.list(societyId);
            rows.clear();
            rows.addAll(fresh);
            dataProvider.refreshAll();
            applyFlatFilter(flatFilter.getValue());
        } catch (RuntimeException ex) {
            rows.clear();
            dataProvider.refreshAll();
            error("Failed to load subscribers: " + ex.getMessage());
        }
    }

    private void applyFlatFilter(String needle) {
        if (needle == null || needle.isBlank()) {
            dataProvider.clearFilters();
            return;
        }
        final String lc = needle.toLowerCase();
        final SerializablePredicate<Subscriber> pred = s -> {
            final String flat = s.getFlatNumber();
            return flat != null && flat.toLowerCase().contains(lc);
        };
        dataProvider.setFilter(pred);
    }

    /* ---------------------------------------------------------------- */
    /* helpers                                                           */
    /* ---------------------------------------------------------------- */

    private static String describe(Subscriber row) {
        final String flat = (row.getFlatNumber() == null) ? "?" : row.getFlatNumber();
        final String name = (row.getName() == null) ? "" : (" — " + row.getName());
        return "flat " + flat + name;
    }

    private void error(String msg) {
        Notification n = Notification.show(msg, 5000, Notification.Position.BOTTOM_START);
        n.addThemeVariants(NotificationVariant.LUMO_ERROR);
    }
}
