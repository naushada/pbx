package com.onprempbx.onprem.ui.subscriber;

import com.onprempbx.onprem.service.AuthService;
import com.onprempbx.onprem.service.BulkSubscriberParser;
import com.onprempbx.onprem.ui.MainLayout;
import com.vaadin.flow.component.button.Button;
import com.vaadin.flow.component.button.ButtonVariant;
import com.vaadin.flow.component.grid.Grid;
import com.vaadin.flow.component.html.Anchor;
import com.vaadin.flow.component.html.Div;
import com.vaadin.flow.component.html.H2;
import com.vaadin.flow.component.html.H3;
import com.vaadin.flow.component.html.Paragraph;
import com.vaadin.flow.component.notification.Notification;
import com.vaadin.flow.component.notification.NotificationVariant;
import com.vaadin.flow.component.orderedlayout.HorizontalLayout;
import com.vaadin.flow.component.orderedlayout.VerticalLayout;
import com.vaadin.flow.component.progressbar.ProgressBar;
import com.vaadin.flow.component.upload.Upload;
import com.vaadin.flow.component.upload.receivers.MemoryBuffer;
import com.vaadin.flow.router.PageTitle;
import com.vaadin.flow.router.Route;
import com.vaadin.flow.server.StreamResource;
import com.vaadin.flow.theme.lumo.LumoUtility;
import org.springframework.web.client.HttpStatusCodeException;

import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.time.LocalDateTime;
import java.time.format.DateTimeFormatter;
import java.util.ArrayList;
import java.util.List;

/**
 * Bulk-import subscribers from a CSV file.
 *
 * <p>Three stacked sections:
 * <ol>
 *   <li><b>Download template</b> — a button that streams the canonical
 *       CSV header from {@link BulkSubscriberParser#downloadTemplate()}
 *       to the browser as {@code subscribers-template.csv}.</li>
 *   <li><b>Upload CSV</b> — Vaadin {@link Upload} into a
 *       {@link MemoryBuffer}; on {@code succeeded}, the body is POSTed
 *       to the cloud via
 *       {@link BulkSubscriberParser#importSubscribers(String, String)}.
 *       A {@link ProgressBar} (indeterminate) is shown while the request
 *       is in flight.</li>
 *   <li><b>Results</b> — hidden until the import succeeds. Renders a
 *       {@link Grid} of {@link ImportResultRow}s parsed from the
 *       response CSV, plus a "save these credentials now" banner and a
 *       button to re-download the raw response CSV.</li>
 * </ol>
 *
 * <p>Item #13 of the {@code project_vaadin_admin_ui} plan.
 */
@Route(value = "subscribers/import", layout = MainLayout.class)
@PageTitle("Bulk import subscribers — onprem-pbx admin")
public class BulkSubscriberImportView extends VerticalLayout {

    private final BulkSubscriberParser parser;
    private final AuthService auth;

    // Results section — hidden until an import succeeds. Held as fields
    // so the upload handler can mutate them in place rather than rebuild
    // the page on each upload.
    private final Div resultsSection = new Div();
    private final Grid<ImportResultRow> resultsGrid = new Grid<>(ImportResultRow.class, false);
    private final HorizontalLayout downloadResultsRow = new HorizontalLayout();
    private final ProgressBar progress = new ProgressBar();

    // Raw response body cached so "Download results as CSV" can hand
    // back exactly what the cloud sent, not a re-serialized version of
    // the grid rows (preserves any field ordering / escaping nuances).
    private String lastResponseCsv;

    public BulkSubscriberImportView(BulkSubscriberParser parser, AuthService auth) {
        this.parser = parser;
        this.auth = auth;

        setSpacing(true);
        setPadding(true);

        add(new H2("Bulk import subscribers"));

        add(buildTemplateSection());
        add(buildUploadSection());
        add(buildResultsSection());
    }

    // ── Section 1 — download template ───────────────────────────────────

    private Div buildTemplateSection() {
        final Div section = new Div();
        section.add(new H3("1. Download template"));
        section.add(new Paragraph(
                "Start by downloading a template. The Excel version highlights "
                + "mandatory columns (flat_number, name, email) in YELLOW so you "
                + "can't miss them; the CSV version is the plain header only."));

        // XLSX template — primary download. Mandatory columns highlighted
        // yellow. Source of truth: scripts/generate-subscriber-template.py.
        final StreamResource xlsxResource = new StreamResource(
                "subscribers-template.xlsx",
                () -> new ByteArrayInputStream(safeTemplateXlsx()));
        xlsxResource.setContentType(
                "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet");
        final Anchor xlsxAnchor = new Anchor(xlsxResource, "");
        xlsxAnchor.getElement().setAttribute("download", true);
        xlsxAnchor.removeAll();
        final Button xlsxBtn = new Button("Download Excel template");
        xlsxBtn.addThemeVariants(ButtonVariant.LUMO_PRIMARY);
        xlsxAnchor.add(xlsxBtn);

        // CSV template — secondary download. Kept for operators on plain-
        // text tools (sed/awk/vim/Notepad) where Excel formatting is
        // unwanted overhead.
        final StreamResource csvResource = new StreamResource(
                "subscribers-template.csv",
                () -> new ByteArrayInputStream(safeTemplate()));
        csvResource.setContentType("text/csv");
        final Anchor csvAnchor = new Anchor(csvResource, "");
        csvAnchor.getElement().setAttribute("download", true);
        csvAnchor.removeAll();
        final Button csvBtn = new Button("Download CSV (header-only)");
        csvBtn.addThemeVariants(ButtonVariant.LUMO_TERTIARY);
        csvAnchor.add(csvBtn);

        section.add(xlsxAnchor, csvAnchor);
        return section;
    }

