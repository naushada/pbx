package com.onprempbx.onprem.model;

import com.fasterxml.jackson.annotation.JsonInclude;
import com.fasterxml.jackson.annotation.JsonProperty;

/**
 * Mirrors a row from {@code GET /api/v1/admin/subscribers?societyId=…}.
 *
 * <p>The cloud strips {@code portalPasswordHash} and {@code sipHa1} from
 * each row server-side (see {@code handle_admin_subscribers_GET}); every
 * other field is preserved, hence the wide set here. {@code role} is one
 * of {@code resident|guard|admin}; {@code status} is {@code active} or
 * {@code disabled}.
 */
@JsonInclude(JsonInclude.Include.NON_NULL)
public class Subscriber {

    @JsonProperty("_id")
    private String id;

    private String societyId;
    private String flatNumber;
    private String flatId;
    private String name;
    private String email;
    private String phone;
    private String role;
    private String status;
    private String sipUsername;
    private boolean autoAnswer;

    public Subscriber() {
    }

    public String getId() {
        return id;
    }

    public void setId(String id) {
        this.id = id;
    }

    public String getSocietyId() {
        return societyId;
    }

    public void setSocietyId(String societyId) {
        this.societyId = societyId;
    }

    public String getFlatNumber() {
        return flatNumber;
    }

    public void setFlatNumber(String flatNumber) {
        this.flatNumber = flatNumber;
    }

    public String getFlatId() {
        return flatId;
    }

    public void setFlatId(String flatId) {
        this.flatId = flatId;
    }

    public String getName() {
        return name;
    }

    public void setName(String name) {
        this.name = name;
    }

    public String getEmail() {
        return email;
    }

    public void setEmail(String email) {
        this.email = email;
    }

    public String getPhone() {
        return phone;
    }

    public void setPhone(String phone) {
        this.phone = phone;
    }

    public String getRole() {
        return role;
    }

    public void setRole(String role) {
        this.role = role;
    }

    public String getStatus() {
        return status;
    }

    public void setStatus(String status) {
        this.status = status;
    }

    public String getSipUsername() {
        return sipUsername;
    }

    public void setSipUsername(String sipUsername) {
        this.sipUsername = sipUsername;
    }

    public boolean isAutoAnswer() {
        return autoAnswer;
    }

    public void setAutoAnswer(boolean autoAnswer) {
        this.autoAnswer = autoAnswer;
    }
}
