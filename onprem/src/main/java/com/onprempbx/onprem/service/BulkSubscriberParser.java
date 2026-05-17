package com.onprempbx.onprem.service;

import com.onprempbx.onprem.config.BackendConfig;
import org.springframework.http.HttpEntity;
import org.springframework.http.HttpHeaders;
import org.springframework.http.MediaType;
import org.springframework.http.ResponseEntity;
import org.springframework.stereotype.Service;
import org.springframework.web.client.RestTemplate;
import org.springframework.web.util.UriComponentsBuilder;

import java.util.Map;

/**
 * Bulk-import of subscribers via the cloud's CSV endpoint.
 *
 * <p>POSTs the CSV body to {@code /api/v1/subscriber/import?societyId=…}
 * and returns the raw response body (CSV of per-row outcomes). The cloud
 * also sets {@code X-Import-Created} / {@code X-Import-Skipped} headers,
 * which the upload view (item #13) will lift into a {@link
 * com.onprempbx.onprem.model.BulkResult}.
 *
 * <p>Parsing the response into typed rows is intentionally deferred to
 * item #13 — this service exists now so the service layer is complete
 * and the view PR can land without touching this scaffold.
 */
@Service
public class BulkSubscriberParser {

    private final RestTemplate rest;
    private final BackendConfig backend;
    private final AuthService auth;

    public BulkSubscriberParser(RestTemplate rest, BackendConfig backend, AuthService auth) {
        this.rest = rest;
        this.backend = backend;
        this.auth = auth;
    }

    /**
     * POST a CSV body to the cloud's bulk-import endpoint.
     *
     * @param societyId the {@code _id} of the society the rows belong to
     * @param csvBody   the CSV as-typed (header
     *                  {@code flat_number,name,email,phone,role} +
     *                  data rows)
     * @return the response CSV (one row per input row, with credentials
     *         or {@code skipped})
     */
    public String importSubscribers(String societyId, String csvBody) {
        final String url = uri("/api/v1/subscriber/import",
                Map.of("societyId", societyId));
        final HttpHeaders h = new HttpHeaders();
        h.setContentType(new MediaType("text", "csv"));
        final HttpEntity<String> req = new HttpEntity<>(csvBody, h);
        final ResponseEntity<String> resp = rest.postForEntity(url, req, String.class);
        return resp.getBody();
    }

    /**
     * {@code GET /api/v1/subscriber/import/template} — canonical CSV
     * header the operator should fill in. Returned as {@code byte[]} so
     * the Vaadin {@code DownloadHandler} can stream it without
     * re-encoding.
     */
    public byte[] downloadTemplate() {
        final String url = uri("/api/v1/subscriber/import/template", Map.of());
        final ResponseEntity<byte[]> resp = rest.getForEntity(url, byte[].class);
        return resp.getBody();
    }

    /**
     * {@code GET /api/v1/subscriber/import/template.xlsx} — pre-formatted
     * Excel template with the three mandatory columns highlighted in
     * YELLOW so the operator can't miss them. Source of truth:
     * {@code scripts/generate-subscriber-template.py} →
     * {@code docs/subscribers-template.xlsx}, served as-is by the cloud.
     */
    public byte[] downloadTemplateXlsx() {
        final String url = uri("/api/v1/subscriber/import/template.xlsx", Map.of());
        final ResponseEntity<byte[]> resp = rest.getForEntity(url, byte[].class);
        return resp.getBody();
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
