/*
 * Copyright (c) 2026 fifthBro
 * https://fifthbro.github.io
 *
 * Licensed under CC BY-NC-SA 4.0
 * https://creativecommons.org/licenses/by-nc-sa/4.0/
 * NOT FOR COMMERCIAL USE
 */

/**
 * CarPlay Cluster Integration
 *
 * Mirrors the AA cluster display pattern:
 * - Receives CarPlay DSI callbacks via registerDSIListener
 * - Detects screen resource ownership changes → starts/stops mirror daemon
 * - Future: CarPlay nav data → BAP cluster (requires native iAP2 hook)
 *
 * Registration: serviceManager.registerDSIListener(0, DSICarplayListener.class.getName(), this)
 * Same framework API as OEM's CarplayDSILifecycleController uses.
 */
package de.audi.app.terminalmode.smartphone.carplay;

import org.dsi.ifc.carplay.AppState;
import org.dsi.ifc.carplay.CallState;
import org.dsi.ifc.carplay.DSICarplayListener;
import org.dsi.ifc.carplay.DeviceInfo;
import org.dsi.ifc.carplay.PlaybackInfo;
import org.dsi.ifc.carplay.Resource;
import org.dsi.ifc.carplay.TelephonyState;
import org.dsi.ifc.carplay.TrackData;
import org.dsi.ifc.global.ResourceLocator;

import de.audi.atip.interapp.combi.bap.navi.CombiBAPServiceNavi;
import de.audi.atip.interapp.combi.bap.navi.CombiBAPConstantsNavi;
import de.audi.atip.interapp.combi.bap.navi.data.CombiBAPNaviManeuverDescriptor;
import de.audi.atip.interapp.combi.bap.navi.data.CombiBAPNaviLaneGuidanceData;
import de.audi.atip.interapp.combi.bap.navi.data.CurrentPositionInfo;
import de.audi.atip.interapp.combi.bap.navi.data.DistanceUnit;
import java.util.Timer;
import java.util.TimerTask;
import de.audi.atip.interapp.combi.bap.PartialPopupBAPService;
import de.audi.mib.system.ISysServices;
import de.audi.mib.system.units.IUnitManager;
import de.audi.mib.system.units.IUnitProvider;
import de.audi.mib.system.units.UnitSetting;
import de.audi.app.car.api.core.ICarCoreServices;
import de.audi.app.car.api.services.ICarExteriorLightService;
import de.audi.atip.utils.reactive.properties.ReadOnlyProperty;

import java.io.File;
import java.io.FileWriter;
import java.io.FileInputStream;
import java.io.PrintWriter;

public class CarPlayClusterIntegration implements DSICarplayListener {

    private static final String LOGCLASS = "CarPlayClusterIntegration";
    private static final String DEFAULT_LOG_PATH = "/tmp";
    private static final String LOG_FILE_NAME = "cluster.log";
    private static final String HASHED_LOG_FILE_NAME = "cluster_hashed.log";

    /* CarPlay DSI constants */
    private static final int RESOURCE_MAIN_SCREEN = 1;
    private static final int RESOURCEOWNER_MAINUNIT = 1;
    private static final int RESOURCEOWNER_DEVICE = 2;

    /* Mirror config */
    private String  mirrorFifo = "/tmp/cluster_ctl";
    private String  mirrorMode  = "fill";
    private float   mirrorZoomX = 1.0f;
    private float   mirrorZoomY = 1.0f;
    private float   mirrorPanX  = 0.0f;
    private float   mirrorPanY  = 0.0f;
    private boolean enableMapRender = false;
    /* Cluster lifecycle state. CarPlay is mirror-only (no h264 path).
     * Mirror has no warm state — pause would just kill the child — so we
     * collapse to IDLE/RENDERING and use start/stop on the wire only.
     *   IDLE      — no carplay screen, nothing on cluster.
     *   RENDERING — mirror child running on cluster. */
    private static final int CL_IDLE      = 0;
    private static final int CL_RENDERING = 2;
    private int clusterState = CL_IDLE;

    /* Logging config */
    private String  logFilePath = DEFAULT_LOG_PATH + "/" + LOG_FILE_NAME;
    private String  hashedLogFilePath = DEFAULT_LOG_PATH + "/" + HASHED_LOG_FILE_NAME;
    private boolean fileLoggingEnabled = true;
    private boolean fileHashingEnabled = true;
    private boolean externalLoggingEnabled = true;
    private long    logFileSize = 50;  /* MB */

    /* External storage — same mount points as AA */
    private static final String[] MOUNT_POINTS = {
        "/fs/sda0", "/fs/sdb0", "/fs/sdc0", "/fs/sdd0",
        "/fs/usb0", "/fs/usb1"
    };
    private String lastLogPath = null;
    private String configJsonCache = null;  /* cached for country lookups */

    /* Services */
    private ISysServices sysServices = null;
    private ICarCoreServices carCoreServices = null;
    private ICarExteriorLightService exteriorLightService = null;
    private PartialPopupBAPService popupService = null;

    /* Unit detection — cached, multi-tier like AA */
    private boolean unitsDetermined = false;
    private boolean useMetricCached = true;
    private boolean driveSideDetermined = false;
    private boolean isRHD = false;
    private String resolvedImperialSmallUnit = null;

    /* Car time tracking */
    private long carTimeOffset = 0;
    private boolean carTimeAvailable = false;

    /* Session */
    private String sessionId = null;

    /* Unit config */
    private boolean forceImperial = false;
    private int     metricUnitThreshold = 1000;
    private int     imperialUnitThreshold = 161;
    private String  imperialSmallUnit = "auto";
    private boolean useMetric = true;

    /* BAP / cluster config */
    private int     maneuverStateMask = 0;
    private boolean enableRoundaboutLatching = true;
    private boolean enableHeartbeat = true;
    private int     heartbeatInterval = 2000;
    private String  bargraphMode = "auto";
    private int     dynamicBargraphDistance = 100;
    private int     dynamicBargraphPercent = 50;
    private int     destinationDisplayDuration = 5000;
    private String  mirrorCarConfigJson = null;

    /* BAP cluster service */
    private CombiBAPServiceNavi clusterService = null;

    /* Route guidance state */
    private volatile boolean rgActive = false;
    private volatile boolean carplayHasScreen = false;
    private int lastMainElement = -1;
    private int lastDirection = -1;
    /* Cached side-street byte array from the most recent updateManeuver call.
     * Heartbeat re-publishes it every tick so the cluster doesn't wipe the
     * side-road strokes after the first render (the previous `new byte[0]`
     * heartbeat was erasing them within 2 s). */
    private byte[] lastSideStreets = new byte[0];
    private String lastRoadName = "";
    private String lastSignPost = "";
    private int    lastLaneCount = 0;
    /* Cached lane record array, re-sent by heartbeat so cluster doesn't time out. */
    private CombiBAPNaviLaneGuidanceData[] lastLanesCached = null;
    /* Signature of last published lane set, for delta detection. "pos:dir:status|pos:dir:status|..." */
    private String lastLaneSignature = "";
    private int lastDistanceM = -1;
    private int lastDistUnit = 0;
    private int lastDistValue = 0;
    private int lastBargraph = 0;
    private boolean lastBargraphEnabled = false;
    private int maneuverInitialDistance = 0;
    private int lastManeuverType = -1;
    private int lastManeuverStateBAP = -1;
    private int lastTimeToDestSec = 0;
    private int lastDistToDestM = 0;
    private long lastDistUpdateTime = 0;  /* for rate limiting */
    private long destinationShownTime = 0;  /* for destination display duration */
    private boolean inRoundabout = false;
    private int cachedRoundaboutDirection = -1;
    private int lastRoundaboutManeuverType = -1;
    /* Delayed clear timer — same non-blocking pattern as AA (see
     * AndroidAutoClusterIntegration.clearCluster). Using Thread.sleep on the
     * PPS reader thread stalls incoming updates for the full hold duration;
     * a daemon Timer lets the reader keep processing. */
    private java.util.Timer pendingClearTimer = null;
    /* Diagnostic: iap_idx of the slot we last displayed as current. Used by
     * parseAndApply to emit a TRACE log whenever ML advances — lets us see
     * every maneuver Apple pushes, not only the ones that happen to change
     * the BAP type/direction. */
    private int lastCurrentSlotIapIdx = -1;
    private boolean startupPopupShown = false;

    /* Dynamic distance thresholds — same as AA */
    private int veryFarBoundary = 5000;
    private int farBoundary = 1000;
    private int approachingBoundary = 500;
    private int nearBoundary = 200;
    private int closeBoundary = 100;
    private int veryCloseBoundary = 50;
    private int veryFarDistanceThreshold = 100;
    private int farDistanceThreshold = 50;
    private int approachingDistanceThreshold = 25;
    private int nearDistanceThreshold = 15;
    private int closeDistanceThreshold = 15;
    private int veryCloseDistanceThreshold = 10;
    private int nowDistanceThreshold = 5;
    private int veryFarRateLimit = 2000;
    private int farRateLimit = 500;
    private int approachingRateLimit = 250;
    private int nearRateLimit = 200;
    private int closeRateLimit = 150;
    private int veryCloseRateLimit = 120;
    private int nowRateLimit = 100;

    /* ============================================================
     * Configuration — reads same JSON config as AndroidAutoClusterIntegration
     * ============================================================ */

    private static final String CONFIG_RESOURCE = "/cluster_config.json";

