/*
 * Copyright (c) 2026 fifthBro
 * https://fifthbro.github.io
 *
 * Licensed under CC BY-NC-SA 4.0
 * https://creativecommons.org/licenses/by-nc-sa/4.0/
 * NOT FOR COMMERCIAL USE
 */

package de.audi.app.terminalmode.smartphone.androidauto2.nav;

import de.audi.tghu.navi.app.cluster.IMapClusterService;

import java.io.File;
import java.io.FileWriter;
import java.io.PrintWriter;
import java.text.SimpleDateFormat;
import java.util.Date;

/**
 * Cluster Map Controller - New Approach Using IMapClusterService
 *
 * Strategy: Use navigation's own cluster map service to properly hide/show the map
 * - switchKombiMapToHiddenContext() - Tells navigation to hide cluster map
 * - switchKombiMapToAShownContext() - Tells navigation to show cluster map
 *
 * This uses the proper lifecycle management instead of trying to manipulate rendering.
 */
public class ClusterMapController {
    private static final String LOGCLASS = "ClusterMapController";
    private static final String LOG_FILE = "/tmp/cluster.log";  // Share log with Integration
    private static final long MAX_LOG_SIZE = 5 * 1024 * 1024; // 5MB (match Integration)

    private IMapClusterService mapClusterService;
    private boolean clusterMapHidden = false;

    public ClusterMapController() {
        log("MAP_CTRL: ========================================");
        log("MAP_CTRL: ClusterMapController CREATED");
        log("MAP_CTRL: Strategy: IMapClusterService.switchKombiMapToHiddenContext/AShownContext");
        log("MAP_CTRL: ========================================");
    }

    /**
     * Set the IMapClusterService (navigation's cluster map control)
     * Called from Activator when service becomes available
     */
    public void setMapClusterService(IMapClusterService service) {
        this.mapClusterService = service;
        if (service != null) {
            log("MAP_CTRL: ✓ IMapClusterService AVAILABLE - ready to control cluster map");
            log("MAP_CTRL:   → Can hide map: switchKombiMapToHiddenContext()");
            log("MAP_CTRL:   → Can show map: switchKombiMapToAShownContext()");
        } else {
            log("MAP_CTRL: ✗ IMapClusterService NULL - cannot control cluster map");
        }
    }

    /**
     * Hide the navigation cluster map (when Android Auto takes over)
     * Returns true if successful
     */
    public boolean hideNavigationMap() {
        if (mapClusterService == null) {
            log("MAP_CTRL: ✗ ERROR: Cannot hide map - IMapClusterService not available");
            return false;
        }

        if (clusterMapHidden) {
            log("MAP_CTRL: ⚠ Navigation map already hidden (skip)");
            return true;
        }

        try {
            log("MAP_CTRL: → Hiding navigation cluster map...");
            log("MAP_CTRL:   Calling: IMapClusterService.suspendSetup()");
            mapClusterService.suspendSetup();

            log("MAP_CTRL:   Calling: IMapClusterService.switchKombiMapToHiddenContext()");
            mapClusterService.switchKombiMapToHiddenContext();

            clusterMapHidden = true;
            log("MAP_CTRL: ✓ SUCCESS: Navigation map hidden + setup suspended");
            log("MAP_CTRL:   Cluster display now available for Android Auto");
            return true;
        } catch (Exception e) {
            log("MAP_CTRL: ✗ ERROR hiding navigation map: " + e.toString());
            log("MAP_CTRL:   Exception: " + e.getClass().getName());
            if (e.getMessage() != null) {
                log("MAP_CTRL:   Message: " + e.getMessage());
            }
            return false;
        }
    }

