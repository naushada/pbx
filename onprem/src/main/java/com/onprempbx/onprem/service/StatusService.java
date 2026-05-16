package com.onprempbx.onprem.service;

import com.fasterxml.jackson.annotation.JsonIgnoreProperties;
import com.onprempbx.onprem.config.BackendConfig;
import org.springframework.http.client.SimpleClientHttpRequestFactory;
import org.springframework.stereotype.Service;
import org.springframework.web.client.RestTemplate;
import org.springframework.web.util.UriComponentsBuilder;

/**
 * Polls the cloud's {@code GET /api/v1/admin/connectivity} for live
 * on-prem peer status and exposes a small DTO the {@link
 * com.onprempbx.onprem.ui.ConnectivityBar} renders as chips.
 *
 * <p>Uses a dedicated {@link RestTemplate} with short timeouts so a
 * slow-or-dead cloud doesn't stall the whole UI render thread —
 * worst-case the chip flips to "unknown" for one poll cycle.
 */
@Service
public class StatusService {

    /**
     * Mirrors the cloud's response shape. Booleans default to false
     * on any error path so the UI always renders the safer
     * "disconnected" state — no silent green when reality is red.
     */
    @JsonIgnoreProperties(ignoreUnknown = true)
    public static class Connectivity {
        public Peer agent     = new Peer();
        public Peer wsdbagent = new Peer();
    }

    @JsonIgnoreProperties(ignoreUnknown = true)
    public static class Peer {
        public boolean connected;
    }

    private final RestTemplate probeTemplate;
    private final BackendConfig backend;
    private final AuthService auth;

    public StatusService(RestTemplate restTemplate, BackendConfig backend,
                          AuthService auth) {
        this.backend = backend;
        this.auth    = auth;
        // Independent RestTemplate with tight timeouts. The shared one
        // (used for admin actions) is fine with default 30s timeouts;
        // for a UI-poll we want fail-fast.
        final SimpleClientHttpRequestFactory f = new SimpleClientHttpRequestFactory();
        f.setConnectTimeout(2_000);
        f.setReadTimeout   (3_000);
        this.probeTemplate = new RestTemplate(f);
        this.probeTemplate.setInterceptors(restTemplate.getInterceptors());
    }

    /**
     * One-shot poll. Returns a fully-disconnected status on any error
     * (timeout, 401 because the admin session expired, 5xx, etc.) so
     * the bar reflects "we can't see the cloud" cleanly.
     */
    public Connectivity check() {
        final String token = auth.getToken();
        if (token == null || token.isBlank()) return new Connectivity();
        final String url = UriComponentsBuilder
                .fromHttpUrl(backend.getBackendUrl() + "/api/v1/admin/connectivity")
                .queryParam("token", token)
                .toUriString();
        try {
            final Connectivity body = probeTemplate.getForObject(url, Connectivity.class);
            return body != null ? body : new Connectivity();
        } catch (Exception e) {
            return new Connectivity();
        }
    }
}
