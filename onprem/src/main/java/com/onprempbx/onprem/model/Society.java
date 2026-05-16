package com.onprempbx.onprem.model;

import com.fasterxml.jackson.annotation.JsonInclude;
import com.fasterxml.jackson.annotation.JsonProperty;

/**
 * Mirrors a row in the cloud's {@code societies} Mongo collection.
 *
 * <p>The cloud surfaces {@code sipRealm} and {@code turnSharedSecret}
 * on the single-society detail endpoint so the operator can paste them
 * into the on-prem agent's {@code .env}; the list endpoint strips
 * {@code turnSharedSecret} server-side (see
 * {@code handle_societies_GET}). Either way the field is present here
 * — Jackson tolerates absent fields and omits nulls on write.
 */
@JsonInclude(JsonInclude.Include.NON_NULL)
public class Society {

    @JsonProperty("_id")
    private String id;

    private String code;
    private String name;
    private String address;
    private String sipRealm;
    private String turnSharedSecret;
    private int maxConcurrentCalls;
    private int ringTimeoutSec;

    public Society() {
    }

    public String getId() {
        return id;
    }

    public void setId(String id) {
        this.id = id;
    }

    public String getCode() {
        return code;
    }

    public void setCode(String code) {
        this.code = code;
    }

    public String getName() {
        return name;
    }

    public void setName(String name) {
        this.name = name;
    }

    public String getAddress() {
        return address;
    }

    public void setAddress(String address) {
        this.address = address;
    }

    public String getSipRealm() {
        return sipRealm;
    }

    public void setSipRealm(String sipRealm) {
        this.sipRealm = sipRealm;
    }

    public String getTurnSharedSecret() {
        return turnSharedSecret;
    }

    public void setTurnSharedSecret(String turnSharedSecret) {
        this.turnSharedSecret = turnSharedSecret;
    }

    public int getMaxConcurrentCalls() {
        return maxConcurrentCalls;
    }

    public void setMaxConcurrentCalls(int maxConcurrentCalls) {
        this.maxConcurrentCalls = maxConcurrentCalls;
    }

    public int getRingTimeoutSec() {
        return ringTimeoutSec;
    }

    public void setRingTimeoutSec(int ringTimeoutSec) {
        this.ringTimeoutSec = ringTimeoutSec;
    }
}
