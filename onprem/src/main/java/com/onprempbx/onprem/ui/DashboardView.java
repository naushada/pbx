package com.onprempbx.onprem.ui;

import com.onprempbx.onprem.model.Subscriber;
import com.onprempbx.onprem.service.AuthService;
import com.onprempbx.onprem.service.CdrService;
import com.onprempbx.onprem.service.SubscriberService;
import com.vaadin.flow.component.html.H2;
import com.vaadin.flow.component.html.Paragraph;
import com.vaadin.flow.component.html.Span;
import com.vaadin.flow.component.notification.Notification;
import com.vaadin.flow.component.orderedlayout.FlexComponent;
import com.vaadin.flow.component.orderedlayout.HorizontalLayout;
import com.vaadin.flow.component.orderedlayout.VerticalLayout;
import com.vaadin.flow.router.PageTitle;
import com.vaadin.flow.router.Route;
import com.vaadin.flow.theme.lumo.LumoUtility;

import java.time.Instant;
import java.util.Collections;
import java.util.List;
import java.util.Map;

/**
 * Society at-a-glance — three count tiles for the logged-in admin's
 * own society. Item #10 of the {@code project_vaadin_admin_ui} plan.
 *
 * <p>Failure posture: every backend call is wrapped — on any
 * {@link RuntimeException} the tile falls back to a {@code 0}
 * placeholder and a Vaadin {@link Notification} surfaces the cause.
 * The cloud already returns {@code [] + 200} when the DB is offline
 * (see {@code handle_admin_subscribers_GET} / {@code handle_cdr_GET}),
 * so the empty path renders naturally as {@code 0}.
 *
 * <p>Online-presence gap: the cloud's
 * {@code GET /api/v1/admin/subscribers} projection strips secrets but
 * does not carry the {@code online} bit — presence lives in the
 * agent-side {@code IPresenceCache} and is only surfaced today via the
 * resident-directory endpoint. The middle tile renders {@code —} with
 * a footnote until a later PR teaches the admin endpoint to join the
 * presence cache (or exposes a {@code /api/v1/admin/presence} sibling).
 */
@Route(value = "dashboard", layout = MainLayout.class)
@PageTitle("Dashboard — onprem-pbx admin")
public class DashboardView extends VerticalLayout {

    /** Window for the "Recent CDR" tile — anything with {@code startedAt} in the last 24h. */
    private static final long CDR_WINDOW_SECONDS = 24L * 60L * 60L;

    public DashboardView(AuthService auth,
                         SubscriberService subscribers,
                         CdrService cdrs) {
        setPadding(true);
        setSpacing(true);

        final AuthService.CurrentUser u = auth.getCurrentUser();
        final String societyId = (u == null) ? null : u.societyId;

        add(new H2("Society overview"));

        final List<Subscriber> roster   = safeRoster(subscribers, societyId);
        final long activeCount          = countActive(roster);
        final long recentCdrCount       = safeRecentCdrCount(cdrs, societyId);

        final HorizontalLayout row = new HorizontalLayout(
                tile(Long.toString(activeCount), "active subscribers", null),
                tile("—",                        "online now",
                     "(presence wired in a later PR — backend has IPresenceCache, "
                     + "admin GET doesn't carry it yet)"),
                tile(Long.toString(recentCdrCount), "calls last 24h", null));
        row.setSpacing(true);
        row.setWidthFull();
        add(row);
    }

    // ── Tile factory ────────────────────────────────────────────────────────

    /**
     * One stat tile: big number, small label, optional footnote. Built
     * from plain Vaadin layout + Lumo utility classes so we don't pull
     * in a new component dependency for the dashboard.
     */
    private static VerticalLayout tile(String bigNumber, String label, String footnote) {
        final Span number = new Span(bigNumber);
        number.addClassNames(
                LumoUtility.FontSize.XXXLARGE,
                LumoUtility.FontWeight.BOLD);

        final Span caption = new Span(label);
        caption.addClassNames(
                LumoUtility.FontSize.SMALL,
                LumoUtility.TextColor.SECONDARY);

        final VerticalLayout tile = new VerticalLayout(number, caption);
        tile.setPadding(true);
        tile.setSpacing(false);
        tile.setAlignItems(FlexComponent.Alignment.START);
        tile.addClassNames(
                LumoUtility.Background.CONTRAST_5,
                LumoUtility.BorderRadius.MEDIUM,
                LumoUtility.Padding.MEDIUM);
        tile.setWidth("16em");

        if (footnote != null && !footnote.isBlank()) {
            final Paragraph note = new Paragraph(footnote);
            note.addClassNames(
                    LumoUtility.FontSize.XSMALL,
                    LumoUtility.TextColor.TERTIARY,
                    LumoUtility.Margin.Top.SMALL);
            tile.add(note);
        }
        return tile;
    }

    // ── Backend calls (every one falls back to an empty/zero result) ────────

    private static List<Subscriber> safeRoster(SubscriberService subscribers, String societyId) {
        if (societyId == null || societyId.isBlank()) return Collections.emptyList();
        try {
            return subscribers.list(societyId);
        } catch (RuntimeException e) {
            Notification.show("Could not load subscribers: " + e.getMessage());
            return Collections.emptyList();
        }
    }

    private static long countActive(List<Subscriber> roster) {
        return roster.stream()
                .filter(s -> "active".equalsIgnoreCase(s.getStatus()))
                .count();
    }

    private long safeRecentCdrCount(CdrService cdrs, String societyId) {
        if (societyId == null || societyId.isBlank()) return 0L;
        final List<Map<String, Object>> rows;
        try {
            rows = cdrs.list(societyId);
        } catch (RuntimeException e) {
            Notification.show("Could not load CDR: " + e.getMessage());
            return 0L;
        }
        final long cutoff = Instant.now().getEpochSecond() - CDR_WINDOW_SECONDS;
        return rows.stream()
                .filter(r -> startedAtSeconds(r) >= cutoff)
                .count();
    }

    /**
     * CDR documents carry {@code startedAt} as a unix-epoch seconds
     * {@code int64} (see {@code ari_client.cpp}). Jackson surfaces it
     * as {@link Number} when deserializing into {@code Map<String,
     * Object>}; missing/malformed values fall back to {@code 0} which
     * a sane 24h cutoff treats as "too old to count".
     */
    private static long startedAtSeconds(Map<String, Object> row) {
        final Object raw = row.get("startedAt");
        if (raw instanceof Number n) return n.longValue();
        if (raw instanceof String s) {
            try {
                return Long.parseLong(s.trim());
            } catch (NumberFormatException ignored) {
                return 0L;
            }
        }
        return 0L;
    }
}
