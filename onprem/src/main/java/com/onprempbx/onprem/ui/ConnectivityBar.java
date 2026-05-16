package com.onprempbx.onprem.ui;

import com.onprempbx.onprem.service.StatusService;
import com.onprempbx.onprem.service.StatusService.Connectivity;
import com.onprempbx.onprem.service.StatusService.Peer;
import com.vaadin.flow.component.AttachEvent;
import com.vaadin.flow.component.DetachEvent;
import com.vaadin.flow.component.UI;
import com.vaadin.flow.component.html.Span;
import com.vaadin.flow.component.orderedlayout.FlexComponent;
import com.vaadin.flow.component.orderedlayout.HorizontalLayout;
import com.vaadin.flow.theme.lumo.LumoUtility;

import java.time.Duration;

/**
 * Inline status strip — lives in the {@link MainLayout} header
 * between the page title and the logout block, showing live cloud-
 * side health of the on-prem stack.
 *
 * <p>Four chips, refreshed every {@value #POLL_INTERVAL_SECONDS} s
 * via Vaadin's polling interval + {@link
 * UI#access(com.vaadin.flow.server.Command)}:
 *
 * <ul>
 *   <li><b>Agent</b> — {@code CloudTunnelEndpoint::has_agent()}</li>
 *   <li><b>DB tunnel</b> — {@code WsDbServer::is_connected()}</li>
 *   <li><b>Mongo</b> — derived from a wsdbagent → Mongo ping</li>
 *   <li><b>Asterisk</b> — placeholder (signal="pending"); will be
 *       live once the agent→cloud STATUS_REPORT frame is shipped</li>
 * </ul>
 *
 * <p>Polling stops on detach.
 */
public class ConnectivityBar extends HorizontalLayout {

    private static final int POLL_INTERVAL_SECONDS = 5;

    private final StatusService status;
    private final Span agentChip     = chip("Agent");
    private final Span wsdbagentChip = chip("DB");
    private final Span mongoChip     = chip("Mongo");
    private final Span asteriskChip  = chip("Asterisk");

    public ConnectivityBar(StatusService status) {
        this.status = status;
        setSpacing(true);
        setDefaultVerticalComponentAlignment(FlexComponent.Alignment.CENTER);
        addClassNames(LumoUtility.Padding.Horizontal.MEDIUM);
        add(agentChip, wsdbagentChip, mongoChip, asteriskChip);

        applyStatus(new Connectivity());   // start in "disconnected" state
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
        paintChip(agentChip,     "Agent",    c.agent);
        paintChip(wsdbagentChip, "DB",       c.wsdbagent);
        paintChip(mongoChip,     "Mongo",    c.mongo);
        paintChip(asteriskChip,  "Asterisk", c.asterisk);
    }

    private static Span chip(String label) {
        final Span s = new Span(label);
        s.getElement().getStyle()
            .set("padding",       "2px 10px")
            .set("border-radius", "9999px")
            .set("font-size",     "0.8em")
            .set("white-space",   "nowrap");
        return s;
    }

    /**
     * Repaint the chip. Three states: connected (green), disconnected
     * (red), pending — the cloud has no live signal for this peer yet
     * (grey "—"). The asterisk chip uses this until the agent→cloud
     * STATUS_REPORT plumbing lands.
     */
    private static void paintChip(Span chip, String label, Peer peer) {
        final boolean pending = peer != null && "pending".equals(peer.signal);
        final boolean connected = peer != null && peer.connected;
        final String dot = pending ? "—" : (connected ? "●" : "○");
        chip.setText(dot + " " + label);
        chip.getElement().getStyle()
            .set("background", pending ? "#eceff1"
                                       : (connected ? "#e0f7e9" : "#fde2e1"))
            .set("color",      pending ? "#6b7780"
                                       : (connected ? "#0a7a3b" : "#a3262c"));
    }
}
