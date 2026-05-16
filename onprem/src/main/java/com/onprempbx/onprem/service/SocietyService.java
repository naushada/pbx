package com.onprempbx.onprem.service;

import com.onprempbx.onprem.config.BackendConfig;
import com.onprempbx.onprem.model.Society;
import org.springframework.http.HttpEntity;
import org.springframework.http.HttpHeaders;
import org.springframework.http.MediaType;
import org.springframework.http.ResponseEntity;
import org.springframework.stereotype.Service;
import org.springframework.web.client.RestTemplate;
import org.springframework.web.util.UriComponentsBuilder;

import java.util.Arrays;
import java.util.Collections;
import java.util.List;

/**
 * RestTemplate wrapper for the cloud's society endpoints.
 *
 * <p>Every outbound URL carries the session token as a {@code ?token=}
 * query param — the C++ cloud accepts either the cookie or the query
 * param; the latter is easier for a server-side HTTP client.
 */
@Service
public class SocietyService {

    private final RestTemplate rest;
    private final BackendConfig backend;
    private final AuthService auth;

    public SocietyService(RestTemplate rest, BackendConfig backend, AuthService auth) {
        this.rest = rest;
        this.backend = backend;
        this.auth = auth;
    }

    /** {@code GET /api/v1/societies} — list view; {@code turnSharedSecret} is stripped server-side. */
    public List<Society> list() {
        final String url = uri("/api/v1/societies");
        final ResponseEntity<Society[]> resp = rest.getForEntity(url, Society[].class);
        final Society[] body = resp.getBody();
        return (body == null) ? Collections.emptyList() : Arrays.asList(body);
    }

    /** {@code GET /api/v1/society/{id}} — detail view; includes secrets. */
    public Society get(String id) {
        final String url = uri("/api/v1/society/" + id);
        return rest.getForObject(url, Society.class);
    }

    /** {@code POST /api/v1/society} — cloud derives {@code sipRealm}/{@code turnSharedSecret} and echoes the new doc. */
    public Society create(Society society) {
        final String url = uri("/api/v1/society");
        final HttpHeaders h = new HttpHeaders();
        h.setContentType(MediaType.APPLICATION_JSON);
        final HttpEntity<Society> req = new HttpEntity<>(society, h);
        final ResponseEntity<Society> resp = rest.postForEntity(url, req, Society.class);
        return resp.getBody();
    }

    private String uri(String path) {
        final UriComponentsBuilder b = UriComponentsBuilder
                .fromHttpUrl(backend.getBackendUrl() + path);
        final String token = auth.getToken();
        if (token != null && !token.isBlank()) {
            b.queryParam("token", token);
        }
        return b.toUriString();
    }
}
