package com.onprempbx.onprem.model;

import java.util.ArrayList;
import java.util.List;

/**
 * Aggregate outcome of a bulk subscriber import.
 *
 * <p>Placeholder for item #13 (BulkSubscriberImportView). The cloud
 * currently returns the per-row outcomes as a CSV body plus
 * {@code X-Import-Created} / {@code X-Import-Skipped} headers; that
 * stream is parsed into this struct by
 * {@link com.onprempbx.onprem.service.BulkSubscriberParser} once the
 * upload view lands.
 */
public class BulkResult {

    private int created;
    private int skipped;
    private List<String> errors = new ArrayList<>();

    public BulkResult() {
    }

    public BulkResult(int created, int skipped, List<String> errors) {
        this.created = created;
        this.skipped = skipped;
        this.errors = (errors != null) ? errors : new ArrayList<>();
    }

    public int getCreated() {
        return created;
    }

    public void setCreated(int created) {
        this.created = created;
    }

    public int getSkipped() {
        return skipped;
    }

    public void setSkipped(int skipped) {
        this.skipped = skipped;
    }

    public List<String> getErrors() {
        return errors;
    }

    public void setErrors(List<String> errors) {
        this.errors = (errors != null) ? errors : new ArrayList<>();
    }
}