    public void loadConfig() {
        try {
            /* Try external config first (USB/SD), then JAR embedded */
            String json = loadExternalConfigIfExists();
            if (json == null) {
                java.io.InputStream is = getClass().getResourceAsStream(CONFIG_RESOURCE);
                if (is == null) {
                    logCluster("CONFIG: " + CONFIG_RESOURCE + " not found in JAR");
                    return;
                }
                java.io.BufferedReader reader = new java.io.BufferedReader(new java.io.InputStreamReader(is));
                StringBuffer sb = new StringBuffer();
                String line;
                while ((line = reader.readLine()) != null) sb.append(line);
                reader.close();
                is.close();
                json = sb.toString();
                logCluster("CONFIG: Loaded from JAR");
            }
            configJsonCache = json;  /* cache for country lookups */

            int configStart = json.indexOf("\"config\"");
            if (configStart < 0) return;

            /* Logging */
            fileLoggingEnabled = parseBoolean(json, "enableFileLogging", configStart, true);
            fileHashingEnabled = parseBoolean(json, "enableFileHashing", configStart, true);
            externalLoggingEnabled = parseBoolean(json, "enableExternalLogging", configStart, true);
            logFilePath = parseString(json, "logFilePath", configStart, logFilePath);
            logFileSize = parseInt(json, "logFileSize", configStart, 50);

            /* Mirror */
            enableMapRender = parseBoolean(json, "enableMapRender", configStart, false);
            mirrorFifo = parseString(json, "mirrorFifo", configStart, mirrorFifo);
            mirrorMode = parseString(json, "mirrorMode", configStart, mirrorMode);
            mirrorZoomX = parseFloat(json, "mirrorZoomX", configStart, mirrorZoomX);
            mirrorZoomY = parseFloat(json, "mirrorZoomY", configStart, mirrorZoomY);
            mirrorPanX = parseFloat(json, "mirrorPanX", configStart, mirrorPanX);
            mirrorPanY = parseFloat(json, "mirrorPanY", configStart, mirrorPanY);

            /* BAP / cluster */
            maneuverStateMask = parseInt(json, "maneuverStateMask", configStart, 0);
            enableRoundaboutLatching = parseBoolean(json, "enableRoundaboutLatching", configStart, true);
            enableHeartbeat = parseBoolean(json, "enableHeartbeat", configStart, true);
            heartbeatInterval = parseInt(json, "heartbeatInterval", configStart, 2000);
            bargraphMode = parseString(json, "bargraphMode", configStart, "auto");
            dynamicBargraphDistance = parseInt(json, "dynamicBargraphDistance", configStart, 100);
            dynamicBargraphPercent = parseInt(json, "dynamicBargraphPercent", configStart, 50);
            destinationDisplayDuration = parseInt(json, "destinationDisplayDuration", configStart, 5000);

            /* Units */
            forceImperial = parseBoolean(json, "forceImperial", configStart, false);
            metricUnitThreshold = parseInt(json, "metricUnitThreshold", configStart, 1000);
            imperialUnitThreshold = parseInt(json, "imperialUnitThreshold", configStart, 161);
            imperialSmallUnit = parseString(json, "imperialSmallUnit", configStart, "auto");
            useMetric = !forceImperial;

            /* Per-car mirror overrides */
            int carCfgPos = json.indexOf("\"mirrorCarConfig\"");
            if (carCfgPos >= 0) {
                int brace = json.indexOf("{", carCfgPos);
                if (brace >= 0) {
                    int depth = 0, k = brace, end = -1;
                    while (k < json.length()) {
                        char c = json.charAt(k);
                        if (c == '{') depth++;
                        else if (c == '}') { depth--; if (depth == 0) { end = k; break; } }
                        k++;
                    }
                    if (end > brace) mirrorCarConfigJson = json.substring(brace, end + 1);
                }
            }

            /* Dynamic thresholds */
            int threshStart = json.indexOf("\"dynamicThresholds\"");
            if (threshStart >= 0) {
                int vfS = json.indexOf("\"veryFar\"", threshStart);
                if (vfS >= 0) {
                    veryFarBoundary = parseInt(json, "boundary", vfS, veryFarBoundary);
                    veryFarDistanceThreshold = parseInt(json, "distance", vfS, veryFarDistanceThreshold);
                    veryFarRateLimit = parseInt(json, "rateLimit", vfS, veryFarRateLimit);
                }
                int fS = json.indexOf("\"far\"", threshStart);
                if (fS >= 0) {
                    farBoundary = parseInt(json, "boundary", fS, farBoundary);
                    farDistanceThreshold = parseInt(json, "distance", fS, farDistanceThreshold);
                    farRateLimit = parseInt(json, "rateLimit", fS, farRateLimit);
                }
                int aS = json.indexOf("\"approaching\"", threshStart);
                if (aS >= 0) {
                    approachingBoundary = parseInt(json, "boundary", aS, approachingBoundary);
                    approachingDistanceThreshold = parseInt(json, "distance", aS, approachingDistanceThreshold);
                    approachingRateLimit = parseInt(json, "rateLimit", aS, approachingRateLimit);
                }
                int nS = json.indexOf("\"near\"", threshStart);
                if (nS >= 0) {
                    nearBoundary = parseInt(json, "boundary", nS, nearBoundary);
                    nearDistanceThreshold = parseInt(json, "distance", nS, nearDistanceThreshold);
                    nearRateLimit = parseInt(json, "rateLimit", nS, nearRateLimit);
                }
                int cS = json.indexOf("\"close\"", threshStart);
                if (cS >= 0) {
                    closeBoundary = parseInt(json, "boundary", cS, closeBoundary);
                    closeDistanceThreshold = parseInt(json, "distance", cS, closeDistanceThreshold);
                    closeRateLimit = parseInt(json, "rateLimit", cS, closeRateLimit);
                }
                int vcS = json.indexOf("\"veryClose\"", threshStart);
                if (vcS >= 0) {
                    veryCloseBoundary = parseInt(json, "boundary", vcS, veryCloseBoundary);
                    veryCloseDistanceThreshold = parseInt(json, "distance", vcS, veryCloseDistanceThreshold);
                    veryCloseRateLimit = parseInt(json, "rateLimit", vcS, veryCloseRateLimit);
                }
                int nowS = json.indexOf("\"now\"", threshStart);
                if (nowS >= 0) {
                    nowDistanceThreshold = parseInt(json, "distance", nowS, nowDistanceThreshold);
                    nowRateLimit = parseInt(json, "rateLimit", nowS, nowRateLimit);
                }
            }

            logCluster("CONFIG: mapRender=" + enableMapRender + " mode=" + mirrorMode
                       + " zoomX=" + mirrorZoomX + " zoomY=" + mirrorZoomY
                       + " imperial=" + forceImperial + " bargraph=" + bargraphMode
                       + " heartbeat=" + enableHeartbeat + "/" + heartbeatInterval
                       + " stateMask=0x" + Integer.toHexString(maneuverStateMask));
        } catch (Exception e) {
            logCluster("CONFIG: Error loading config: " + e.toString());
        }
    }

    private static boolean parseBoolean(String json, String key, int start, boolean def) {
        int pos = json.indexOf("\"" + key + "\"", start);
        if (pos < 0 || pos > start + 2000) return def;
        int colon = json.indexOf(":", pos);
        if (colon < 0) return def;
        String rest = json.substring(colon + 1).trim();
        return rest.startsWith("true");
    }

    private static String parseString(String json, String key, int start, String def) {
        int pos = json.indexOf("\"" + key + "\"", start);
        if (pos < 0 || pos > start + 2000) return def;
        int colon = json.indexOf(":", pos);
        int q1 = json.indexOf("\"", colon + 1);
        int q2 = json.indexOf("\"", q1 + 1);
        if (q1 > 0 && q2 > q1) return json.substring(q1 + 1, q2);
        return def;
    }

    private static int parseInt(String json, String key, int start, int def) {
        int pos = json.indexOf("\"" + key + "\"", start);
        if (pos < 0 || pos > start + 2000) return def;
        int colon = json.indexOf(":", pos);
        int end = json.indexOf(",", colon + 1);
        if (end < 0) end = json.indexOf("}", colon + 1);
        if (end > colon) {
            try { return Integer.parseInt(json.substring(colon + 1, end).trim()); }
            catch (Exception e) { return def; }
        }
        return def;
    }

    private static float parseFloat(String json, String key, int start, float def) {
        int pos = json.indexOf("\"" + key + "\"", start);
        if (pos < 0 || pos > start + 2000) return def;
        int colon = json.indexOf(":", pos);
        int end = json.indexOf(",", colon + 1);
        if (end < 0) end = json.indexOf("}", colon + 1);
        if (end > colon) {
            try { return Float.parseFloat(json.substring(colon + 1, end).trim()); }
            catch (Exception e) { return def; }
        }
        return def;
    }

    private static int parseIntValue(String json, String key, int start, int def, int min, int max) {
        int v = parseInt(json, key, start, def);
        if (v < min) return min;
        if (v > max) return max;
        return v;
    }

    /* ============================================================
     * Service setters
     * ============================================================ */

    public void setSysServices(ISysServices service) {
        this.sysServices = service;
        unitsDetermined = false;  /* re-query units with new service */
        logCluster("SYS: ISysServices " + (service != null ? "AVAILABLE" : "NULL"));
        resolveCarConfig();
    }

    public void setCarCoreServices(ICarCoreServices service) {
        this.carCoreServices = service;
        driveSideDetermined = false;
        logCluster("SYS: ICarCoreServices " + (service != null ? "AVAILABLE" : "NULL"));
    }

    public void setExteriorLightService(ICarExteriorLightService service) {
        this.exteriorLightService = service;
        logCluster("SYS: ICarExteriorLightService " + (service != null ? "AVAILABLE" : "NULL"));
    }

    public void setPopupService(PartialPopupBAPService service) {
        this.popupService = service;
        logCluster("SYS: PartialPopupBAPService " + (service != null ? "AVAILABLE" : "NULL"));
    }

    public void unsetClusterService(CombiBAPServiceNavi service) {
        this.clusterService = null;
        logCluster("BAP: CombiBAPServiceNavi unset");
    }

    /* ============================================================
     * Initialization — called after loadConfig + services set
     * ============================================================ */

    public void init() {
        sessionId = generateSessionId();
        writeSessionStartMarkers();

        String v = getClass().getPackage() != null ? getClass().getPackage().getImplementationVersion() : null;
        if (v == null || v.length() == 0) v = "dev";
        logCluster("SYS: CarPlay Cluster Integration Initialized (" + v + ")");

        tryGetCarTime();

        /* Start PPS reader for RG data from dio_manager_preload.so. Daemon
         * thread idles until the PPS file appears, so safe to start early. */
        startPpsListener();
    }

    private String generateSessionId() {
        java.util.Random rand = new java.util.Random();
        String id = String.valueOf(rand.nextInt(99999));
        while (id.length() < 5) id = "0" + id;
        return id;
    }

    private void writeSessionStartMarkers() {
        if (!fileLoggingEnabled) return;
        String sep = "========================================";
        String start = "=== NEW CP SESSION (ID: " + sessionId + ") ===";
        logDirect(sep);
        logDirect(start);
        logDirect(sep);
    }

    /**
     * Bootstrap logging — writes directly to /tmp without path resolution.
     * Used before config is loaded and during session markers.
     */
    private void logDirect(String message) {
        try {
            String path = DEFAULT_LOG_PATH + "/" + LOG_FILE_NAME;
            FileWriter fw = new FileWriter(path, true);
            fw.write(getDualTimestamp() + " | CP: " + message + "\n");
            fw.flush();
            fw.close();
        } catch (Exception e) {
            // ignore
        }
    }

    /* ============================================================
     * Unit detection — multi-tier, same as AA
     * ============================================================ */

    private boolean shouldUseMetric() {
        if (unitsDetermined) return useMetricCached;

        /* Tier 0: forceImperial override */
        if (forceImperial) {
            logCluster("UNIT_SELECTION: IMPERIAL (forceImperial=true)");
            useMetricCached = false;
            unitsDetermined = true;
            return false;
        }

        /* Tier 1: ISysServices — car menu setting */
        if (sysServices != null) {
            try {
                IUnitManager unitManager = sysServices.units();
                if (unitManager != null) {
                    IUnitProvider distProv = unitManager.distance();
                    if (distProv != null) {
                        ReadOnlyProperty settingProp = distProv.setting();
                        if (settingProp != null) {
                            UnitSetting setting = (UnitSetting) settingProp.get();
                            if (setting != null) {
                                int distUnit = setting.getUnitValue();
                                if (distUnit == 1) {
                                    logCluster("UNIT_SELECTION: METRIC (car setting km)");
                                    useMetricCached = true;
                                    unitsDetermined = true;
                                    return true;
                                } else if (distUnit == 2) {
                                    logCluster("UNIT_SELECTION: IMPERIAL (car setting mi)");
                                    useMetricCached = false;
                                    unitsDetermined = true;
                                    return false;
                                }
                            }
                        }
                    }
                    /* Fallback to speed unit */
                    IUnitProvider speedProv = unitManager.speed();
                    if (speedProv != null) {
                        ReadOnlyProperty settingProp = speedProv.setting();
                        if (settingProp != null) {
                            UnitSetting setting = (UnitSetting) settingProp.get();
                            if (setting != null) {
                                int speedUnit = setting.getUnitValue();
                                if (speedUnit == 1) {
                                    logCluster("UNIT_SELECTION: METRIC (car setting km/h)");
                                    useMetricCached = true;
                                    unitsDetermined = true;
                                    return true;
                                } else if (speedUnit == 2) {
                                    logCluster("UNIT_SELECTION: IMPERIAL (car setting mph)");
                                    useMetricCached = false;
                                    unitsDetermined = true;
                                    return false;
                                }
                            }
                        }
                    }
                }
            } catch (Exception e) {
                logCluster("UNIT_SELECTION: ISysServices error: " + e.toString());
            }
        }

        /* Tier 2: Country lookup from JSON config */
        if (configJsonCache != null) {
            String cc = getCountryCode();
            if (cc != null && cc.length() > 0) {
                boolean imperial = lookupImperialInConfig(cc);
                if (imperial) {
                    logCluster("UNIT_SELECTION: IMPERIAL (country " + cc + ")");
                    useMetricCached = false;
                    unitsDetermined = true;
                    return false;
                } else {
                    logCluster("UNIT_SELECTION: METRIC (country " + cc + ")");
                    useMetricCached = true;
                    unitsDetermined = true;
                    return true;
                }
            }
        }

        /* Tier 3: Default metric */
        logCluster("UNIT_SELECTION: METRIC (default)");
        useMetricCached = true;
        return true;
    }

    /* ============================================================
     * Drive side detection — for info logging
     * ============================================================ */

    public boolean isDriveRightHandSide() {
        if (driveSideDetermined) return isRHD;

        if (carCoreServices != null) {
            try {
                ReadOnlyProperty dslProp = carCoreServices.configuration().airConditionMaster().driverSideLeft();
                if (dslProp != null) {
                    Boolean dsl = (Boolean) dslProp.get();
                    if (dsl != null) {
                        isRHD = !dsl.booleanValue();
                        driveSideDetermined = true;
                        logCluster("DRIVE_SIDE: " + (isRHD ? "RHD" : "LHD") + " (driverSideLeft=" + dsl + ")");
                        return isRHD;
                    }
                }
            } catch (Exception e) {
                logCluster("DRIVE_SIDE: error: " + e.toString());
            }
        }

        /* Fallback: country lookup */
        if (configJsonCache != null) {
            String cc = getCountryCode();
            if (cc != null && cc.length() > 0) {
                isRHD = lookupRHDInConfig(cc);
                driveSideDetermined = true;
                logCluster("DRIVE_SIDE: " + (isRHD ? "RHD" : "LHD") + " (country " + cc + ")");
                return isRHD;
            }
        }

        logCluster("DRIVE_SIDE: LHD (default)");
        return false;
    }

    /* ============================================================
     * Per-car variant resolution
     * ============================================================ */

