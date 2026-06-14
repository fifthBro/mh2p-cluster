/*
 * Copyright (c) 2026 fifthBro
 * https://fifthbro.github.io
 *
 * Licensed under CC BY-NC-SA 4.0
 * https://creativecommons.org/licenses/by-nc-sa/4.0/
 * NOT FOR COMMERCIAL USE
 */

package de.audi.app.terminalmode.smartphone.androidauto2.nav;

import de.esolutions.hmi.service.IWidgetContext;

import java.io.File;
import java.io.FileWriter;
import java.io.PrintWriter;
import java.text.SimpleDateFormat;
import java.util.Date;

/**
 * Cluster Android Auto Mirror
 *
 * Placeholder for future AA mirror functionality.
 * Currently logs mirror start/stop events.
 */
public class ClusterAAMirror {
    private static final String LOGCLASS = "ClusterAAMirror";
    private static final String LOG_FILE = "/tmp/cluster_aa_mirror.log";
    private static final long MAX_LOG_SIZE = 1024 * 1024;

    private IWidgetContext context;
    private volatile boolean active = false;

    public ClusterAAMirror() {
        log("ClusterAAMirror created");
    }

    public void setClusterWidgetContext(IWidgetContext ctx) {
        log("===== WIDGET CONTEXT SET =====");
        this.context = ctx;
        log("Context: " + (ctx != null ? "VALID" : "NULL"));

        if (ctx != null) {
            // IWidgetContext received - ready for use
            log("IWidgetContext is valid and ready");
        }
    }

    public void startMirror() {
        if (active) {
            log("Already active");
            return;
        }

        log("===== STARTING AA MIRROR =====");
        log("Mirror start requested");
        // TODO: Implement actual AA mirror functionality
        active = true;
    }

    public void stopMirror() {
        if (!active) return;
        log("===== STOPPING AA MIRROR =====");
        log("Mirror stop requested");
        active = false;
        // TODO: Implement actual AA mirror cleanup
    }

    private void log(String msg) {
        PrintWriter w = null;
        try {
            File f = new File(LOG_FILE);
            if (f.exists() && f.length() > MAX_LOG_SIZE) f.delete();
            w = new PrintWriter(new FileWriter(LOG_FILE, true));
            w.println(new SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS").format(new Date()) + " [" + LOGCLASS + "] " + msg);
            w.flush();
        } catch (Exception e) {
        } finally {
            if (w != null) try { w.close(); } catch (Exception e) {}
        }
    }
}
