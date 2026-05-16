package com.onprempbx.onprem.service;

import com.onprempbx.onprem.config.BackendConfig;
import com.onprempbx.onprem.model.Subscriber;
import org.springframework.http.HttpEntity;
import org.springframework.http.HttpHeaders;
import org.springframework.http.HttpMethod;
import org.springframework.http.MediaType;
import org.springframework.http.ResponseEntity;
import org.springframework.stereotype.Service;
import org.springframework.web.client.RestTemplate;
import org.springframework.web.util.UriComponentsBuilder;

import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * RestTemplate wrapper for the cloud's admin-scoped subscriber endpoints.
 *
 * <p>Every call passes the session token via {@code ?token=}. The
 * {@code societyId} query param is required by the cloud on the
 * admin-list, status-update, and delete handlers — the cloud rejects
 * cross-society writes by checking the path's {@code sipUsername}
 * against the {@code (societyId, sipUsername)} pair.
 */
@Service
public class SubscriberService {

    private final RestTemplate rest;
    private final BackendConfig backend;
    private final AuthService auth;

    public SubscriberService(RestTemplate rest, BackendConfig backend, AuthService auth) {
        this.rest = rest;
        this.backend = backend;
        this.auth = auth;
    }

    /** {@code GET /api/v1/admin/subscribers?societyId=…} */
    public List<Subscriber> list(String societyId) {
        final String url = uri("/api/v1/admin/subscribers", Map.of("societyId", societyId));
        final ResponseEntity<Subscriber[]> resp = rest.getForEntity(url, Subscriber[].class);
        final Subscriber[] body = resp.getBody();
        return (body == null) ? Collections.emptyList() : Arrays.asList(body);
    }

    /**
     * {@code PUT /api/v1/subscriber/{sipUsername}?societyId=…} with
     * {@code {"status": "active"|"disabled"}}.
     */
    public void setStatus(String societyId, String sipUsername, String status) {
        final String url = uri("/api/v1/subscriber/" + sipUsername,
                Map.of("societyId", societyId));
        final HttpHeaders h = new HttpHeaders();
        h.setContentType(MediaType.APPLICATION_JSON);
        final Map<String, String> body = new HashMap<>();
        body.put("status", status);
        final HttpEntity<Map<String, String>> req = new HttpEntity<>(body, h);
        rest.exchange(url, HttpMethod.PUT, req, Void.class);
    }

    /** {@code DELETE /api/v1/subscriber/{sipUsername}?societyId=…} */
    public void delete(String societyId, String sipUsername) {
        final String url = uri("/api/v1/subscriber/" + sipUsername,
                Map.of("societyId", societyId));
        rest.exchange(url, HttpMethod.DELETE, HttpEntity.EMPTY, Void.class);
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
