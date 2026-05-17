package com.onprempbx.onprem.config;

import org.springframework.beans.factory.annotation.Value;
import org.springframework.context.annotation.Configuration;

/**
 * Optional dev/operator convenience — when all three of {@code
 * admin.auto.user} (flat number, typically {@code ADMIN}), {@code
 * admin.auto.password}, and {@code admin.auto.society} (society code)
 * are set, {@link com.onprempbx.onprem.ui.LoginView} submits the login
 * automatically and forwards to {@code /dashboard} without the
 * operator typing anything.
 *
 * <p>Default OFF — empty defaults on every property mean the form
 * shows normally unless the operator opts in via env vars or
 * {@code --admin.auto.*=…} CLI flags.
 *
 * <p>Cloud-side validation is unchanged: a bad auto-login just falls
 * through to the manual form with the cloud's error notification, same
 * as a manual login.
 *
 * <p>Security: the credentials live in the JVM process env. Treat
 * them like any other secret — don't enable on a shared dev box;
 * don't bake into a container image.
 */
@Configuration
public class AutoLoginConfig {

    @Value("${admin.auto.user:}")
    private String user;

    @Value("${admin.auto.password:}")
    private String password;

    @Value("${admin.auto.society:}")
    private String society;

    /** {@code true} iff all three credential fields are non-blank. */
    public boolean isConfigured() {
        return notBlank(user) && notBlank(password) && notBlank(society);
    }

    public String getUser()     { return user; }
    public String getPassword() { return password; }
    public String getSociety()  { return society; }

    private static boolean notBlank(String s) {
        return s != null && !s.isBlank();
    }
}
