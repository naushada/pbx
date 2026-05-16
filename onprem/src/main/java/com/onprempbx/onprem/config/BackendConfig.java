package com.onprempbx.onprem.config;

import org.springframework.beans.factory.annotation.Value;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Configuration;
import org.springframework.web.client.RestTemplate;

/**
 * Backend connectivity for the admin UI.
 *
 * <p>Exposes:
 * <ul>
 *   <li>a singleton {@link RestTemplate} used by every {@code service.*}
 *       class for outbound HTTP to the cloud; and
 *   <li>{@link #getBackendUrl()} — the cloud base URL, resolved from
 *       the {@code backend.url} property (override via env / CLI to
 *       point a dev UI at a staging cloud).
 * </ul>
 */
@Configuration
public class BackendConfig {

    @Value("${backend.url}")
    private String backendUrl;

    @Bean
    public RestTemplate restTemplate() {
        return new RestTemplate();
    }

    /**
     * Base URL of the C++ pbx-cloud HTTP API, e.g. {@code http://localhost:8080}.
     * Service classes append the path (e.g. {@code /api/v1/societies}).
     */
    public String getBackendUrl() {
        return backendUrl;
    }
}