    /**
     * Show the navigation cluster map (when Android Auto exits)
     * Returns true if successful
     */
    public boolean showNavigationMap() {
        if (mapClusterService == null) {
            log("MAP_CTRL: ✗ ERROR: Cannot show map - IMapClusterService not available");
            return false;
        }

        if (!clusterMapHidden) {
            log("MAP_CTRL: ⚠ Navigation map already visible (skip)");
            return true;
        }

        try {
            log("MAP_CTRL: → Showing navigation cluster map...");
            log("MAP_CTRL:   Calling: IMapClusterService.switchKombiMapToAShownContext()");
            mapClusterService.switchKombiMapToAShownContext();

            log("MAP_CTRL:   Calling: IMapClusterService.notifyViewSizeChanged(true)");
            log("MAP_CTRL:   Reason: Force navigation to recreate window on displayable 33");
            mapClusterService.notifyViewSizeChanged(true);

            log("MAP_CTRL:   Calling: IMapClusterService.resumeSetupAndStore()");
            mapClusterService.resumeSetupAndStore();

            clusterMapHidden = false;
            log("MAP_CTRL: ✓ SUCCESS: Navigation map restored + view notified + setup resumed");
            log("MAP_CTRL:   Cluster display returned to navigation");
            return true;
        } catch (Exception e) {
            log("MAP_CTRL: ✗ ERROR showing navigation map: " + e.toString());
            log("MAP_CTRL:   Exception: " + e.getClass().getName());
            if (e.getMessage() != null) {
                log("MAP_CTRL:   Message: " + e.getMessage());
            }
            return false;
        }
    }

    /**
     * Check if cluster map control is available
     */
    public boolean isAvailable() {
        return mapClusterService != null;
    }

    /**
     * Check if navigation map is currently hidden
     */
    public boolean isNavigationMapHidden() {
        return clusterMapHidden;
    }

    /**
     * Called when Android Auto video becomes available (AA starts)
     * Hides the navigation cluster map
     */
    public void onAndroidAutoVideoAvailable() {
        log("MAP_CTRL: ════════════════════════════════════════");
        log("MAP_CTRL: EVENT: Android Auto video AVAILABLE");
        log("MAP_CTRL: Action: Hide navigation cluster map");
        log("MAP_CTRL: ════════════════════════════════════════");
        hideNavigationMap();
    }

    /**
     * Called when Android Auto video becomes unavailable (AA stops)
     * Shows the navigation cluster map
     */
    public void onAndroidAutoVideoUnavailable() {
        log("MAP_CTRL: ════════════════════════════════════════");
        log("MAP_CTRL: EVENT: Android Auto video UNAVAILABLE");
        log("MAP_CTRL: Action: Show navigation cluster map");
        log("MAP_CTRL: ════════════════════════════════════════");
        showNavigationMap();
    }

    /**
     * Logging helper - writes to same log file as AndroidAutoClusterIntegration
     * Format matches Integration: "timestamp | message"
     */
    private void log(String message) {
        PrintWriter writer = null;
        try {
            File logFile = new File(LOG_FILE);
            if (logFile.exists() && logFile.length() > MAX_LOG_SIZE) {
                logFile.delete();
            }

            SimpleDateFormat sdf = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS");
            String timestamp = sdf.format(new Date());
            writer = new PrintWriter(new FileWriter(LOG_FILE, true));
            writer.println(timestamp + " | " + message);  // Match Integration format
            writer.flush();
        } catch (Exception e) {
            // Silent fail - don't disrupt operation if logging fails
        } finally {
            if (writer != null) {
                try { writer.close(); } catch (Exception e) {}
            }
        }
    }

    /**
     * Cleanup - restore navigation map if we hid it
     */
    public void dispose() {
        log("MAP_CTRL: ════════════════════════════════════════");
        log("MAP_CTRL: DISPOSE: ClusterMapController shutting down");
        log("MAP_CTRL: ════════════════════════════════════════");
        if (clusterMapHidden) {
            log("MAP_CTRL: → Restoring navigation map before disposal");
            showNavigationMap();
        } else {
            log("MAP_CTRL: ✓ Navigation map not hidden, no action needed");
        }
        log("MAP_CTRL: ✓ ClusterMapController disposed");
    }
}