    private byte[] safeTemplate() {
        try {
            final byte[] b = parser.downloadTemplate();
            return (b != null) ? b : new byte[0];
        } catch (Exception e) {
            errorNotification("Could not fetch template: " + e.getMessage());
            return new byte[0];
        }
    }

    private byte[] safeTemplateXlsx() {
        try {
            final byte[] b = parser.downloadTemplateXlsx();
            return (b != null) ? b : new byte[0];
        } catch (Exception e) {
            errorNotification("Could not fetch XLSX template: " + e.getMessage());
            return new byte[0];
        }
    }

    // ── Section 2 — upload CSV ──────────────────────────────────────────

    private Div buildUploadSection() {
        final Div section = new Div();
        section.add(new H3("2. Upload CSV"));
        section.add(new Paragraph(
                "Pick the completed CSV. Each row is processed in order; "
                + "duplicate emails are skipped, unknown flat numbers fail the row."));

        final MemoryBuffer buffer = new MemoryBuffer();
        final Upload upload = new Upload(buffer);
        upload.setAcceptedFileTypes("text/csv", ".csv");
        upload.setMaxFiles(1);

        progress.setIndeterminate(true);
        progress.setVisible(false);

        upload.addSucceededListener(event -> {
            progress.setVisible(true);
            try {
                final byte[] bytes = buffer.getInputStream().readAllBytes();
                final String csvBody = new String(bytes, StandardCharsets.UTF_8);
                final String societyId = currentSocietyId();
                if (societyId == null || societyId.isBlank()) {
                    errorNotification("No active session — please log in again.");
                    return;
                }
                final String responseCsv = parser.importSubscribers(societyId, csvBody);
                lastResponseCsv = (responseCsv != null) ? responseCsv : "";
                renderResults(parseResponseCsv(lastResponseCsv));
            } catch (HttpStatusCodeException httpErr) {
                // Cloud surfaced a structured error (400 unknown flat,
                // 401/403 gate). Show its body — typically a short text
                // explanation — and keep the results section hidden.
                hideResults();
                final String body = httpErr.getResponseBodyAsString();
                final String msg = (body != null && !body.isBlank())
                        ? body
                        : ("Import failed (HTTP " + httpErr.getStatusCode().value() + ")");
                errorNotification(msg);
            } catch (IOException ioe) {
                hideResults();
                errorNotification("Could not read upload: " + ioe.getMessage());
            } catch (Exception ex) {
                hideResults();
                errorNotification("Import failed: " + ex.getMessage());
            } finally {
                progress.setVisible(false);
                // Let the operator try another file without a page refresh.
                upload.clearFileList();
            }
        });

        upload.addFailedListener(event -> {
            progress.setVisible(false);
            errorNotification("Upload failed: " + event.getReason().getMessage());
        });

        section.add(upload, progress);
        return section;
    }

    // ── Section 3 — results ────────────────────────────────────────────

    private Div buildResultsSection() {
        resultsSection.setVisible(false);
        resultsSection.add(new H3("3. Results"));

        final Paragraph banner = new Paragraph(
                "Save these credentials NOW — passwords are not recoverable. "
                + "Click Download to save as CSV.");
        banner.addClassNames(
                LumoUtility.Background.ERROR_10,
                LumoUtility.TextColor.ERROR,
                LumoUtility.Padding.SMALL,
                LumoUtility.BorderRadius.MEDIUM,
                LumoUtility.FontWeight.SEMIBOLD);
        resultsSection.add(banner);

        resultsGrid.addColumn(ImportResultRow::getFlatNumber).setHeader("Flat").setAutoWidth(true);
        resultsGrid.addColumn(ImportResultRow::getName).setHeader("Name").setAutoWidth(true);
        resultsGrid.addColumn(ImportResultRow::getEmail).setHeader("Email").setAutoWidth(true);
        resultsGrid.addColumn(ImportResultRow::getSipUsername).setHeader("SIP username").setAutoWidth(true);
        resultsGrid.addColumn(ImportResultRow::getGeneratedPassword).setHeader("Generated password").setAutoWidth(true);
        resultsGrid.addColumn(ImportResultRow::getStatus).setHeader("Status").setAutoWidth(true);
        resultsGrid.setAllRowsVisible(true);
        resultsSection.add(resultsGrid);

        resultsSection.add(downloadResultsRow);
        return resultsSection;
    }

