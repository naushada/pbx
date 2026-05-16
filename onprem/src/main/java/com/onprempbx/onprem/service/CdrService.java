package com.onprempbx.onprem.service;

import com.onprempbx.onprem.config.BackendConfig;
import org.springframework.core.ParameterizedTypeReference;
import org.springframework.http.HttpEntity;
import org.springframework.http.HttpMethod;
import org.springframework.http.ResponseEntity;
import org.springframework.stereotype.Service;
import org.springframework.web.client.RestTemplate;
import org.springframework.web.util.UriComponentsBuilder;

import java.util.Collections;
import java.util.List;
import java.util.Map;

/**
 * RestTemplate wrapper for the cloud's CDR endpoint.
 *
 * <p>The cloud surface is single-method ({@code GET /api/v1/cdr?societyId=…})
 * and the documents are schema-light — the UI only needs the row count
 * (and {@code startedAt} for the "last 24h" filter) — so we keep the
 * rows as raw {@code Map<String, Object>} instead of minting a model
 * class that no other call-site would ever consume. Sibling services
 * ({@link SocietyService}, {@link SubscriberService}) use typed POJOs
 * because their fields drive real grids; the dashboard tile here is
 * count-only.
 */
@Service
public class CdrService {

    private final RestTemplate rest;
    private final BackendConfig backend;
    private final AuthService auth;

    public CdrService(RestTemplate rest, BackendConfig backend, AuthService auth) {
        this.rest = rest;
        this.backend = backend;
        this.auth = auth;
    }

    /**
     * {@code GET /api/v1/cdr?societyId=…} — full CDR list for one
     * society. The cloud returns {@code []} with HTTP 200 when no DB
     * is configured or when no rows match, so callers never need to
     * special-case the empty path.
     */
    public List<Map<String, Object>> list(String societyId) {
        final String url = uri("/api/v1/cdr", Map.of("societyId", societyId));
        final ResponseEntity<List<Map<String, Object>>> resp = rest.exchange(
                url,
                HttpMethod.GET,
                HttpEntity.EMPTY,
                new ParameterizedTypeReference<List<Map<String, Object>>>() {});
        final List<Map<String, Object>> body = resp.getBody();
        return (body == null) ? Collections.emptyList() : body;
    }

    private String uri(String path, Map<String, String> extraQuery) {
        final UriComponentsBuilder b = UriComponentsBuilder
                .fromHttpUrl(backend.getBackendUrl() + path);
        for (Map.Entry<String, String> e : extraQuery.entrySet()) {
            b.queryParam(e.getKey(), e.getValue());
        }
        final String token = auth.getToken();
        if (token != null && !token.isBlank()) {
            b.queryParam("token", token);
        }
        return b.toUriString();
    }
}