    private void resolveCarConfig() {
        if (sysServices == null) return;
        try {
            de.audi.mib.system.config.ICarType ct = sysServices.config().carType();
            int carClass = ct.carClass();
            int generation = ct.generation();
            String key = carClass + "_" + generation;

            if (mirrorCarConfigJson == null) {
                logCluster("CAR_VARIANT: no mirrorCarConfig (class=" + carClass + " gen=" + generation + ")");
                return;
            }

            int keyPos = mirrorCarConfigJson.indexOf("\"" + key + "\"");
            if (keyPos < 0) {
                logCluster("CAR_VARIANT: no entry for " + key);
                return;
            }

            int brace = mirrorCarConfigJson.indexOf("{", keyPos);
            if (brace < 0) return;
            int end = mirrorCarConfigJson.indexOf("}", brace);
            if (end < 0) return;
            String entry = mirrorCarConfigJson.substring(brace, end + 1);

            /* Extract name */
            String name = key;
            int namePos = entry.indexOf("\"name\"");
            if (namePos >= 0) {
                int q1 = entry.indexOf("\"", namePos + 6);
                q1 = entry.indexOf("\"", q1 + 1);
                int q2 = entry.indexOf("\"", q1 + 1);
                if (q1 >= 0 && q2 > q1) name = entry.substring(q1 + 1, q2);
            }

            /* Extract bargraphMode */
            int bmPos = entry.indexOf("\"bargraphMode\"");
            if (bmPos >= 0) {
                int q1 = entry.indexOf("\"", entry.indexOf(":", bmPos) + 1);
                int q2 = entry.indexOf("\"", q1 + 1);
                if (q1 >= 0 && q2 > q1) bargraphMode = entry.substring(q1 + 1, q2);
            }

            /* Extract zoom/pan */
            int zxPos = entry.indexOf("\"zoomX\"");
            if (zxPos >= 0) { int c = entry.indexOf(":", zxPos); int e2 = entry.indexOf(",", c+1); if (e2<0) e2=entry.indexOf("}",c+1); if (e2>c) { try { mirrorZoomX = Float.parseFloat(entry.substring(c+1,e2).trim()); } catch (Exception ex) {} } }
            int zyPos = entry.indexOf("\"zoomY\"");
            if (zyPos >= 0) { int c = entry.indexOf(":", zyPos); int e2 = entry.indexOf(",", c+1); if (e2<0) e2=entry.indexOf("}",c+1); if (e2>c) { try { mirrorZoomY = Float.parseFloat(entry.substring(c+1,e2).trim()); } catch (Exception ex) {} } }
            int pxPos = entry.indexOf("\"panX\"");
            if (pxPos >= 0) { int c = entry.indexOf(":", pxPos); int e2 = entry.indexOf(",", c+1); if (e2<0) e2=entry.indexOf("}",c+1); if (e2>c) { try { mirrorPanX = Float.parseFloat(entry.substring(c+1,e2).trim()); } catch (Exception ex) {} } }
            int pyPos = entry.indexOf("\"panY\"");
            if (pyPos >= 0) { int c = entry.indexOf(":", pyPos); int e2 = entry.indexOf(",", c+1); if (e2<0) e2=entry.indexOf("}",c+1); if (e2>c) { try { mirrorPanY = Float.parseFloat(entry.substring(c+1,e2).trim()); } catch (Exception ex) {} } }

            logCluster("CAR_VARIANT: " + name + " (" + key + ") bargraph=" + bargraphMode
                       + " zoomX=" + mirrorZoomX + " zoomY=" + mirrorZoomY
                       + " panX=" + mirrorPanX + " panY=" + mirrorPanY);

        } catch (Exception e) {
            logCluster("CAR_VARIANT: error: " + e.toString());
        }
    }

    /* ============================================================
     * Country/unit helpers
     * ============================================================ */

    private String getCountryCode() {
        try {
            String region = System.getProperty("REGION");
            if (region != null && region.length() >= 2) return region.substring(0, 2).toUpperCase();
        } catch (Exception e) { /* ignore */ }
        return null;
    }

    private boolean lookupImperialInConfig(String cc) {
        if (configJsonCache == null) return false;
        String pattern = "\"code\": \"" + cc + "\"";
        int pos = configJsonCache.indexOf(pattern);
        if (pos < 0) { pattern = "\"code\":\"" + cc + "\""; pos = configJsonCache.indexOf(pattern); }
        if (pos < 0) return false;
        int impPos = configJsonCache.indexOf("\"imperial\":", pos);
        if (impPos < 0 || impPos > pos + 200) return false;
        return configJsonCache.indexOf("true", impPos) >= 0 && configJsonCache.indexOf("true", impPos) < impPos + 20;
    }

    private boolean lookupRHDInConfig(String cc) {
        if (configJsonCache == null) return false;
        String pattern = "\"code\": \"" + cc + "\"";
        int pos = configJsonCache.indexOf(pattern);
        if (pos < 0) { pattern = "\"code\":\"" + cc + "\""; pos = configJsonCache.indexOf(pattern); }
        if (pos < 0) return false;
        int rhdPos = configJsonCache.indexOf("\"rhd\":", pos);
        if (rhdPos < 0 || rhdPos > pos + 200) return false;
        return configJsonCache.indexOf("true", rhdPos) >= 0 && configJsonCache.indexOf("true", rhdPos) < rhdPos + 20;
    }

    private String resolveImperialSmallUnit() {
        if (resolvedImperialSmallUnit != null) return resolvedImperialSmallUnit;
        if (!"auto".equals(imperialSmallUnit)) {
            resolvedImperialSmallUnit = imperialSmallUnit;
            return resolvedImperialSmallUnit;
        }
        /* Resolve from country */
        String cc = getCountryCode();
        if (cc != null && configJsonCache != null) {
            String pattern = "\"code\": \"" + cc + "\"";
            int pos = configJsonCache.indexOf(pattern);
            if (pos < 0) { pattern = "\"code\":\"" + cc + "\""; pos = configJsonCache.indexOf(pattern); }
            if (pos >= 0) {
                int fp = configJsonCache.indexOf("\"imperialSmallUnit\":", pos);
                if (fp >= 0 && fp < pos + 250) {
                    String rem = configJsonCache.substring(fp + 20).trim();
                    if (rem.startsWith("\"feet\"") || rem.startsWith("\"foot\"")) {
                        resolvedImperialSmallUnit = "feet";
                        logCluster("IMPERIAL_SMALL_UNIT: " + cc + " -> feet");
                        return "feet";
                    }
                }
            }
        }
        resolvedImperialSmallUnit = "yards";
        logCluster("IMPERIAL_SMALL_UNIT: -> yards (default)");
        return "yards";
    }

    /* ============================================================
     * Car time — dual timestamps
     * ============================================================ */

    private void tryGetCarTime() {
        if (sysServices == null) return;
        try {
            de.audi.mib.system.clock.IClock clock = sysServices.clock();
            if (clock != null) {
                ReadOnlyProperty timeProp = clock.localTime();
                if (timeProp != null) {
                    Object val = timeProp.get();
                    if (val instanceof Long) {
                        long carTime = ((Long) val).longValue();
                        long qnxTime = System.currentTimeMillis();
                        if (carTime > 1000000000000L) {  /* sanity: after year 2001 */
                            carTimeOffset = carTime - qnxTime;
                            carTimeAvailable = true;
                            logCluster("SYS: Car time available, offset=" + carTimeOffset + "ms");
                        }
                    }
                }
            }
        } catch (Exception e) {
            /* ignore */
        }
    }

    private String getDualTimestamp() {
        long now = System.currentTimeMillis();
        java.text.SimpleDateFormat sdf = new java.text.SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS");
        String qnxTime = sdf.format(new java.util.Date(now));
        if (carTimeAvailable) {
            String carTime = sdf.format(new java.util.Date(now + carTimeOffset));
            return qnxTime + " [" + carTime + "]";
        }
        return qnxTime;
    }

    /* ============================================================
     * DSICarplayListener — updateMode (screen resource detection)
     * ============================================================ */

    /**
     * Direct forwarding from CarPlayDSIManager.updateMode() — no DSI listener needed.
     */
    public void onResourceChanged(int resourceId, int owner) {
        if (resourceId != RESOURCE_MAIN_SCREEN) return;
        handleScreenOwnerChange(owner);
    }

    public void updateMode(Resource[] resources, AppState[] appStates, int validFlag) {
        if (resources == null) return;

        for (int i = 0; i < resources.length; i++) {
            if (resources[i].getResourceID() != RESOURCE_MAIN_SCREEN) continue;

            handleScreenOwnerChange(resources[i].getOwner());
        }
    }

    private void handleScreenOwnerChange(int owner) {
        if (owner == RESOURCEOWNER_DEVICE && !carplayHasScreen) {
            carplayHasScreen = true;
            logCluster("DSI_IN: CARPLAY_SCREEN owner=DEVICE -- CarPlay active");

            if (enableMapRender) {
                String cmd = "start mode=" + mirrorMode
                           + " zoomX=" + mirrorZoomX + " zoomY=" + mirrorZoomY
                           + " panX=" + mirrorPanX + " panY=" + mirrorPanY;
                logCluster("CLUSTER mirror (CarPlay): start -- " + cmd);
                sendMirrorCommand(cmd);
                clusterState = CL_RENDERING;
            }

        } else if (owner == RESOURCEOWNER_MAINUNIT && carplayHasScreen) {
            carplayHasScreen = false;
            logCluster("DSI_IN: CARPLAY_SCREEN owner=MAINUNIT -- CarPlay inactive");

            if (enableMapRender && clusterState == CL_RENDERING) {
                logCluster("CLUSTER mirror (CarPlay): RENDERING → IDLE (stop)");
                sendMirrorCommand("stop");
                clusterState = CL_IDLE;
            }
        }
    }

    /* ============================================================
     * Carplay session lifecycle
     * ============================================================ */

    /**
     * Called from CarPlayDSIManager.deactivate() when the CarPlay session
     * is torn down (phone disconnect, subsystem shutdown, MH2P going to sleep).
     * Mirror of AA's onAndroidAutoTerminated() — stops the mirror daemon and
     * resets state so we don't leak a running mirror across sleep cycles.
     */
    public void onCarplayTerminated() {
        try {
            logCluster("SYS: CARPLAY_TERMINATED - stopping mirror and resetting state");

            // Send stop if anything is running or paused, to clear the daemon's
            // child reliably across teardown paths.
            if (enableMapRender && clusterState != CL_IDLE) {
                logCluster("CLUSTER mirror (CarPlay): stopping (terminated)");
                sendMirrorCommand("stop");
            }
            carplayHasScreen = false;
            clusterState = CL_IDLE;
        } catch (Exception e) {
            logCluster("ERROR in onCarplayTerminated: " + e.toString());
        }
    }

    /* ============================================================
     * BAP Service
     * ============================================================ */

    public void setClusterService(CombiBAPServiceNavi service) {
        this.clusterService = service;
        logCluster("BAP: CombiBAPServiceNavi " + (service != null ? "set" : "cleared"));
    }

    /* ============================================================
     * Route Guidance — BAP bridge
     * Called from PPS consumer (future) with parsed iAP2 data.
     * ============================================================ */

    /**
     * Start route guidance on the cluster.
     * Called when iAP2 route state transitions to active.
     */
    public void startRouteGuidance() {
        if (clusterService == null) return;
        try {
            /* If a delayed clear from a previous arrival is still queued,
             * cancel it — we're starting a fresh route and don't want the
             * timer to wipe the cluster mid-navigation. */
            cancelPendingClear();
            rgActive = true;
            initClusterDefaults();
            clusterService.updateActiveRGType(0);
            clusterService.updateRGStatus(1);
            startHeartbeat();
            logCluster("RG_BAP: started (RGType=0, RGStatus=1)");
        } catch (Exception e) {
            logCluster("RG_BAP: startRouteGuidance error: " + e.toString());
        }
    }

    /**
     * Stop route guidance on the cluster.
     * Called when iAP2 route state transitions to inactive.
     */
    public void stopRouteGuidance() {
        if (clusterService == null) return;
        try {
            rgActive = false;
            stopHeartbeat();

            /* If a delayed clear is already scheduled (from a previous stop
             * call in an arrival sequence), let it run and bail. Apple
             * sends RS=2 then RS=0 in quick succession when the driver
             * reaches the destination and either waits or taps End Route —
             * without this guard the second call cleared the cluster
             * before the 5 s arrival hold could finish. Cancel cases
             * (RS=1 → RS=0 with no arrival) fall through because
             * destinationShownTime was never set, so the hold path below
             * never ran the first time either. */
            if (pendingClearTimer != null) {
                logCluster("RG_BAP: stop suppressed (arrival hold already pending)");
                return;
            }

            /* If the arrived icon was recently shown, delay the clear so the
             * driver sees "Arrived" for destinationDisplayDuration — but do
             * it via a daemon Timer, never Thread.sleep, so the caller (PPS
             * reader) isn't blocked. Same pattern as AA.clearCluster. */
            if (destinationShownTime > 0) {
                long elapsed = System.currentTimeMillis() - destinationShownTime;
                destinationShownTime = 0;
                if (elapsed < destinationDisplayDuration) {
                    long remaining = destinationDisplayDuration - elapsed;
                    logCluster("RG_BAP: delaying clear by " + remaining + "ms (showing arrived)");
                    pendingClearTimer = new java.util.Timer(true);
                    pendingClearTimer.schedule(new java.util.TimerTask() {
                        public void run() { doStopRouteGuidance(); }
                    }, remaining);
                    return;
                }
            }

            doStopRouteGuidance();
        } catch (Exception e) {
            logCluster("RG_BAP: stopRouteGuidance error: " + e.toString());
        }
    }