    private void renderResults(List<ImportResultRow> rows) {
        resultsGrid.setItems(rows);

        // Rebuild the download-results anchor on each import so the
        // StreamResource closes over the freshest `lastResponseCsv`.
        downloadResultsRow.removeAll();
        final String ts = DateTimeFormatter.ofPattern("yyyyMMdd-HHmmss")
                .format(LocalDateTime.now());
        final String filename = "subscribers-import-result-" + ts + ".csv";
        final StreamResource resultResource = new StreamResource(
                filename,
                () -> new ByteArrayInputStream(
                        (lastResponseCsv == null ? "" : lastResponseCsv)
                                .getBytes(StandardCharsets.UTF_8)));
        resultResource.setContentType("text/csv");

        final Anchor downloadAnchor = new Anchor(resultResource, "");
        downloadAnchor.getElement().setAttribute("download", true);
        downloadAnchor.removeAll();
        final Button downloadBtn = new Button("Download results as CSV");
        downloadBtn.addThemeVariants(ButtonVariant.LUMO_PRIMARY);
        downloadAnchor.add(downloadBtn);
        downloadResultsRow.add(downloadAnchor);

        resultsSection.setVisible(true);
    }

    private void hideResults() {
        resultsSection.setVisible(false);
        resultsGrid.setItems(new ArrayList<>());
        downloadResultsRow.removeAll();
        lastResponseCsv = null;
    }

    // ── Helpers ────────────────────────────────────────────────────────

    private String currentSocietyId() {
        final AuthService.CurrentUser u = auth.getCurrentUser();
        return (u == null) ? null : u.societyId;
    }

    private static void errorNotification(String msg) {
        final Notification n = Notification.show(msg, 5000, Notification.Position.MIDDLE);
        n.addThemeVariants(NotificationVariant.LUMO_ERROR);
    }

    /**
     * Parse the cloud's response CSV — header
     * {@code flat_number,name,email,sipUsername,sipPassword,portalPassword,status}
     * — into typed rows. Mirrors {@code split_csv_row} in
     * {@code microservice_pbx.cpp}: trim trailing {@code \r}, split on
     * commas, no quoted-field handling (the cloud only quotes fields
     * containing commas, and none of the generated values do).
     */
    static List<ImportResultRow> parseResponseCsv(String csv) {
        final List<ImportResultRow> out = new ArrayList<>();
        if (csv == null || csv.isEmpty()) return out;

        final String[] lines = csv.split("\n", -1);
        // Skip line 0 — header.
        for (int i = 1; i < lines.length; i++) {
            String line = lines[i];
            if (line.isEmpty()) continue;
            if (line.charAt(line.length() - 1) == '\r') {
                line = line.substring(0, line.length() - 1);
            }
            if (line.isEmpty()) continue;

            final String[] f = line.split(",", -1);
            final ImportResultRow row = new ImportResultRow();
            row.flatNumber        = field(f, 0);
            row.name              = field(f, 1);
            row.email             = field(f, 2);
            row.sipUsername       = field(f, 3);
            // Cloud emits sipPassword in column 4 and portalPassword in
            // column 5; the UI surfaces the SIP password as the primary
            // "generated password" since that's the one the resident
            // needs for their softphone. Portal password (column 5) is
            // intentionally not exposed in the grid — the full row is
            // still in the downloadable CSV.
            row.generatedPassword = field(f, 4);
            row.status            = field(f, 6);
            out.add(row);
        }
        return out;
    }

    private static String field(String[] f, int i) {
        return (i >= 0 && i < f.length) ? f[i] : "";
    }

    /**
     * One parsed row from the cloud's import-response CSV. Public-static
     * nested class so the {@link Grid#addColumn} method references work
     * and tests (if added later) can construct fixtures.
     */
    public static class ImportResultRow {
        private String flatNumber;
        private String name;
        private String email;
        private String sipUsername;
        private String generatedPassword;
        private String status;

        public String getFlatNumber()        { return flatNumber; }
        public String getName()              { return name; }
        public String getEmail()             { return email; }
        public String getSipUsername()       { return sipUsername; }
        public String getGeneratedPassword() { return generatedPassword; }
        public String getStatus()            { return status; }

        public void setFlatNumber(String v)        { this.flatNumber = v; }
        public void setName(String v)              { this.name = v; }
        public void setEmail(String v)             { this.email = v; }
        public void setSipUsername(String v)       { this.sipUsername = v; }
        public void setGeneratedPassword(String v) { this.generatedPassword = v; }
        public void setStatus(String v)            { this.status = v; }
    }
}
