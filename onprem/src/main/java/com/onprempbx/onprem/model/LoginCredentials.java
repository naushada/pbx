package com.onprempbx.onprem.model;

/**
 * Form binding for {@link com.onprempbx.onprem.ui.LoginView}, also the
 * JSON body posted to {@code POST /api/v1/subscriber/login}.
 *
 * <p>Field names match the cloud's required keys verbatim
 * (see {@code handle_subscriber_login_POST}). Don't rename them
 * without updating the cloud handler.
 */
public class LoginCredentials {

    private String societyCode;
    private String flatNumber;
    private String password;

    public LoginCredentials() {
    }

    public LoginCredentials(String societyCode, String flatNumber, String password) {
        this.societyCode = societyCode;
        this.flatNumber = flatNumber;
        this.password = password;
    }

    public String getSocietyCode() {
        return societyCode;
    }

    public void setSocietyCode(String societyCode) {
        this.societyCode = societyCode;
    }

    public String getFlatNumber() {
        return flatNumber;
    }

    public void setFlatNumber(String flatNumber) {
        this.flatNumber = flatNumber;
    }

    public String getPassword() {
        return password;
    }

    public void setPassword(String password) {
        this.password = password;
    }
}