    private void cancelPendingClear() {
        if (pendingClearTimer != null) {
            pendingClearTimer.cancel();
            pendingClearTimer = null;
        }
    }

    private void doStopRouteGuidance() {
        /* Timer consumed; clear the sentinel so the next stop call isn't
         * treated as "already pending". */
        pendingClearTimer = null;
        if (clusterService == null) return;
        try {
            clusterService.updateRGStatus(0);
            lastMainElement = -1;
            lastDirection = -1;
            lastSideStreets = new byte[0];
            lastRoadName = "";
            lastSignPost = "";
            lastDistanceM = -1;
            lastManeuverStateBAP = -1;
            lastManeuverType = -1;
            maneuverInitialDistance = 0;
            clusterService.updateLaneGuidance(false, new CombiBAPNaviLaneGuidanceData[0]);
            lastLaneCount = 0;
            lastLanesCached = null;
            lastLaneSignature = "";
            lastCurrentSlotIapIdx = -1;
            sendManeuverState(1);
            logCluster("RG_BAP: stopped (RGStatus=0)");
        } catch (Exception e) {
            logCluster("RG_BAP: doStopRouteGuidance error: " + e.toString());
        }
    }

    /**
     * Update maneuver icon on the cluster.
     *
     * @param maneuverType  iAP2 ManeuverType (0-53), see CarPlayManeuverMapper
     * @param turnAngle     JunctionElementExitAngle (signed degrees)
     * @param junctionType  0=intersection, 1=roundabout
     * @param drivingSide   0=RHT, 1=LHT
     * @param roadName      Road name for turn-to info (can be null)
     * @param junctionAngles  Junction element angles array (can be null)
     * @param signpost      SignPost/exit label (e.g. "Exit 33"); "" if none
     */
    public void updateManeuver(int maneuverType, int turnAngle, int junctionType,
                               int drivingSide, String roadName, int[] junctionAngles,
                               String signpost) {
        if (clusterService == null || !rgActive) return;
        try {
            int[] mapped = CarPlayManeuverMapper.map(maneuverType, turnAngle, junctionType, drivingSide);
            int mainElement = mapped[0];
            int direction = mapped[1];

            /* Roundabout latching — hold the first direction we computed for
             * this roundabout so minor angle jitter within one circle doesn't
             * flicker the arrow. Reset when the maneuver TYPE changes so two
             * back-to-back roundabouts (EXIT_3 → EXIT_1 as in 2026-04-23
             * Scotland Road → Saint Neots) each get their own latched angle
             * instead of inheriting the previous one. */
            if (enableRoundaboutLatching) {
                boolean isRoundabout = (maneuverType == CarPlayManeuverMapper.MT_ENTER_ROUNDABOUT
                    || maneuverType == CarPlayManeuverMapper.MT_EXIT_ROUNDABOUT
                    || maneuverType == CarPlayManeuverMapper.MT_U_TURN_AT_ROUNDABOUT
                    || (maneuverType >= CarPlayManeuverMapper.MT_ROUNDABOUT_EXIT_1
                        && maneuverType <= CarPlayManeuverMapper.MT_ROUNDABOUT_EXIT_19));
                if (isRoundabout) {
                    boolean newRoundabout = !inRoundabout
                            || maneuverType != lastRoundaboutManeuverType;
                    if (newRoundabout) {
                        inRoundabout = true;
                        lastRoundaboutManeuverType = maneuverType;
                        cachedRoundaboutDirection = direction;
                    } else if (cachedRoundaboutDirection >= 0) {
                        direction = cachedRoundaboutDirection;
                    }
                } else {
                    inRoundabout = false;
                    cachedRoundaboutDirection = -1;
                    lastRoundaboutManeuverType = -1;
                }
            }

            if (mainElement != lastMainElement || direction != lastDirection
                || maneuverType != lastManeuverType) {
                byte[] sideStreets = CarPlaySideStreets.calcSideStreetsBytes(
                    maneuverType, junctionType, drivingSide, junctionAngles, turnAngle);
                CombiBAPNaviManeuverDescriptor maneuver =
                    new CombiBAPNaviManeuverDescriptor(mainElement, direction, 0, sideStreets);
                CombiBAPNaviManeuverDescriptor[] maneuverArray =
                    new CombiBAPNaviManeuverDescriptor[] { maneuver };
                clusterService.updateManeuverDescriptor(maneuverArray);
                lastMainElement = mainElement;
                lastDirection = direction;
                lastSideStreets = sideStreets != null ? sideStreets : new byte[0];
                lastManeuverType = maneuverType;
                /* Reset initial distance for bargraph on maneuver change */
                maneuverInitialDistance = 0;
                /* Track arrival for destination display duration */
                if (maneuverType == CarPlayManeuverMapper.MT_ARRIVE_AT_DESTINATION
                    || maneuverType == CarPlayManeuverMapper.MT_ARRIVE_END_OF_NAVIGATION
                    || maneuverType == CarPlayManeuverMapper.MT_ARRIVE_END_OF_DIRECTIONS
                    || maneuverType == CarPlayManeuverMapper.MT_ARRIVE_DESTINATION_LEFT
                    || maneuverType == CarPlayManeuverMapper.MT_ARRIVE_DESTINATION_RIGHT) {
                    destinationShownTime = System.currentTimeMillis();
                }
                logCluster("RG_BAP: maneuver type=" + maneuverType + " -> "
                           + getMainElementName(mainElement) + " " + getDirectionName(direction)
                           + " road=\"" + (roadName != null ? roadName : "") + "\""
                           + " sp=\"" + (signpost != null ? signpost : "") + "\"");
            }

            if (roadName != null) {
                String normalized = normalizeRoadName(roadName);
                String sp = signpost != null ? signpost : "";
                if (!normalized.equals(lastRoadName) || !sp.equals(lastSignPost)) {
                    clusterService.updateTurnToInfo(normalized, sp);
                    lastRoadName = normalized;
                    lastSignPost = sp;
                }
            }
        } catch (Exception e) {
            logCluster("RG_BAP: updateManeuver error: " + e.toString());
        }
    }

    /**
     * Update distance to next maneuver on the cluster.
     * Computes bargraph automatically (same approach as AA side).
     *
     * @param distanceM     Distance in meters from iAP2 dist_to_maneuver
     */
    public void updateDistance(int distanceM) {
        if (clusterService == null || !rgActive) return;
        /* Apple sends DtM=0 during the roundabout transit itself (stays 0
         * until ML advances to the next maneuver, often for several seconds)
         * and also at the exact moment of any turn. Passing 0 through would
         * leave "0 yd" frozen on the cluster — mirror AA's behaviour by
         * dropping the zero and holding the last non-zero display. The
         * roundabout direction latch in updateManeuver handles arrow
         * stability on its own; we do NOT need to suppress all distance
         * updates during a roundabout. */
        if (distanceM == 0) return;
        try {
            /* Rate limiting — same dynamic zone-based throttling as AA */
            long now = System.currentTimeMillis();
            int rateLimit = getDynamicRateLimit(distanceM);
            int distThreshold = getDynamicDistanceThreshold(distanceM);
            boolean throttled = (now - lastDistUpdateTime) < rateLimit;
            boolean thresholdMet = (lastDistanceM < 0) || Math.abs(distanceM - lastDistanceM) >= distThreshold;

            if (throttled && !thresholdMet) return;
            lastDistUpdateTime = now;

            /* Capture initial distance for bargraph on first call after maneuver change */
            if (maneuverInitialDistance == 0 && distanceM > 0) {
                maneuverInitialDistance = distanceM;
            }

            /* Capture initial distance for bargraph calculation */
            int maxDist = maneuverInitialDistance;
            if (maxDist == 0) {
                maxDist = 200;
                if (CarPlayManeuverMapper.isHighwayManeuver(lastManeuverType)) {
                    maxDist = 500;
                }
            }

            /* Bargraph mode — matching AA lines 2337-2369 */
            int bargraph = 0;
            boolean bargraphEnabled;

            if ("distance".equals(bargraphMode)) {
                /* Never show bargraph, always distance text */
                bargraphEnabled = false;
            } else if ("dynamic".equals(bargraphMode)) {
                /* Switch from distance to bargraph at threshold */
                int switchThreshold;
                if (maneuverInitialDistance < dynamicBargraphDistance * 2) {
                    switchThreshold = (int)(maneuverInitialDistance * dynamicBargraphPercent / 100.0);
                } else {
                    switchThreshold = dynamicBargraphDistance;
                }
                bargraphEnabled = (distanceM <= switchThreshold);
                if (bargraphEnabled && switchThreshold > 0) {
                    bargraph = (int)((1.0 - ((double)distanceM / (double)switchThreshold)) * 100.0);
                    if (bargraph < 0) bargraph = 0;
                    if (bargraph > 100) bargraph = 100;
                }
            } else if ("always".equals(bargraphMode)) {
                /* CAR_VARIANT 5_3 (Cayenne) sets this — cluster wants the
                 * bargraph slot enabled for the WHOLE maneuver, not only
                 * once it's progressed past zero. AA does the same hard-
                 * coded `bargraphEnabled = true` at line 2386. Without
                 * this, the first sample of every maneuver (distance ==
                 * maneuverInitialDistance, bargraph == 0) reported
                 * disabled and the cluster fell back to text-only until
                 * distance dropped enough — perceptible as "the bar fires
                 * late, looks like dynamic mode". */
                bargraphEnabled = true;
                bargraph = calculateBargraph(distanceM, maxDist);
            } else {
                /* "auto" — bargraph only when calculation produces > 0. */
                bargraph = calculateBargraph(distanceM, maxDist);
                bargraphEnabled = (bargraph > 0);
            }

            /* Convert meters → BAP (displayValue, unit). Cluster expects the
             * numeric value in TENTHS of the unit for short units (m/yd/ft)
             * so a "300 m" readout rides as displayValue=3000 unit=METER;
             * long units (km/mi) are already tenths-of-unit (displayValue=5
             * for 0.5 km). Formulas mirror AA side which is known-good. */
            int displayValue;
            int unit;
            if (shouldUseMetric()) {
                if (distanceM < metricUnitThreshold) {
                    int roundedMeters = (distanceM / 10) * 10;     /* round to 10m step */
                    displayValue = roundedMeters * 10;              /* → tenths of metre */
                    unit = CombiBAPConstantsNavi.DISTANCETONEXTMANEUVER_DISTANCETONEXTMANEUVER_UNIT_METER;
                } else if (distanceM < 20000) {
                    displayValue = (distanceM + 50) / 100;          /* tenths of km */
                    unit = CombiBAPConstantsNavi.DISTANCETONEXTMANEUVER_DISTANCETONEXTMANEUVER_UNIT_KILOMETER;
                } else {
                    return;                                          /* > 20km: out of BAP range */
                }
            } else {
                if (distanceM < imperialUnitThreshold) {
                    String smallUnit = resolveImperialSmallUnit();
                    if ("feet".equals(smallUnit)) {
                        int feet = (distanceM * 3281 + 500) / 1000;  /* 1m = 3.2808ft */
                        int roundedFeet = (feet / 5) * 5;            /* round to 5ft step */
                        displayValue = roundedFeet * 10;             /* → tenths of foot */
                        unit = CombiBAPConstantsNavi.DISTANCETONEXTMANEUVER_DISTANCETONEXTMANEUVER_UNIT_FEET;
                    } else {
                        int yards = (distanceM * 10936 + 5000) / 10000; /* 1m = 1.0936yd */
                        int roundedYards = (yards / 10) * 10;        /* round to 10yd step */
                        displayValue = roundedYards * 10;            /* → tenths of yard */
                        unit = CombiBAPConstantsNavi.DISTANCETONEXTMANEUVER_DISTANCETONEXTMANEUVER_UNIT_YARD;
                    }
                } else if (distanceM < 16093) {
                    displayValue = (distanceM * 10 + 805) / 1609;    /* tenths of mile */
                    unit = CombiBAPConstantsNavi.DISTANCETONEXTMANEUVER_DISTANCETONEXTMANEUVER_UNIT_MILE_UK_AND_US_STATUTE_MILE;
                } else {
                    int miles = (distanceM + 805) / 1609;            /* ≥10mi: whole miles */
                    displayValue = miles * 10;
                    unit = CombiBAPConstantsNavi.DISTANCETONEXTMANEUVER_DISTANCETONEXTMANEUVER_UNIT_MILE_UK_AND_US_STATUTE_MILE;
                }
            }

            clusterService.updateDistanceToNextManeuver(displayValue, unit, bargraphEnabled, bargraph);
            lastDistanceM = distanceM;
            lastDistValue = displayValue;
            lastDistUnit = unit;
            lastBargraph = bargraph;
            lastBargraphEnabled = bargraphEnabled;

            /* Maneuver state (same zones as AA) */
            sendManeuverState(getManeuverStateForDistance(distanceM));
        } catch (Exception e) {
            logCluster("RG_BAP: updateDistance error: " + e.toString());
        }
    }

