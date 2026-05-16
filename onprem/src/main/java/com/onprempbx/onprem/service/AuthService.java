package com.onprempbx.onprem.service;

import com.fasterxml.jackson.annotation.JsonIgnoreProperties;
import com.onprempbx.onprem.config.BackendConfig;
import com.onprempbx.onprem.model.LoginCredentials;
import com.vaadin.flow.server.VaadinSession;
import org.springframework.http.HttpEntity;
import org.springframework.http.HttpHeaders;
import org.springframework.http.HttpStatus;
import org.springframework.http.HttpStatusCode;
import org.springframework.http.MediaType;
import org.springframework.http.ResponseEntity;
import org.springframework.stereotype.Service;
import org.springframework.web.client.HttpStatusCodeException;
import org.springframework.web.client.RestTemplate;

/**
 * Login + session for the admin UI.
 *
 * <p>State lives in {@link VaadinSession} attributes — one bag per
 * browser tab, evicted when the session expires. We deliberately avoid
 * a singleton or {@code @SessionScope} bean: VaadinSession is the
 * idiomatic store for per-user Vaadin app state and keeps the auth
 * token in the same lifecycle as the UI that uses it.
 */
@Service
public class AuthService {

    private static final String ATTR_TOKEN = "auth.token";
    private static final String ATTR_USER  = "auth.user";

    private final RestTemplate rest;
    private final BackendConfig backend;

    public AuthService(RestTemplate rest, BackendConfig backend) {
        this.rest = rest;
        this.backend = backend;
    }

    /**
     * POST {@code /api/v1/subscriber/login}; on success store the token
     * and the projected subscriber profile in the current VaadinSession.
     *
     * @throws AuthException on 401 (bad credentials) or 403 (disabled).
     */
    public CurrentUser login(LoginCredentials creds) {
        final String url = backend.getBackendUrl() + "/api/v1/subscriber/login";
        final HttpHeaders headers = new HttpHeaders();
        headers.setContentType(MediaType.APPLICATION_JSON);
        final HttpEntity<LoginCredentials> req = new HttpEntity<>(creds, headers);

        final ResponseEntity<LoginResponse> resp;
        try {
            resp = rest.postForEntity(url, req, LoginResponse.class);
        } catch (HttpStatusCodeException e) {
            final HttpStatusCode sc = e.getStatusCode();
            if (sc.value() == HttpStatus.UNAUTHORIZED.value()
                    || sc.value() == HttpStatus.FORBIDDEN.value()) {
                throw new AuthException(sc.value(), readableReason(sc), e);
            }
            throw new AuthException(sc.value(),
                    "Cloud login failed (HTTP " + sc.value() + ")", e);
        }

        final LoginResponse body = resp.getBody();
        if (body == null || body.token == null || body.token.isBlank()) {
            throw new AuthException(500, "Cloud returned an empty token");
        }

        final VaadinSession session = VaadinSession.getCurrent();
        if (session != null) {
            session.setAttribute(ATTR_TOKEN, body.token);
            session.setAttribute(ATTR_USER, body.subscriber);
        }
        return body.subscriber;
    }

    /** Bearer/session token minted by the cloud, or {@code null} when logged-out. */
    public String getToken() {
        final VaadinSession s = VaadinSession.getCurrent();
        return (s == null) ? null : (String) s.getAttribute(ATTR_TOKEN);
    }

    /** Projected login profile, or {@code null} when logged-out. */
    public CurrentUser getCurrentUser() {
        final VaadinSession s = VaadinSession.getCurrent();
        return (s == null) ? null : (CurrentUser) s.getAttribute(ATTR_USER);
    }

    /** {@code true} iff the logged-in user's role is {@code admin}. */
    public boolean isAdmin() {
        final CurrentUser u = getCurrentUser();
        return u != null && "admin".equalsIgnoreCase(u.role);
    }

    /** Drop the per-session token + profile attributes. */
    public void logout() {
        final VaadinSession s = VaadinSession.getCurrent();
        if (s == null) return;
        s.setAttribute(ATTR_TOKEN, null);
        s.setAttribute(ATTR_USER, null);
    }

    private static String readableReason(HttpStatusCode sc) {
        if (sc.value() == HttpStatus.UNAUTHORIZED.value()) return "Invalid credentials";
        if (sc.value() == HttpStatus.FORBIDDEN.value())    return "Account disabled";
        return "Login failed";
    }

    /**
     * Wire shape of the login response — {@code { token, subscriber }} —
     * see {@code finish_login} in modules/module/pbx/src/microservice_pbx.cpp.
     */
    @JsonIgnoreProperties(ignoreUnknown = true)
    static class LoginResponse {
        public String token;
        public CurrentUser subscriber;
    }

    /**
     * The login projection the cloud surfaces — intentionally narrower
     * than {@link com.onprempbx.onprem.model.Subscriber}; only the
     * fields the UI needs to render and authorize.
     */
    @JsonIgnoreProperties(ignoreUnknown = true)
    public static class CurrentUser {
        public String societyId;
        public String flatNumber;
        public String displayName;
        public String sipUser;
        public String role;
        public boolean autoAnswer;

        /**
         * Best-effort email used by {@link com.onprempbx.onprem.ui.MainLayout}
         * for the header label. The login projection omits email
         * server-side; we fall back to {@code displayName} (and then
         * {@code sipUser}) so the header always renders something.
         */
        public String displayLabel() {
            if (displayName != null && !displayName.isBlank()) return displayName;
            if (sipUser != null     && !sipUser.isBlank())     return sipUser;
            return "(unknown)";
        }
    }
}
