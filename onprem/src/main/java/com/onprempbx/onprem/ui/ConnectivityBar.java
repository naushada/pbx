package com.onprempbx.onprem.ui;

import com.onprempbx.onprem.service.StatusService;
import com.onprempbx.onprem.service.StatusService.Connectivity;
import com.vaadin.flow.component.AttachEvent;
import com.vaadin.flow.component.DetachEvent;
import com.vaadin.flow.component.UI;
import com.vaadin.flow.component.html.Span;
import com.vaadin.flow.component.orderedlayout.FlexComponent;
import com.vaadin.flow.component.orderedlayout.HorizontalLayout;
import com.vaadin.flow.theme.lumo.LumoUtility;

import java.time.Duration;

/**
 * Sub-navbar strip showing live cloud-side health of the two on-prem
 * peers: {@code pbx-agent} (SIP tunnel) and {@code pbx-wsdbagent}
 * (DB tunnel).
 *
 * <p>One {@link Span} chip per peer, refreshed every {@value
 * #POLL_INTERVAL_SECONDS} seconds via Vaadin's polling interval +
 * {@link UI#access(com.vaadin.flow.server.Command)} so the chip can
 * update on the server-side timer without a browser action. The
 * polling stops when the component detaches.
 *
 * <p>Chip colour: green = connected, red = disconnected. We don't show
 * a separate "unknown" state — {@link StatusService} translates every
 * error path to a connected=false response so the safer-looking red
 * always reflects "we can't see green truth."
 */
public class ConnectivityBar extends HorizontalLayout {

    private static final int POLL_INTERVAL_SECONDS = 5;

    private final StatusService status;
    private final Span agentChip      = chip("Agent");
    private final Span wsdbagentChip  = chip("DB tunnel");

    public ConnectivityBar(StatusService status) {
        this.status = status;
        setSpacing(true);
        setDefaultVerticalComponentAlignment(FlexComponent.Alignment.CENTER);
        addClassNames(LumoUtility.Padding.Horizontal.MEDIUM,
                       LumoUtility.Padding.Vertical.XSMALL,
                       LumoUtility.Background.CONTRAST_5);
        setWidthFull();

        final Span label = new Span("On-prem stack:");
        label.addClassNames(LumoUtility.FontSize.SMALL,
                             LumoUtility.TextColor.SECONDARY);
        add(label, agentChip, wsdbagentChip);

        applyStatus(new Connectivity());   // start "disconnected" until first poll
    }

    @Override
    protected void onAttach(AttachEvent attachEvent) {
        super.onAttach(attachEvent);
        // Drive a fresh poll on attach so the chip isn't stale-red for
        // POLL_INTERVAL_SECONDS after navigation.
        refresh();
        attachEvent.getUI().setPollInterval(
                (int) Duration.ofSeconds(POLL_INTERVAL_SECONDS).toMillis());
        attachEvent.getUI().addPollListener(e -> refresh());
    }

    @Override
    protected void onDetach(DetachEvent detachEvent) {
        // Stop polling when the bar leaves the DOM (e.g. user logs out).
        detachEvent.getUI().setPollInterval(-1);
        super.onDetach(detachEvent);
    }

    /** Re-fetch + update chips on the UI thread. */
    private void refresh() {
        final UI ui = UI.getCurrent();
        if (ui == null) return;
        final Connectivity c = status.check();
        ui.access(() -> applyStatus(c));
    }

    private void applyStatus(Connectivity c) {
        paintChip(agentChip,     "Agent",     c.agent.connected);
        paintChip(wsdbagentChip, "DB tunnel", c.wsdbagent.connected);
    }

    private static Span chip(String label) {
        final Span s = new Span(label);
        s.getElement().getStyle()
            .set("padding",       "2px 8px")
            .set("border-radius", "9999px")
            .set("font-size",     "0.75em");
        return s;
    }

    /**
     * Repaint the chip's bullet + color. We rebuild the text from the
     * label every call so the dot stays in sync with the on/off state.
     */
    private static void paintChip(Span chip, String label, boolean connected) {
        final String dot = connected ? "●" : "○";
        chip.setText(dot + " " + label);
        chip.getElement().getStyle()
            .set("background", connected ? "#e0f7e9" : "#fde2e1")
            .set("color",      connected ? "#0a7a3b" : "#a3262c");
    }
}