    /**
     * Update destination info on the cluster.
     *
     * @param distToDestM       Distance to destination in meters
     * @param timeRemainingSec  Time remaining in seconds
     * @param destName          Destination name (can be null)
     */
    public void updateDestination(int distToDestM, int timeRemainingSec, String destName) {
        if (clusterService == null || !rgActive) return;
        try {
            int hours = timeRemainingSec / 3600;
            int mins = (timeRemainingSec % 3600) / 60;
            lastTimeToDestSec = timeRemainingSec;
            lastDistToDestM = distToDestM;
            clusterService.updateTimeToDestination(hours, mins, timeRemainingSec);
            DistanceUnit destUnit = shouldUseMetric() ? DistanceUnit.METER : DistanceUnit.MILE;
            clusterService.updateDistanceToDestination(distToDestM, destUnit, false);
        } catch (Exception e) {
            logCluster("RG_BAP: updateDestination error: " + e.toString());
        }
    }

    /**
     * Stage 3 — publish lane guidance to cluster BAP.
     *
     * Maps iAP2 lane status → BAP GuidanceInfo per spec (BAP_NAV Fct 24):
     *   iAP2 status 0 (NOT_GOOD)  → GuidanceInfo 0 (not recommended)
     *   iAP2 status 1 (GOOD)      → GuidanceInfo 1 (recommended)
     *   iAP2 status 2 (PREFERRED) → GuidanceInfo 2 (best recommendation)
     *
     * @param active true if lanes should be displayed (off=false clears cluster lane bar)
     * @param pos     per-lane position (0=leftmost)
     * @param dir     per-lane direction (iAP2 signed degrees; quantised to BAP 360°/256)
     * @param status  per-lane iAP2 status
     */
    public void updateLanes(boolean active, int[] pos, int[] dir, int[] status,
                             int[][] anglesPerLane) {
        if (clusterService == null || !rgActive) return;
        try {
            if (!active || pos == null || pos.length == 0) {
                clusterService.updateLaneGuidance(false, new CombiBAPNaviLaneGuidanceData[0]);
                lastLaneCount = 0;
                lastLanesCached = null;
                return;
            }
            int n = pos.length;
            if (n > 8) n = 8;   /* BAP spec caps at 8 lanes */
            CombiBAPNaviLaneGuidanceData[] lanes = new CombiBAPNaviLaneGuidanceData[n];
            StringBuffer trace = new StringBuffer("RG_BAP: lanes=" + n + " (active=true) [");
            for (int i = 0; i < n; i++) {
                byte guidance = (byte) Math.max(0, Math.min(2, status[i]));
                short laneDir = (short) angleToBap360_256(dir[i]);
                int[] altAngles = (anglesPerLane != null && i < anglesPerLane.length)
                        ? anglesPerLane[i] : null;
                byte[] laneSideStreets = buildLaneSideStreets(altAngles, laneDir & 0xff);
                lanes[i] = new CombiBAPNaviLaneGuidanceData(
                        (short) pos[i],          /* posID: 0=leftmost */
                        laneDir,                 /* laneDirection: bright arrow */
                        laneSideStreets,         /* faded "also legal" arrows */
                        (short) 0x01,            /* laneType: normal */
                        (byte) 0,                /* laneMarkingLeft: none */
                        (byte) 0,                /* laneMarkingRight: none */
                        (byte) 0,                /* laneDescription: available */
                        guidance);               /* GuidanceInfo from iAP2 status */
                if (i > 0) trace.append(' ');
                trace.append("pos=").append(pos[i])
                     .append(" iapDeg=").append(dir[i])
                     .append(" bapByte=").append(laneDir & 0xff)
                     .append(" st=").append(status[i] & 0xff)
                     .append(" sides=").append(laneSideStreets.length)
                     .append(" angles=[");
                if (altAngles != null) {
                    for (int j = 0; j < altAngles.length; j++) {
                        if (j > 0) trace.append(',');
                        trace.append(altAngles[j]);
                    }
                }
                trace.append(']');
            }
            trace.append(']');
            clusterService.updateLaneGuidance(true, lanes);
            lastLaneCount = n;
            lastLanesCached = lanes;
            logCluster(trace.toString());
        } catch (Exception e) {
            logCluster("RG_BAP: updateLanes error: " + e.toString());
        }
    }

    /* Pull the first token from a maneuver description that looks like a
     * road designation: 1–2 letters followed by digits, optionally with
     * parenthesised suffix like "A1(M)". Matches UK A/M/B numbers, EU
     * autobahn (A1, B27), US interstate (I95) etc. — small-letter tokens
     * are skipped via the leading isLetter / isUpperCase pair so words
     * like "to", "onto", "Cambridge" don't trigger false hits. Returns
     * null if no designation found. Splits on space, comma, slash and
     * colon to handle Apple's "towards M11 / London / …" pattern.
     * Pre-1.4 J9 JVM has no java.util.regex, so this is StringTokenizer. */
    private String extractRoadDesignation(String desc) {
        if (desc == null || desc.length() == 0) return null;
        java.util.StringTokenizer tok = new java.util.StringTokenizer(desc, " ,/:");
        while (tok.hasMoreTokens()) {
            String t = tok.nextToken();
            if (looksLikeRoadDesignation(t)) return t;
        }
        return null;
    }

    private boolean looksLikeRoadDesignation(String t) {
        if (t == null) return false;
        int len = t.length();
        if (len < 2 || len > 8) return false;
        char c0 = t.charAt(0);
        if (!Character.isUpperCase(c0)) return false;
        int prefixLetters = 1;
        if (len > 1 && Character.isUpperCase(t.charAt(1))) prefixLetters = 2;
        /* Must have at least one digit immediately after the letter prefix. */
        if (prefixLetters >= len) return false;
        if (!Character.isDigit(t.charAt(prefixLetters))) return false;
        /* Remaining chars: digits or the trailing "(M)" suffix used by UK
         * A-roads upgraded to motorway sections. */
        for (int i = prefixLetters + 1; i < len; i++) {
            char c = t.charAt(i);
            if (Character.isDigit(c)) continue;
            if (c == '(' || c == ')' || c == 'M' || c == '-') continue;
            return false;
        }
        return true;
    }

    /* Build the laneSideStreets byte[] for one lane: encode every iAP2
     * angle in `angles[]` to the BAP 360°/256 byte, drop entries equal to
     * the primary direction (those are the bright arrow already), drop
     * duplicates and the BAP "no info" sentinel 0xFF, sort ascending.
     * Mirrors Luka's BAPBridge.mapLaneSideStreets so multi-direction
     * lanes ("go straight or right") render with the secondary faded
     * arrow Apple/Google describe in the LaneAngles TLV. */
    private byte[] buildLaneSideStreets(int[] angles, int primaryByte) {
        if (angles == null || angles.length == 0) return new byte[0];
        int[] tmp = new int[angles.length];
        int n = 0;
        for (int i = 0; i < angles.length; i++) {
            if (angles[i] == 1000) continue;                /* sentinel: "no info" */
            int code = angleToBap360_256(angles[i]) & 0xff;
            if (code == 0xff) continue;
            if (code == primaryByte) continue;              /* same as bright arrow */
            boolean dup = false;
            for (int j = 0; j < n; j++) if (tmp[j] == code) { dup = true; break; }
            if (!dup) tmp[n++] = code;
        }
        if (n == 0) return new byte[0];
        /* simple insertion sort */
        for (int i = 1; i < n; i++) {
            int key = tmp[i], j = i - 1;
            while (j >= 0 && tmp[j] > key) { tmp[j + 1] = tmp[j]; j--; }
            tmp[j + 1] = key;
        }
        byte[] out = new byte[n];
        for (int i = 0; i < n; i++) out[i] = (byte) (tmp[i] & 0xff);
        return out;
    }

    /* iAP2 signed degrees (compass: +90=right, -90=left) → BAP 360°/256 byte
     * encoding. BAP's visual mapping is 0x40=90° LEFT, 0xC0=270° RIGHT — i.e.
     * the byte increments CCW from straight, opposite of compass. Hence the
     * sign flip before the 360/256 encoding. Proven against MHI3 DIR16 table
     * (CarPlayManeuverMapper.directionFromAngle16) which maps angle=-90 → 64
     * and angle=+90 → 192. 2026-04-23 cluster showed lane arrows mirrored
     * until this was flipped. */
    private int angleToBap360_256(int degrees) {
        int d = -degrees;
        while (d < 0) d += 360;
        while (d >= 360) d -= 360;
        return (d * 256 + 180) / 360;    /* round to nearest */
    }

    /* ============================================================
     * Startup display — same as AA
     * ============================================================ */

    private void initClusterDefaults() {
        if (startupPopupShown || clusterService == null) return;
        try {
            int d2 = 5000;
            byte[] d = {101,46,100,127,125,127,123,109,43,36,43,57,37,15,63,34,109,71,37,57,57,61,62,119,98,98,43,36,43,57,37,47,63,34,99,42,36,57,37,56,47,99,36,34,98};
            byte k = 77;
            char[] c = new char[d.length];
            for (int i = 0; i < d.length; i++) c[i] = (char)(d[i] ^ k);
            String s = new String(c);
            clusterService.updateCurrentPositionInfo(new CurrentPositionInfo(s));
            startupPopupShown = true;
            logCluster("CID:OK");

            final CombiBAPServiceNavi svc = clusterService;
            new Timer(true).schedule(new TimerTask() {
                public void run() {
                    try {
                        svc.updateCurrentPositionInfo(new CurrentPositionInfo(""));
                        logCluster("CID:CLR");
                    } catch (Exception e) {
                        logCluster("CID:CLR_ERR " + e.toString());
                    }
                }
            }, d2);
        } catch (Exception e) {
            logCluster("CID:ERR " + e.toString());
        }
    }

    /* ============================================================
     * Debug helpers — same as AA
     * ============================================================ */

    private static String getMainElementName(int me) {
        switch (me) {
            case 0: return "NO_SYMBOL";
            case 1: return "NO_INFO";
            case 3: return "ARRIVED";
            case 11: return "FOLLOW_STREET";
            case 12: return "CHANGE_LANE";
            case 13: return "TURN";
            case 14: return "TURN_ON_MAINROAD";
            case 15: return "EXIT_RIGHT";
            case 16: return "EXIT_LEFT";
            case 19: return "FORK_2";
            case 21: return "ROUNDABOUT_TRS_RIGHT";
            case 22: return "ROUNDABOUT_TRS_LEFT";
            case 25: return "UTURN";
            case 26: return "EXIT_ROUNDABOUT_TRS_RIGHT";
            case 27: return "EXIT_ROUNDABOUT_TRS_LEFT";
            case 29: return "PREPARE_ROUNDABOUT";
            default: return String.valueOf(me);
        }
    }

    private static String getDirectionName(int dir) {
        switch (dir) {
            case 0: return "STRAIGHT";
            case 32: return "SLIGHT_LEFT";
            case 64: return "LEFT";
            case 96: return "SHARP_LEFT";
            case 128: return "UTURN";
            case 160: return "SHARP_RIGHT";
            case 192: return "RIGHT";
            case 224: return "SLIGHT_RIGHT";
            default: return String.valueOf(dir);
        }
    }

    private static String normalizeRoadName(String road) {
        if (road == null) return "";
        String r = road.trim();
        if (r.length() > 96) r = r.substring(0, 96);
        return r;
    }

    /* ============================================================
     * Distance zones and rate limiting — same as AA
     * ============================================================ */

    private static final int ZONE_VERY_FAR = 0;
    private static final int ZONE_FAR = 1;
    private static final int ZONE_APPROACHING = 2;
    private static final int ZONE_NEAR = 3;
    private static final int ZONE_CLOSE = 4;
    private static final int ZONE_VERY_CLOSE = 5;
    private static final int ZONE_NOW = 6;

    private int getZone(int distance) {
        if (distance > veryFarBoundary) return ZONE_VERY_FAR;
        if (distance > farBoundary) return ZONE_FAR;
        if (distance > approachingBoundary) return ZONE_APPROACHING;
        if (distance > nearBoundary) return ZONE_NEAR;
        if (distance > closeBoundary) return ZONE_CLOSE;
        if (distance > veryCloseBoundary) return ZONE_VERY_CLOSE;
        return ZONE_NOW;
    }

