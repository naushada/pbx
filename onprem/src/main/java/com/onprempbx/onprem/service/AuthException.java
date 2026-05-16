package com.onprempbx.onprem.service;

/**
 * Thrown by {@link AuthService#login} on a 401 or 403 from the cloud.
 * Carries the HTTP status so the LoginView can distinguish "Invalid
 * credentials" (401) from "Account disabled" (403).
 */
public class AuthException extends RuntimeException {

    private final int statusCode;

    public AuthException(int statusCode, String message) {
        super(message);
        this.statusCode = statusCode;
    }

    public AuthException(int statusCode, String message, Throwable cause) {
        super(message, cause);
        this.statusCode = statusCode;
    }

    public int getStatusCode() {
        return statusCode;
    }
}
