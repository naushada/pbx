package com.onprempbx.onprem;

import org.springframework.boot.SpringApplication;
import org.springframework.boot.autoconfigure.SpringBootApplication;

/**
 * Boot entry-point for the onprem-pbx admin UI.
 *
 * <p>The app is a stateless Vaadin 24 shell whose only outbound dependency
 * is the C++ pbx-cloud REST API (see {@code application.properties} →
 * {@code backend.url}). All persistence — sessions, societies, subscribers
 * — lives in the cloud; this process holds nothing but per-tab Vaadin
 * session attributes for the auth token.
 */
@SpringBootApplication
public class Application {
    public static void main(String[] args) {
        SpringApplication.run(Application.class, args);
    }
}