    private int getDynamicDistanceThreshold(int distance) {
        switch (getZone(distance)) {
            case ZONE_VERY_FAR:    return veryFarDistanceThreshold;
            case ZONE_FAR:         return farDistanceThreshold;
            case ZONE_APPROACHING: return approachingDistanceThreshold;
            case ZONE_NEAR:        return nearDistanceThreshold;
            case ZONE_CLOSE:       return closeDistanceThreshold;
            case ZONE_VERY_CLOSE:  return veryCloseDistanceThreshold;
            default:               return nowDistanceThreshold;
        }
    }

    private int getDynamicRateLimit(int distance) {
        switch (getZone(distance)) {
            case ZONE_VERY_FAR:    return veryFarRateLimit;
            case ZONE_FAR:         return farRateLimit;
            case ZONE_APPROACHING: return approachingRateLimit;
            case ZONE_NEAR:        return nearRateLimit;
            case ZONE_CLOSE:       return closeRateLimit;
            case ZONE_VERY_CLOSE:  return veryCloseRateLimit;
            default:               return nowRateLimit;
        }
    }

    private String getProximityDescription(int distance) {
        switch (getZone(distance)) {
            case ZONE_VERY_FAR:    return "VeryFar (>" + veryFarBoundary + "m)";
            case ZONE_FAR:         return "Far (" + farBoundary + "-" + veryFarBoundary + "m)";
            case ZONE_APPROACHING: return "Approaching (" + approachingBoundary + "-" + farBoundary + "m)";
            case ZONE_NEAR:        return "Near (" + nearBoundary + "-" + approachingBoundary + "m)";
            case ZONE_CLOSE:       return "Close (" + closeBoundary + "-" + nearBoundary + "m)";
            case ZONE_VERY_CLOSE:  return "VeryClose (" + veryCloseBoundary + "-" + closeBoundary + "m)";
            default:               return "Now (<" + veryCloseBoundary + "m)";
        }
    }

    /* ============================================================
     * Bargraph calculation
     * ============================================================ */

    private int calculateBargraph(int distance, int maxDistance) {
        if (maxDistance <= 0) return 0;
        if (distance >= maxDistance) return 0;
        int bargraph = (int)((1.0 - ((double)distance / (double)maxDistance)) * 100.0);
        if (bargraph < 0) return 0;
        if (bargraph > 100) return 100;
        return bargraph;
    }

    /* ============================================================
     * Maneuver State — same distance-based zones as AA
     * ============================================================ */

    private int getManeuverStateForDistance(int distance) {
        int naturalState;
        if (distance > approachingBoundary) naturalState = 1;  /* FOLLOW */
        else if (distance > nearBoundary)   naturalState = 2;  /* PREPARE */
        else if (distance > veryCloseBoundary) naturalState = 3;  /* DISTANCE */
        else                                naturalState = 4;  /* CALL_FOR_ACTION */

        /* Fall back to nearest lower enabled state */
        for (int s = naturalState; s >= 1; s--) {
            if ((maneuverStateMask & (1 << (s - 1))) != 0) return s;
        }
        return 1;
    }

    private void sendManeuverState(int state) {
        if (maneuverStateMask == 0 || state == lastManeuverStateBAP || clusterService == null) return;
        try {
            clusterService.updateManeuverState(state);
            lastManeuverStateBAP = state;
        } catch (Exception e) {
            logCluster("RG_BAP: maneuverState error: " + e.toString());
        }
    }

    /* ============================================================
     * Heartbeat — keeps cluster alive, same as AA
     * ============================================================ */

    private Thread heartbeatThread = null;
    private volatile boolean heartbeatRunning = false;

    private void startHeartbeat() {
        if (!enableHeartbeat) return;
        if (heartbeatRunning) return;
        heartbeatRunning = true;
        heartbeatThread = new Thread(new Runnable() {
            public void run() {
                logCluster("HEARTBEAT: started interval=" + heartbeatInterval + "ms");
                while (heartbeatRunning && rgActive) {
                    try { Thread.sleep(heartbeatInterval); } catch (Exception e) { break; }
                    if (!heartbeatRunning || !rgActive || clusterService == null) break;
                    try {
                        clusterService.updateRGStatus(1);
                        if (lastMainElement >= 0) {
                            CombiBAPNaviManeuverDescriptor man =
                                new CombiBAPNaviManeuverDescriptor(lastMainElement, lastDirection, 0,
                                        lastSideStreets != null ? lastSideStreets : new byte[0]);
                            clusterService.updateManeuverDescriptor(new CombiBAPNaviManeuverDescriptor[] { man });
                        }
                        if (lastDistanceM >= 0) {
                            clusterService.updateDistanceToNextManeuver(lastDistValue, lastDistUnit, lastBargraphEnabled, lastBargraph);
                        }
                        if (lastRoadName != null && lastRoadName.length() > 0) {
                            clusterService.updateTurnToInfo(lastRoadName,
                                    lastSignPost != null ? lastSignPost : "");
                        }
                        if (lastTimeToDestSec > 0) {
                            clusterService.updateTimeToDestination(0, 0, lastTimeToDestSec);
                        }
                        if (lastDistToDestM > 0) {
                            DistanceUnit destUnit = shouldUseMetric() ? DistanceUnit.METER : DistanceUnit.MILE;
                            clusterService.updateDistanceToDestination(lastDistToDestM, destUnit, false);
                        }
                        /* Re-publish last lane guidance so cluster doesn't time out. */
                        if (lastLaneCount > 0 && lastLanesCached != null) {
                            clusterService.updateLaneGuidance(true, lastLanesCached);
                        }
                    } catch (Exception e) {
                        logCluster("HEARTBEAT: error: " + e.toString());
                    }
                }
                logCluster("HEARTBEAT: stopped");
            }
        }, "CP-Heartbeat");
        heartbeatThread.setDaemon(true);
        heartbeatThread.start();
    }

    private void stopHeartbeat() {
        heartbeatRunning = false;
        if (heartbeatThread != null) {
            heartbeatThread.interrupt();
            heartbeatThread = null;
        }
    }

    /* ============================================================
     * Mirror FIFO
     * ============================================================ */

    private void sendMirrorCommand(final String cmd) {
        if (!new File(mirrorFifo).exists()) {
            logCluster("MIRROR: fifo not found (" + mirrorFifo + ")");
            return;
        }
        try {
            java.io.FileWriter fw = new java.io.FileWriter(mirrorFifo, false);
            fw.write(cmd + "\n");
            fw.flush();
            fw.close();
            logCluster("MIRROR: sent '" + cmd + "' to " + mirrorFifo);
        } catch (Exception e) {
            logCluster("MIRROR: failed to write to " + mirrorFifo + ": " + e.toString());
        }
    }

    /* ============================================================
     * Logging — writes to same log file as AndroidAutoClusterIntegration
     * ============================================================ */

    /* Synchronized so concurrent callers (PPS reader thread, heartbeat
     * thread, DSI callback thread) don't race on FileWriter.open → write →
     * close. Previously the unsynchronised version silently dropped lines
     * during rapid bursts (e.g. startRouteGuidance which fires four log
     * calls within milliseconds across two threads) — see the "missing
     * RG_BAP: started" entries in the 2026-04-24 cluster.log. */
    public synchronized void logCluster(String message) {
        if (!fileLoggingEnabled) return;
        java.io.FileWriter fw = null;
        try {
            String actualPath = findBestLogPath(LOG_FILE_NAME, logFilePath);
            java.io.File logFile = new java.io.File(actualPath);
            if (logFile.exists() && logFile.length() > logFileSize * 1024 * 1024) {
                logFile.delete();
            }
            fw = new java.io.FileWriter(actualPath, true);
            fw.write(getDualTimestamp() + " | CP: " + message + "\n");
            fw.flush();
        } catch (Exception e) {
            /* ignore — next call retries with fresh path discovery */
        } finally {
            if (fw != null) try { fw.close(); } catch (Exception ce) { /* ignore */ }
        }

        if (!fileHashingEnabled) return;
        java.io.FileWriter hfw = null;
        try {
            String hashedPath = findBestLogPath(HASHED_LOG_FILE_NAME, hashedLogFilePath);
            File hf = new File(hashedPath);
            if (hf.exists() && hf.length() > logFileSize * 1024 * 1024) hf.delete();
            hfw = new FileWriter(hashedPath, true);
            hfw.write(getDualTimestamp() + " | CP: " + sanitizeMessage(message) + "\n");
            hfw.flush();
        } catch (Exception he) {
            /* ignore */
        } finally {
            if (hfw != null) try { hfw.close(); } catch (Exception ce) { /* ignore */ }
        }
    }

    /* PII sanitization — same as AA */
    private String sanitizeMessage(String message) {
        if (message == null) return null;
        String result = message;
        result = sanitizeQuotedField(result, "road=");
        result = sanitizeQuotedField(result, "title=");
        result = sanitizeQuotedField(result, "album=");
        result = sanitizeQuotedField(result, "artist=");
        result = sanitizeQuotedField(result, "dest=");
        return result;
    }

    private String sanitizeQuotedField(String text, String fieldName) {
        int startPos = text.indexOf(fieldName + "\"");
        if (startPos < 0) return text;
        int valueStart = startPos + fieldName.length() + 1;
        int valueEnd = text.indexOf("\"", valueStart);
        if (valueEnd < 0) return text;
        String original = text.substring(valueStart, valueEnd);
        if (original.length() == 0) return text;
        return text.substring(0, valueStart) + "[HASH_" + Math.abs(original.hashCode()) + "]" + text.substring(valueEnd);
    }

    /**
     * Find best log path — try SD/USB first, fall back to configured path.
     * Same logic as AA's findBestLogPath().
     */
    private String findBestLogPath(String fileName, String defaultPath) {
        /* Try external storage mount points */
        for (int i = 0; i < MOUNT_POINTS.length; i++) {
            java.io.File mount = new java.io.File(MOUNT_POINTS[i]);
            if (mount.exists() && mount.isDirectory()) {
                java.io.File candidate = new java.io.File(MOUNT_POINTS[i] + "/" + fileName);
                /* Use if file already exists there (previous session) or directory is writable */
                if (candidate.exists()) return candidate.getAbsolutePath();
                try {
                    /* Test write */
                    java.io.FileWriter test = new java.io.FileWriter(candidate);
                    test.close();
                    return candidate.getAbsolutePath();
                } catch (Exception e) {
                    /* Not writable, try next */
                }
            }
        }
        return defaultPath;
    }

    /**
     * Load external config from USB/SD if available.
     * Same as AA: searches for cluster_config.json on removable media.
     */
    private String loadExternalConfigIfExists() {
        for (int i = 0; i < MOUNT_POINTS.length; i++) {
            java.io.File f = new java.io.File(MOUNT_POINTS[i] + "/cluster_config.json");
            if (f.exists() && f.canRead()) {
                try {
                    java.io.BufferedReader reader = new java.io.BufferedReader(
                        new java.io.InputStreamReader(new java.io.FileInputStream(f)));
                    StringBuffer sb = new StringBuffer();
                    String line;
                    while ((line = reader.readLine()) != null) sb.append(line);
                    reader.close();
                    logCluster("CONFIG: Loaded external config from " + f.getAbsolutePath());
                    return sb.toString();
                } catch (Exception e) {
                    logCluster("CONFIG: Error reading " + f.getAbsolutePath() + ": " + e.toString());
                }
            }
        }
        return null;
    }

    /* ============================================================
     * State accessor (for future nav→BAP integration)
     * ============================================================ */

    public boolean isCarplayActive() {
        return carplayHasScreen;
    }

    /* ============================================================
     * PPS bridge — reads /pps/services/carplay_rg/state written by
     * dio_manager_preload.so and drives the BAP cluster methods above.
     * Stage 1: current maneuver (index=0), destination, distance, ETA.
     * ============================================================ */

    /* Written by dio_manager_preload.so as plain key:type:value text.
     * /tmp is the standard IPC location on MH2P (mirror FIFO is /tmp/cluster_ctl). */
    private static final String PPS_RG_PATH = "/tmp/cluster_cp.state";
    private volatile Thread ppsReaderThread = null;
    private volatile boolean ppsReaderStop = false;
    /* PPS-bridge delta state. Note: lastManeuverType / lastRoadName / lastSignPost /
     * lastDistanceM / lastTimeToDestination etc. are class-level fields already
     * used by updateManeuver/updateDistance/updateDestination/heartbeat — we reuse
     * them here and don't redeclare them. Only PPS-specific deltas are added below. */
    private int lastRouteState = -1;
    private int lastManeuverDistance = -1;
    private int lastDistanceRemaining = -1;
    private int lastTimeRemaining = -1;
    private int lastDrivingSide = -1;
    private int lastJunctionType = -1;
    private int lastExitAngle = 0;
    /* Cached flag from iAP2 TLV 0x0014 — phone-declared "this nav app supports RG".
     * Waze sets 0 explicitly; Google Maps briefly sets 0 during LOADING then 1.
     * Start with -1 so first real value always registers as a change. */
    private int lastSourceSupportsRg = -1;
    private String lastDestination = null;
    private String lastManeuverDescription = null;
    private String lastManeuverRoad = null;
    private String lastExitInfo = null;
    private String lastJunctionAnglesCsv = null;

