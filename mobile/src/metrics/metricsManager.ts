/**
 * MetricsManager - Collects and manages usage metrics for the mobile app
 * 
 * This system tracks key user interactions and app usage patterns
 * that can be exported to various formats for analysis.
 */

export interface MetricEvent {
  id: string;
  timestamp: number;
  type: string;
  data?: Record<string, any>;
}

export interface MetricsConfig {
  enabled: boolean;
  exportFormat: 'json' | 'csv' | 'both';
  exportIntervalMs: number;
  /** When true, log each tracked event to the console (off by default). */
  debug?: boolean;
}

export class MetricsManager {
  private static instance: MetricsManager;
  private events: MetricEvent[] = [];
  private config: MetricsConfig;
  private isInitialized = false;

  private constructor(config: MetricsConfig = {
    enabled: true,
    exportFormat: 'json',
    exportIntervalMs: 300000 // 5 minutes
  }) {
    this.config = config;
  }

  static getInstance(config?: MetricsConfig): MetricsManager {
    if (!MetricsManager.instance) {
      MetricsManager.instance = new MetricsManager(config);
    }
    return MetricsManager.instance;
  }

  initialize(): void {
    if (this.isInitialized) return;
    this.isInitialized = true;
    if (this.config.debug) {
      console.log('MetricsManager initialized');
    }
  }

  // Track a specific event with optional metadata
  trackEvent(type: string, data?: Record<string, any>): void {
    if (!this.config.enabled) return;
    
    const event: MetricEvent = {
      id: this.generateId(),
      timestamp: Date.now(),
      type,
      data
    };
    
    this.events.push(event);

    // Optional debug logging (off by default — avoids runtime/test noise).
    if (this.config.debug) {
      console.log(`[Metrics] Event tracked: ${type}`, data);
    }
  }

  // Track user login (subscriber identity — Session has no id field)
  trackLogin(societyId: string, flatNumber: string): void {
    this.trackEvent('user_login', {societyId, flatNumber});
  }

  // Track user logout
  trackLogout(societyId: string, flatNumber: string): void {
    this.trackEvent('user_logout', {societyId, flatNumber});
  }

  // Track an outbound call once it ends. success = call connected/completed.
  trackOutboundCall(
    destinationFlat: string,
    callDurationMs: number | undefined,
    success: boolean
  ): void {
    this.trackEvent('outbound_call', {destinationFlat, callDurationMs, success});
  }

  // Track an inbound call once it ends. accepted = call connected/completed.
  trackInboundCall(
    callerFlat: string,
    callDurationMs: number | undefined,
    accepted: boolean
  ): void {
    this.trackEvent('inbound_call', {callerFlat, callDurationMs, accepted});
  }

  // Track registration
  trackRegistration(societyId: string, flatNumber: string): void {
    this.trackEvent('registration', {
      societyId,
      flatNumber,
      timestamp: Date.now()
    });
  }

  // Get all collected metrics
  getMetrics(): MetricEvent[] {
    return [...this.events];
  }

  // Clear all collected metrics
  clearMetrics(): void {
    this.events = [];
  }

  // Export metrics in specified format
  exportMetrics(format: 'json' | 'csv' | 'both' = this.config.exportFormat): string | Record<string, any> {
    const metrics = this.events;
    
    if (format === 'json' || format === 'both') {
      const jsonExport = JSON.stringify({
        metrics: metrics,
        exportedAt: new Date().toISOString(),
        count: metrics.length
      }, null, 2);
      
      if (format === 'json') {
        return jsonExport;
      }
    }
    
    if (format === 'csv' || format === 'both') {
      const csvExport = this.toCSV(metrics);
      if (format === 'csv') {
        return csvExport;
      }
    }
    
    return {};
  }

  // Convert metrics to CSV format
  private toCSV(events: MetricEvent[]): string {
    if (events.length === 0) {
      return '';
    }
    
    // Create header
    const headers = ['id', 'timestamp', 'type', 'data'];
    const csvRows = [headers.join(',')];
    
    // Add data rows
    events.forEach(event => {
      const row = [
        `"${event.id}"`,
        `"${new Date(event.timestamp).toISOString()}"`,
        `"${event.type}"`,
        `"${JSON.stringify(event.data ?? {})}"`
      ];
      csvRows.push(row.join(','));
    });
    
    return csvRows.join('\n');
  }

  // Generate unique ID for events
  private generateId(): string {
    return 'metric_' + Math.random().toString(36).substr(2, 9);
  }
}