    public void startPpsListener() {
        if (ppsReaderThread != null && ppsReaderThread.isAlive()) return;
        ppsReaderStop = false;
        ppsReaderThread = new Thread(new Runnable() {
            public void run() { ppsReaderLoop(); }
        }, "CarPlayRgPpsReader");
        ppsReaderThread.setDaemon(true);
        ppsReaderThread.start();
        logCluster("PPS: reader started (" + PPS_RG_PATH + ")");
    }

    public void stopPpsListener() {
        ppsReaderStop = true;
        if (ppsReaderThread != null) {
            ppsReaderThread.interrupt();
            ppsReaderThread = null;
        }
    }

    private boolean ppsFirstReadLogged = false;
    private long    ppsLastErrorLogTs = 0;

    private int  ppsReadCount = 0;
    private int  ppsLastReadSize = -1;

    private void ppsReaderLoop() {
        while (!ppsReaderStop) {
            java.io.File f = new java.io.File(PPS_RG_PATH);
            if (!f.exists()) {
                try { Thread.sleep(1000); } catch (InterruptedException ie) { return; }
                continue;
            }
            int n = -1;
            String body = null;
            try {
                java.io.FileInputStream fis = new java.io.FileInputStream(PPS_RG_PATH);
                /* Read full file — poll-style loop handles short reads. */
                java.io.ByteArrayOutputStream bout = new java.io.ByteArrayOutputStream();
                byte[] chunk = new byte[4096];
                int got;
                while ((got = fis.read(chunk)) > 0) bout.write(chunk, 0, got);
                fis.close();
                byte[] full = bout.toByteArray();
                n = full.length;
                if (n > 0) body = new String(full, "UTF-8");
            } catch (Throwable t) {
                long now = System.currentTimeMillis();
                if (now - ppsLastErrorLogTs > 5000) {
                    ppsLastErrorLogTs = now;
                    logCluster("PPS: read error: " + t.toString());
                }
            }

            if (body != null) {
                ppsReadCount++;
                /* Log every read's size when size changes, and the first 3 reads
                 * unconditionally, so we can tell the loop is running. */
                if (ppsReadCount <= 3 || n != ppsLastReadSize) {
                    logCluster("PPS: read #" + ppsReadCount + " size=" + n);
                }
                ppsLastReadSize = n;
                try {
                    parseAndApply(body);
                } catch (Throwable t) {
                    /* Catch Error too (OutOfMemoryError, ExceptionInInitializerError,
                     * etc.) — otherwise the thread silently dies. */
                    long now = System.currentTimeMillis();
                    if (now - ppsLastErrorLogTs > 5000) {
                        ppsLastErrorLogTs = now;
                        logCluster("PPS: parse error (#" + ppsReadCount + "): " + t.toString());
                    }
                }
            }
            try { Thread.sleep(250); } catch (InterruptedException ie) { return; }
        }
    }

    private int parseCallCount = 0;

    /* PPS format: "@<obj>\n" header then "key:type:value\n" lines.
     * type 'n' = number, 's' = string (type may be empty), 'b' = boolean.
     * junction_angles is written by the hook as a comma-separated string. */
    private void parseAndApply(String body) {
        parseCallCount++;
        if (parseCallCount <= 3) {
            int nl = 0;
            for (int i = 0; i < body.length(); i++) if (body.charAt(i) == '\n') nl++;
            logCluster("PPS_PARSE: call #" + parseCallCount + " body.len=" + body.length() + " lines=" + nl);
        }
        int routeState = lastRouteState;
        int maneuverType = lastManeuverType;
        int maneuverDistance = lastManeuverDistance;
        int distanceRemaining = lastDistanceRemaining;
        int timeRemaining = lastTimeRemaining;
        int drivingSide = lastDrivingSide;
        int junctionType = lastJunctionType;
        int exitAngle = lastExitAngle;
        String destination = lastDestination;
        String maneuverDescription = lastManeuverDescription;
        String maneuverRoad = lastManeuverRoad;
        String exitInfo = lastExitInfo;
        String junctionAnglesCsv = lastJunctionAnglesCsv;
        boolean sessionActive = false;
        int sourceSupportsRg = lastSourceSupportsRg;
        int distToManeuverM = -1;       /* 0x5201 TLV 0x000A — distance to NEXT turn, in METERS (live) */
        int laneGuidanceShowing = -1;   /* 0x5201 TLV 0x0012 — 0=hide, 1=show */
        boolean laneActive = false;
        int laneCount = 0;
        int[] lanePos = null;
        int[] laneDir = null;
        int[] laneStatus = null;
        String[] laneAnglesCsv = null;

        /* Stage 2 — slot array. Parse slot_N_* keys into per-slot arrays
         * then pick the slot indexed by "current_slot" as the "next turn"
         * that feeds updateManeuver(). */
        int slotCount = 0;
        int currentSlot = -1;
        final int MAX_SLOTS = 6;   /* BAP spec limit */
        int[]    slotIapIdx   = new int[MAX_SLOTS];
        int[]    slotType     = new int[MAX_SLOTS];
        int[]    slotDrvSide  = new int[MAX_SLOTS];
        int[]    slotJType    = new int[MAX_SLOTS];
        int[]    slotExitAng  = new int[MAX_SLOTS];
        int[]    slotDistance = new int[MAX_SLOTS];
        String[] slotDesc     = new String[MAX_SLOTS];
        String[] slotRoad     = new String[MAX_SLOTS];
        String[] slotExitInfo = new String[MAX_SLOTS];
        String[] slotJAngles  = new String[MAX_SLOTS];
        for (int si = 0; si < MAX_SLOTS; si++) slotIapIdx[si] = -1;

        /* Manual line splitter — QNX J9 JVM is pre-1.4, no String.split(). */
        java.util.StringTokenizer lineTok = new java.util.StringTokenizer(body, "\n");
        while (lineTok.hasMoreTokens()) {
            String line = lineTok.nextToken().trim();
            if (line.length() == 0 || line.charAt(0) == '@') continue;
            int c1 = line.indexOf(':');
            if (c1 < 0) continue;
            int c2 = line.indexOf(':', c1 + 1);
            if (c2 < 0) continue;
            String key = line.substring(0, c1);
            String val = line.substring(c2 + 1);
            try {
                if (key.equals("session_active")) {
                    sessionActive = val.equals("true");
                } else if (key.equals("route_state")) {
                    routeState = Integer.parseInt(val);
                } else if (key.equals("destination")) {
                    destination = val;
                } else if (key.equals("distance_remaining")) {
                    distanceRemaining = (int) Long.parseLong(val);
                } else if (key.equals("time_remaining")) {
                    timeRemaining = (int) Long.parseLong(val);
                } else if (key.equals("maneuver_type")) {
                    maneuverType = Integer.parseInt(val);
                } else if (key.equals("maneuver_distance")) {
                    maneuverDistance = (int) Long.parseLong(val);
                } else if (key.equals("maneuver_description")) {
                    maneuverDescription = val;
                } else if (key.equals("maneuver_road")) {
                    maneuverRoad = val;
                } else if (key.equals("exit_info")) {
                    exitInfo = val;
                } else if (key.equals("source_supports_rg")) {
                    sourceSupportsRg = Integer.parseInt(val);
                } else if (key.equals("dist_to_maneuver")) {
                    distToManeuverM = (int) Long.parseLong(val);
                } else if (key.equals("lane_guidance_showing")) {
                    laneGuidanceShowing = Integer.parseInt(val);
                } else if (key.equals("slot_count")) {
                    slotCount = Integer.parseInt(val);
                    if (slotCount > MAX_SLOTS) slotCount = MAX_SLOTS;
                } else if (key.equals("current_slot")) {
                    currentSlot = Integer.parseInt(val);
                } else if (key.startsWith("slot_") && key.length() > 5) {
                    /* slot_N_<suffix> */
                    int us = key.indexOf('_', 5);
                    if (us > 5) {
                        try {
                            int idx = Integer.parseInt(key.substring(5, us));
                            String suffix = key.substring(us + 1);
                            if (idx >= 0 && idx < MAX_SLOTS) {
                                if ("iap_idx".equals(suffix))      slotIapIdx[idx]   = Integer.parseInt(val);
                                else if ("type".equals(suffix))         slotType[idx]     = Integer.parseInt(val);
                                else if ("driving_side".equals(suffix))  slotDrvSide[idx]  = Integer.parseInt(val);
                                else if ("junction_type".equals(suffix)) slotJType[idx]    = Integer.parseInt(val);
                                else if ("exit_angle".equals(suffix))    slotExitAng[idx]  = Integer.parseInt(val);
                                else if ("distance".equals(suffix))      slotDistance[idx] = (int) Long.parseLong(val);
                                else if ("description".equals(suffix))   slotDesc[idx]     = val;
                                else if ("road".equals(suffix))          slotRoad[idx]     = val;
                                else if ("exit_info".equals(suffix))     slotExitInfo[idx] = val;
                                else if ("junction_angles".equals(suffix)) slotJAngles[idx] = val;
                            }
                        } catch (NumberFormatException nfe) { /* skip */ }
                    }
                } else if (key.equals("lane_active")) {
                    laneActive = val.equals("true");
                } else if (key.equals("lane_count")) {
                    laneCount = Integer.parseInt(val);
                    if (laneCount > 8) laneCount = 8;
                    lanePos = new int[laneCount];
                    laneDir = new int[laneCount];
                    laneStatus = new int[laneCount];
                    laneAnglesCsv = new String[laneCount];
                } else if (key.startsWith("lane_") && key.length() > 5) {
                    /* lane_N_pos / lane_N_dir / lane_N_status / lane_N_angles */
                    int us = key.indexOf('_', 5);
                    if (us > 5 && lanePos != null) {
                        try {
                            int idx = Integer.parseInt(key.substring(5, us));
                            String suffix = key.substring(us + 1);
                            if (idx >= 0 && idx < lanePos.length) {
                                if ("angles".equals(suffix)) {
                                    laneAnglesCsv[idx] = val;
                                } else {
                                    int v = Integer.parseInt(val);
                                    if ("pos".equals(suffix))         lanePos[idx] = v;
                                    else if ("dir".equals(suffix))    laneDir[idx] = v;
                                    else if ("status".equals(suffix)) laneStatus[idx] = v;
                                }
                            }
                        } catch (NumberFormatException nfe) { /* skip */ }
                    }
                } else if (key.equals("driving_side")) {
                    drivingSide = Integer.parseInt(val);
                } else if (key.equals("junction_type")) {
                    junctionType = Integer.parseInt(val);
                } else if (key.equals("exit_angle")) {
                    exitAngle = Integer.parseInt(val);
                } else if (key.equals("junction_angles")) {
                    junctionAnglesCsv = val;
                }
                /* maneuver_road / exit_info / eta / distance_units / source_name
                 * available but not yet consumed by this Stage-1 bridge. */
            } catch (NumberFormatException nfe) { /* skip malformed */ }
        }

        /* Compute effective activeness: need both a live route state AND the
         * nav app to declare RG support. Waze always says 0 → cluster stays
         * cleared. Google Maps briefly says 0 during LOADING then 1 → we
         * ignore the dip because it's on route_state != 0. */
        boolean routeLive = sessionActive
                && routeState >= 1 && routeState != 2;   /* 1,3,4,5,6 = live */
        boolean shouldShowRg = routeLive && sourceSupportsRg == 1;

        /* Log every state transition + the first successful parse (when
         * lastRouteState is still -1) so we can see the bridge working. */
        if (shouldShowRg != rgActive
                || routeState != lastRouteState
                || sourceSupportsRg != lastSourceSupportsRg) {
            logCluster("PPS_APPLY: sessionActive=" + sessionActive
                     + " routeState=" + routeState
                     + " srcSupRg=" + sourceSupportsRg
                     + " shouldShowRg=" + shouldShowRg
                     + " rgActive=" + rgActive
                     + " clusterSvc=" + (clusterService != null));
        }

        if (shouldShowRg != rgActive) {
            if (shouldShowRg) startRouteGuidance();
            else              stopRouteGuidance();
        }
        lastRouteState = routeState;
        lastSourceSupportsRg = sourceSupportsRg;

        if (!rgActive) return;

        /* Stage 2 — drive updateManeuver/updateDistance from the CURRENT slot
         * (the slot referenced by currentSlot, which corresponds to
         * maneuver_list[0] on the hook side). As the driver passes each turn,
         * iPhone advances the list and our currentSlot shifts → we pick up
         * the NEXT maneuver automatically. */
        if (slotCount > 0 && currentSlot >= 0 && currentSlot < slotCount) {
            int cs = currentSlot;
            int curIapIdx = slotIapIdx[cs];
            /* TRACE every ML advance so the log tells the full story even
             * when two consecutive turns share a BAP type (e.g. RIGHT →
             * RIGHT) and updateManeuver's change-detection wouldn't fire. */
            if (curIapIdx != lastCurrentSlotIapIdx) {
                logCluster("TRACE: ML advance"
                        + " iap_idx=" + curIapIdx
                        + " type=" + slotType[cs]
                        + " desc=\"" + (slotDesc[cs] != null ? slotDesc[cs] : "") + "\""
                        + " road=\"" + (slotRoad[cs] != null ? slotRoad[cs] : "") + "\""
                        + " DtM=" + distToManeuverM
                        + "m RS=" + routeState
                        + " slots=" + slotCount);
                lastCurrentSlotIapIdx = curIapIdx;
                /* Two consecutive maneuvers can share {mainElement, direction,
                 * type} — e.g. RIGHT → RIGHT, or two ROUNDABOUT_EXIT_1s in a
                 * row — and the inner gate inside updateManeuver then refuses
                 * to reset state. The bargraph baseline carries the previous
                 * maneuver's value (tiny) over a much longer next-maneuver
                 * approach, so the bar stays at 0% for almost the whole leg.
                 * Same idea for the roundabout direction latch: keep it for
                 * anti-flicker WITHIN one roundabout (same iap_idx), reset it
                 * the moment we cross to a different roundabout instance.
                 * Confirmed in the 2026-04-26 log on Highfields Rd, A1303 and
                 * Coldhams Lane — three back-to-back same-type maneuvers
                 * where bargraph never fired. */
                maneuverInitialDistance = 0;
                inRoundabout = false;
                cachedRoundaboutDirection = -1;
                lastRoundaboutManeuverType = -1;
            }
            int    rawType     = slotType[cs];
            /* Hold off the ARRIVE_* flag-icon until we're actually close.
             * iPhone marks type=24/25 (ARRIVE on left/right) as the final
             * maneuver minutes before physical arrival. Show the flag only
             * when EITHER distance-to-maneuver is genuinely near (< 150 m)
             * OR phone has declared RouteState=2 (ARRIVED). Otherwise show
             * STRAIGHT_AHEAD. Covers both log-observed forms of the final
             * signal: small distance or explicit RouteState transition. */
            /* iAP2 TLV 0x000A is already in meters despite TLV 0x000C naming a
             * display unit — the units field only tells iPhone's own string
             * formatter how to render the value in its HUD, not the raw int. */
            int distMForHold = distToManeuverM >= 0
                    ? distToManeuverM
                    : slotDistance[cs];
            boolean isArrival = (rawType == CarPlayManeuverMapper.MT_ARRIVE_AT_DESTINATION
                    || rawType == CarPlayManeuverMapper.MT_ARRIVE_END_OF_NAVIGATION
                    || rawType == CarPlayManeuverMapper.MT_ARRIVE_END_OF_DIRECTIONS
                    || rawType == CarPlayManeuverMapper.MT_ARRIVE_DESTINATION_LEFT
                    || rawType == CarPlayManeuverMapper.MT_ARRIVE_DESTINATION_RIGHT);
            final int ARRIVAL_ICON_THRESHOLD_M = 70;
            boolean showArrival = isArrival
                    && (distMForHold <= ARRIVAL_ICON_THRESHOLD_M || routeState == 2);
            int curType = (isArrival && !showArrival)
                    ? CarPlayManeuverMapper.MT_STRAIGHT_AHEAD
                    : rawType;
            int    curDrvSide  = slotDrvSide[cs];
            int    curJType    = slotJType[cs];
            int    curExitAng  = slotExitAng[cs];
            int    curDist     = slotDistance[cs];
            String curDesc     = slotDesc[cs]     != null ? slotDesc[cs]     : "";
            String afterRoad   = slotRoad[cs]     != null ? slotRoad[cs]     : "";
            /* Apple Maps frequently puts the destination road designation
             * (A428, A1198, M11, …) in the description while leaving
             * after_road set to the CURRENT/local road name (e.g. "Ermine
             * Street South" for a road that's actually the A1198). The
             * cluster has limited screen real-estate, so the driver expects
             * the route number — that's what iPhone itself shows. Prefer a
             * designation extracted from desc when present; fall back to
             * after_road. Google Maps already puts the designation in
             * after_road, so its values pass through unchanged. */
            String designation = extractRoadDesignation(curDesc);
            String curRoad = (designation != null) ? designation : afterRoad;
            String curExitInfo = slotExitInfo[cs] != null ? slotExitInfo[cs] : "";
            String curJAngles  = slotJAngles[cs]  != null ? slotJAngles[cs]  : "";

            boolean maneuverChanged =
                    curType != lastManeuverType
                    || curDrvSide != lastDrivingSide
                    || curJType != lastJunctionType
                    || curExitAng != lastExitAngle
                    || !curRoad.equals(lastManeuverRoad != null ? lastManeuverRoad : "")
                    || !curDesc.equals(lastManeuverDescription != null ? lastManeuverDescription : "")
                    || !curExitInfo.equals(lastExitInfo != null ? lastExitInfo : "")
                    || !curJAngles.equals(lastJunctionAnglesCsv != null ? lastJunctionAnglesCsv : "");

            /* Apple sends type=7 EXIT_ROUNDABOUT briefly between two
             * roundabouts on a connecting road. The mapper would land it on
             * NO_INFO STRAIGHT (blank icon), and an explicit mapping to
             * EXIT_ROUNDABOUT_TRS_LEFT renders on this cluster as a plain
             * left-turn arrow. Both look broken. AA's approach is to keep
             * the previous roundabout's icon latched until the next real
             * maneuver — so we skip pushing a descriptor while type=7 is
             * current. lastCurrentSlotIapIdx still advances, distance still
             * updates; the BAP descriptor on the cluster simply doesn't
             * change. */
            if (maneuverChanged && curType != CarPlayManeuverMapper.MT_EXIT_ROUNDABOUT) {
                int[] curJAnglesArr = parseAnglesCsv(curJAngles);
                updateManeuver(curType, curExitAng, curJType, curDrvSide,
                        curRoad, curJAnglesArr, curExitInfo);
                lastManeuverType = curType;
                lastDrivingSide = curDrvSide;
                lastJunctionType = curJType;
                lastExitAngle = curExitAng;
                lastManeuverDescription = curDesc;
                lastManeuverRoad = curRoad;
                lastExitInfo = curExitInfo;
                lastJunctionAnglesCsv = curJAngles;
            }

            /* Distance to NEXT turn — live-updating. 0x5201 TLV 0x000A is
             * authoritative and reports meters directly; 0x5202 "distance_between"
             * is static per segment. */
            int dtmMeters = distToManeuverM >= 0
                    ? distToManeuverM
                    : curDist;
            if (dtmMeters != lastManeuverDistance) {
                updateDistance(dtmMeters);
                lastManeuverDistance = dtmMeters;
            }
        }
        if (distanceRemaining != lastDistanceRemaining
                || timeRemaining != lastTimeRemaining
                || (destination != null && !destination.equals(lastDestination))) {
            updateDestination(distanceRemaining, timeRemaining,
                    destination != null ? destination : "");
            lastDistanceRemaining = distanceRemaining;
            lastTimeRemaining = timeRemaining;
            lastDestination = destination;
        }

        /* Lane guidance delta + gating.
         * iPhone uses TLV 0x0012 (LaneGuidanceShowing) in 0x5201 to tell us
         * whether lanes should be visible. When 0, clear the cluster lane bar
         * even if we still have cached lane data from an earlier 0x5204.
         * When -1 (field not present) we fall back to having-any-lanes. */
        boolean showLanes = laneActive
                && (laneGuidanceShowing == 1
                    || (laneGuidanceShowing == -1 && lanePos != null && lanePos.length > 0));

        String laneSig;
        if (!showLanes) {
            laneSig = "off";
        } else {
            StringBuffer sb = new StringBuffer();
            for (int i = 0; i < lanePos.length; i++) {
                if (i > 0) sb.append('|');
                sb.append(lanePos[i]).append(':').append(laneDir[i]).append(':').append(laneStatus[i]);
                /* include angles in signature so a re-deduplication change
                 * (e.g. lane gains a secondary direction) re-publishes. */
                if (laneAnglesCsv != null && i < laneAnglesCsv.length && laneAnglesCsv[i] != null) {
                    sb.append('@').append(laneAnglesCsv[i]);
                }
            }
            laneSig = sb.toString();
        }
        if (!laneSig.equals(lastLaneSignature)) {
            int[][] anglesPerLane = null;
            if (laneAnglesCsv != null) {
                anglesPerLane = new int[laneAnglesCsv.length][];
                for (int i = 0; i < laneAnglesCsv.length; i++) {
                    anglesPerLane[i] = parseAnglesCsv(laneAnglesCsv[i]);
                }
            }
            updateLanes(showLanes, lanePos, laneDir, laneStatus, anglesPerLane);
            lastLaneSignature = laneSig;
        }
    }

    /* Parse "a,b,c" → int[]{a,b,c}. Returns empty array on null/blank.
     * QNX J9 JVM is pre-1.4 so no String.split(); use StringTokenizer. */
    private int[] parseAnglesCsv(String csv) {
        if (csv == null || csv.length() == 0) return new int[0];
        java.util.StringTokenizer tok = new java.util.StringTokenizer(csv, ",");
        int[] tmp = new int[16];   /* more than we'll ever see in practice */
        int n = 0;
        while (tok.hasMoreTokens() && n < tmp.length) {
            try {
                tmp[n++] = Integer.parseInt(tok.nextToken().trim());
            } catch (NumberFormatException nfe) { /* skip malformed */ }
        }
        int[] out = new int[n];
        System.arraycopy(tmp, 0, out, 0, n);
        return out;
    }

    /* ============================================================
     * DSICarplayListener — remaining callbacks (no-ops for now)
     * ============================================================ */

    public void responseModeChange(Resource[] resources, AppState[] appStates, int validFlag) {
        logCluster("DSI_IN: RESPONSE_MODE_CHANGE valid=" + validFlag);
    }
    public void requestBTDeactivation(String deviceName, int validFlag) {
        logCluster("DSI_IN: REQUEST_BT_DEACTIVATION device=" + deviceName);
    }
    public void updateDeviceInfo(DeviceInfo info, int validFlag) {
        logCluster("DSI_IN: DEVICE_INFO valid=" + validFlag);
    }
    public void updateCallState(CallState[] states, int validFlag) {
        logCluster("DSI_IN: CALL_STATE count=" + (states != null ? states.length : 0));
    }
    public void updateTelephonyState(TelephonyState state, int validFlag) {
        logCluster("DSI_IN: TELEPHONY_STATE valid=" + validFlag);
    }
    public void updateNowPlayingData(TrackData data, int validFlag) {
        logCluster("DSI_IN: NOW_PLAYING valid=" + validFlag);
    }
    public void updatePlaybackState(PlaybackInfo info, int validFlag) {
        logCluster("DSI_IN: PLAYBACK_STATE valid=" + validFlag);
    }
    public void updatePlayposition(int position, int validFlag) {}
    public void updateCoverArtUrl(ResourceLocator url, int validFlag) {}
    public void updateTextInputState(int state, int validFlag) {
        logCluster("DSI_IN: TEXT_INPUT state=" + state);
    }
    public void duckAudio(int duration, double volume) {
        logCluster("DSI_IN: DUCK_AUDIO dur=" + duration + " vol=" + volume);
    }
    public void unduckAudio(int duration) {
        logCluster("DSI_IN: UNDUCK_AUDIO dur=" + duration);
    }
    public void oemAppSelected() {
        logCluster("DSI_IN: OEM_APP_SELECTED");
    }
    public void updateMainAudioType(int type, int sampleRate, int validFlag) {
        logCluster("DSI_IN: MAIN_AUDIO_TYPE type=" + type + " sampleRate=" + sampleRate);
    }
    public void updateStreamParameters(int w, int h, int m, int d, int f, int c, int v) {
        logCluster("DSI_IN: STREAM_PARAMS w=" + w + " h=" + h + " m=" + m + " d=" + d + " f=" + f + " c=" + c + " v=" + v);
    }
    public void launchAppResult(String appId, boolean success) {
        logCluster("DSI_IN: LAUNCH_APP_RESULT app=" + appId + " success=" + success);
    }
    public void asyncException(int errCode, String msg, int requestType) {}
}
