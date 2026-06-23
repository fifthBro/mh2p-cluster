/*
 * Copyright (c) 2026 fifthBro
 * https://fifthbro.github.io
 *
 * Licensed under CC BY-NC-SA 4.0
 * https://creativecommons.org/licenses/by-nc-sa/4.0/
 * NOT FOR COMMERCIAL USE
 */

package de.audi.app.terminalmode.smartphone.androidauto2.nav;

import de.audi.app.terminalmode.dsi.androidauto2.DSIAndroidAuto2ListenerSafe;
import de.audi.app.terminalmode.dsi.androidauto2.NavigationFocusType;
import de.audi.atip.interapp.combi.bap.navi.CombiBAPServiceNavi;
import de.audi.atip.interapp.combi.bap.navi.CombiBAPConstantsNavi;
import de.audi.atip.interapp.combi.bap.navi.data.DistanceUnit;
import de.audi.atip.interapp.combi.bap.navi.data.CombiBAPNaviManeuverDescriptor;
import de.audi.atip.interapp.combi.bap.navi.data.CurrentPositionInfo;
import de.audi.atip.interapp.combi.bap.PartialPopupBAPService;
import de.audi.atip.interapp.combi.bap.PartialPopupBAPServiceListener;
import de.audi.atip.interapp.combi.bap.data.PartialPopupBAPContent;
import de.audi.app.car.api.services.ICarStatisticsService;
import de.audi.app.car.api.sys.resources.statistics.IStatisticsResource;
import de.audi.app.car.api.typedef.statistics.IStatisticsTypeCollection;
import de.audi.app.car.api.core.ICarCoreServices;
import de.audi.app.car.api.services.ICarExteriorLightService;
import org.dsi.ifc.map.DSIMapViewerControl;
import de.audi.mib.system.ISysServices;
import de.audi.mib.system.clock.IClock;
import de.audi.mib.system.units.IUnitManager;
import de.audi.mib.system.units.IUnitProvider;
import de.audi.mib.system.units.UnitSetting;
import de.audi.atip.utils.reactive.properties.ReadOnlyProperty;
import org.dsi.ifc.androidauto2.PlaybackInfo;
import org.dsi.ifc.androidauto2.TrackData;
import org.dsi.ifc.global.ResourceLocator;
import java.io.File;
import java.io.FileWriter;
import java.io.PrintWriter;
import java.io.FileInputStream;
import java.io.InputStreamReader;
import java.io.BufferedReader;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Timer;
import java.util.TimerTask;

// Storage mount handler for USB/SD remounting
import de.audi.app.car.adi.legacy.sportchrono.StorageMountHandler;
import de.audi.atip.log.LogChannel;
import org.osgi.framework.BundleContext;
import java.util.ArrayList;
import java.util.List;

public class AndroidAutoClusterIntegration implements DSIAndroidAuto2ListenerSafe, PartialPopupBAPServiceListener {
    private static final String LOGCLASS = "AndroidAutoClusterIntegration";
    private static final String DEFAULT_LOG_PATH           = "/tmp";
    private static final String LOG_FILE_NAME              = "cluster.log";
    private static final String HASHED_LOG_FILE_NAME       = "cluster_hashed.log";
    private static final String CLUSTER_LOG_FILE           = DEFAULT_LOG_PATH + "/" + LOG_FILE_NAME;
    private static final String CLUSTER_HASHED_LOG_FILE    = DEFAULT_LOG_PATH + "/" + HASHED_LOG_FILE_NAME;
    private static final String CLUSTER_CONFIG_FILE        = "/cluster_config.json";
    /* Installed config path written by install.sh from the deploy package.
     * The JAR no longer embeds this file; we read it from disk instead. */
    private static final String CLUSTER_CONFIG_INSTALLED   = "/mnt/app/eso/bin/apps/cluster/cluster_config.json";
    private static final long   MAX_LOG_SIZE               = 1024 * 1024; // 1024KB

    // External storage mount points (config and logs are derived from these)
    private static final String[] EXTERNAL_MOUNT_POINTS = {
        "/fs/usb0_0",
        "/fs/usb1_0",
        "/fs/sda0",
        "/fs/sdb0"
    };

    // BAP mainElement constants (per BAP spec section)
    private static final int MAIN_ELEMENT_FOLLOW_STREET = 0x0B;     // 11 = FollowStreet
    private static final int MAIN_ELEMENT_TURN = 0x0D;              // 13 = Turn
    private static final int MAIN_ELEMENT_EXIT_RIGHT = 0x0F;        // 15 = ExitRight (off-ramp right)
    private static final int MAIN_ELEMENT_EXIT_LEFT = 0x10;         // 16 = ExitLeft (off-ramp left)
    private static final int MAIN_ELEMENT_ROUNDABOUT_TRS_RIGHT = 0x15;  // 21 = RoundaboutTrsRight
    private static final int MAIN_ELEMENT_ROUNDABOUT_TRS_LEFT = 0x16;   // 22 = RoundaboutTrsLeft
    private static final int MAIN_ELEMENT_UTURN = 0x19;             // 25 = Uturn
    private static final int MAIN_ELEMENT_EXIT_ROUNDABOUT_TRS_RIGHT = 0x1A;  // 26 = ExitRoundaboutTrsRight
    private static final int MAIN_ELEMENT_EXIT_ROUNDABOUT_TRS_LEFT = 0x1B;   // 27 = ExitRoundaboutTrsLeft
    private static final int MAIN_ELEMENT_PREPARE_ROUNDABOUT = 0x1D;         // 29 = PrepareRoundabout (generic, no direction)
    private static final int MAIN_ELEMENT_TURN_ON_MAINROAD = 0x0E;  // 14 = TurnOnMainroad
    private static final int MAIN_ELEMENT_FORK_2 = 0x13;            // 19 = Fork-2
    private static final int MAIN_ELEMENT_FORK_3 = 0x14;            // 20 = Fork-3 (reserved for future)
    private static final int MAIN_ELEMENT_MAINROAD_RIGHT = 0x29;    // 41 = MainRoadRight (on-ramp right)
    private static final int MAIN_ELEMENT_MAINROAD_LEFT = 0x2A;     // 42 = MainRoadLeft (on-ramp left)
    private static final int MAIN_ELEMENT_FERRY = 0x2C;             // 44 = Take Ferry
    private static final int MAIN_ELEMENT_DESTINATION = 0x03;       // 3 = Arrived (not used to avoid premature arrival)

    // BAP direction constants
    private static final int DIRECTION_STRAIGHT = 0;
    private static final int DIRECTION_LEFT = 1;
    private static final int DIRECTION_RIGHT = 2;
    private static final int DIRECTION_SLIGHT_LEFT = 3;
    private static final int DIRECTION_SLIGHT_RIGHT = 4;
    private static final int DIRECTION_SHARP_LEFT = 5;
    private static final int DIRECTION_SHARP_RIGHT = 6;
    private static final int DIRECTION_UTURN_LEFT = 7;
    private static final int DIRECTION_UTURN_RIGHT = 8;

    private CombiBAPServiceNavi clusterService;
	private PartialPopupBAPService popupService;
    private ICarStatisticsService statisticsService;
    private ISysServices sysServices;
    private ICarCoreServices carCoreServices;
    private ICarExteriorLightService exteriorLightService;
    private DSIMapViewerControl kombiMapViewerControl;  // KOMBI instance (3) - native nav cluster map control
    private Object mapClusterService;  // de.audi.tghu.navi.app.cluster.IMapClusterService - via reflection
    private StorageMountHandler storageMountHandler;

    // Car time tracking (dual timestamps: QNX time + actual car time)
    private boolean carTimeAvailable = false;
    private boolean carTimeChecked = false;
    private String carTimeDebugInfo = null;

    // External config file support
    private boolean externalConfigChecked = false;
    private String externalConfigContent = null;
    
	private String buildVersion = "__VERSION__";
	
	private boolean navigationActive = false;
    private boolean forceImperial = false;  // For testing on metric testbench
    private boolean forceRHD = false;  // For forcing right-hand drive override
    private String logFilePath = CLUSTER_LOG_FILE;  // Configurable log file path
    private String hashedLogFilePath = CLUSTER_HASHED_LOG_FILE;  // Configurable hashed log file path
    private long logFileSize = 50;  // Log file size in MB (default: 50MB)

    // Config options loaded from JSON
    private boolean fileLoggingEnabled = true;  // default: enabled
    private boolean fileHashingEnabled = false;  // default: disabled
    private boolean enableExternalLogging = false;  // default: disabled (logs to configured path)
    private boolean roundaboutLatchingEnabled = true;  // default: enabled
    private boolean heartbeatEnabled = true;  // default: enabled
    private int maneuverStateMask = 0;  // bitmask: bit0=state1, bit1=state2, bit2=state3, bit3=state4. 0=disabled
    private int heartbeatInterval = 2000;  // default: 2000ms (2 seconds)

    // Map rendering / AA mirror config
    private boolean enableMapRender      = false;
    private String  mirrorFifo           = "/tmp/cluster_ctl";
    private String  mirrorMode           = "fill";
    private float   mirrorZoomX          = 1.0f;
    private float   mirrorZoomY          = 1.0f;
    private float   mirrorPanX           = 0.0f;
    private float   mirrorPanY           = 0.0f;
    private String  mirrorCarConfigJson  = null;  // raw JSON block for per-car overrides
    private boolean videoCurrentlyAvailable = false;

    // AA cluster pipeline mode: "h264" (default — dedicated cluster stream from
    // phone, decoded by cluster daemon) or "mirror" (HMI screen mirror fallback).
    // Loaded from config JSON; overridable per-car in mirrorCarConfig.
    private String  aaClusterMode        = "h264";

    // Cluster pipeline lifecycle state. Java owns this — daemon is a dumb
    // dispatcher that just forwards commands.
    //   IDLE      — no active session, nothing running on cluster.
    //   PREPARED  — h264 only: prepare command sent, decoder warming up,
    //               not yet rendering to displayable 33.
    //   RENDERING — child is actively rendering on displayable 33.
    //   PAUSED    — h264 ONLY: SIGUSR2 sent, decoder kept alive but not
    //               rendering. Mirror has no warm state and never enters
    //               PAUSED; mirror toggles between IDLE and RENDERING via
    //               start/stop on the wire.
    private static final int CL_IDLE      = 0;
    private static final int CL_PREPARED  = 1;
    private static final int CL_RENDERING = 2;
    private static final int CL_PAUSED    = 3;
    private int clusterState = CL_IDLE;

    // Zone constants
    private static final int ZONE_VERY_FAR  = 0;
    private static final int ZONE_FAR       = 1;
    private static final int ZONE_APPROACHING = 2;
    private static final int ZONE_NEAR      = 3;
    private static final int ZONE_CLOSE     = 4;
    private static final int ZONE_VERY_CLOSE = 5;
    private static final int ZONE_NOW       = 6;

    // Zone boundaries (meters) - single source of truth
    private int veryFarBoundary    = 5000;
    private int farBoundary        = 1000;
    private int approachingBoundary = 500;
    private int nearBoundary       = 200;
    private int closeBoundary      = 100;
    private int veryCloseBoundary  = 50;

    // Dynamic threshold configuration (distance in meters, rate limit in milliseconds)
    private int veryFarDistanceThreshold = 100;
    private int veryFarRateLimit = 2000;
    private int farDistanceThreshold = 50;
    private int farRateLimit = 500;
    private int approachingDistanceThreshold = 25;
    private int approachingRateLimit = 250;
    private int nearDistanceThreshold = 15;
    private int nearRateLimit = 200;
    private int closeDistanceThreshold = 15;
    private int closeRateLimit = 150;
    private int veryCloseDistanceThreshold = 10;
    private int veryCloseRateLimit = 120;
    private int nowDistanceThreshold = 5;
    private int nowRateLimit = 100;

    // Bargraph display mode for platforms that show either distance OR bargraph
    private String bargraphMode = "auto";  // "auto", "always", "distance", "dynamic"
    private int dynamicBargraphDistance = 100;  // meters (for "dynamic" mode switch threshold)
    private int dynamicBargraphPercent = 50;   // percentage for short maneuvers in "dynamic" mode

    // Unit switching thresholds (when to switch from km/mi to m/ft or m/yd)
    private int metricUnitThreshold = 1000;    // meters - switch to meters below this (default 1km)
    private int imperialUnitThreshold = 161;   // meters - switch to small unit below this (default 0.1mi)
    private String imperialSmallUnit = "auto"; // "auto", "feet" or "yards"
    private String resolvedImperialSmallUnit = null; // cached resolution of "auto" → "feet"/"yards"

    private boolean configLoaded = false;
    private String loadedConfigJson = null; // cached config JSON for lazy country lookups

    // Cached unit detection result (can be refreshed by querying services)
    private boolean useMetricCached = true;
    private boolean unitsDetermined = false;

    private int lastEventCode = -1;
    private int lastTurnSide = -1;
    private int lastDistance = -1;
    private long lastUpdateTime = 0;

    // BAP Heartbeat mechanism to override native nav
    private Timer heartbeatTimer = null;
    private CombiBAPNaviManeuverDescriptor[] lastManeuverArray = null;
    private String lastRoadName = null;
    private String lastSignPost = null;
    private int lastDisplayValue = -1;
    private int lastUnit = -1;
    private int lastBargraph = -1;
    private boolean lastBargraphEnabled = true;

    // Dynamic bargraph
    private String lastManeuverKey = null;
    private int maneuverInitialDistance = 0;
    private boolean resetManeuverOnNextDistance = false;
    private boolean waitingForPostTurnTurnEvent = false;
    private boolean resetCameFromRoundabout = false;
    private static final boolean PRESERVE_BASELINE_ENABLED = false;  // TEST: disable to verify preserveBaseline is not needed

    // Cached values for destination distance/time from next-turn data
    private long lastTimeToDestination = 0;  // estimated time (seconds)
    private int lastDistanceToDestination = 0;  // distance to next turn (meters)
    private int cachedRoundaboutDirection = -1;
    private boolean destinationEventSeen = false;
    private boolean isRHD = true;  // Right-hand drive (default for UK/AU/JP)
    private boolean driveSideDetermined = false;

    // Path override tracking
    private String lastLogPath = null;
    private String lastHashedLogPath = null;
    private boolean suppressPathLogging = false;
    private String lastExternalLogPath = null;  // Track external path state to avoid spam

    // Path caching to avoid repeated discovery
    private String cachedLogPath = null;
    private String cachedLogPathKey = null;  // Store configuredPath to know when cache is valid
    private boolean startupPopupShown = false;
    private static final int STARTUP_POPUP_ID = 1001;

    // Destination arrival delay
    private long destinationShownTime = 0;
    private int destinationDisplayDuration = 2000;  // milliseconds (default 2 seconds)
    private java.util.Timer pendingClearTimer = null;


    // ManeuverState tracking
    private int lastManeuverStateBAP = -1;  // -1 = not yet sent

    public AndroidAutoClusterIntegration(StorageMountHandler storageMountHandler) {
        this.storageMountHandler = storageMountHandler;
        // Note: Can't log here yet - logger not set until setLogger() is called
        // The "SYS: StorageMountHandler AVAILABLE" message will appear after setLogger()
    }

    public void logCluster(String message) {
        // Check if file logging is enabled (from JSON config)
        if (!fileLoggingEnabled) {
            return;  // Skip file logging if disabled
        }

        PrintWriter writer = null;
        try {
            // Determine best log file path using intelligent selection
            String logFileName = new File(logFilePath).getName();
            String actualLogPath = findBestLogPath(logFileName, logFilePath);
            
            // Log path override when actual path differs from configured path (initial) or previous path (change)
            if (!suppressPathLogging) {
                if (lastLogPath == null && !actualLogPath.equals(logFilePath)) {
                    // Initial override from config
                    suppressPathLogging = true;
                    logCluster("LOG: Path override: " + logFilePath + " -> " + actualLogPath);
                    suppressPathLogging = false;
                } else if (lastLogPath != null && !actualLogPath.equals(lastLogPath)) {
                    // Path changed from previous
                    suppressPathLogging = true;
                    logCluster("LOG: Path changed: " + lastLogPath + " -> " + actualLogPath);
                    suppressPathLogging = false;
                }
            }
            lastLogPath = actualLogPath;

            // Check log size and truncate if exceeds configured size
            File logFile = new File(actualLogPath);
            long maxLogSizeBytes = logFileSize * 1024 * 1024; // Convert MB to bytes
            if (logFile.exists() && logFile.length() > maxLogSizeBytes) {
                logFile.delete();
            }

            String timestamp = getDualTimestamp();
            writer = new PrintWriter(new FileWriter(actualLogPath, true));
            writer.println(timestamp + " | GAL: " + message);
            writer.flush();

            // Also write to hashed log if enabled
            logClusterHashed(message);
        } catch (Exception e) {
            // Write failed - invalidate cache to force re-discovery on next write
            invalidateLogPathCache();
        } finally {
            if (writer != null) {
                try {
                    writer.close();
                } catch (Exception e) {
                    // Ignore close errors
                }
            }
        }
    }

    private void logClusterHashed(String message) {
        // Check if hashed file logging is enabled (from JSON config)
        if (!fileHashingEnabled) {
            return;  // Skip hashed file logging if disabled
        }

        PrintWriter writer = null;
        try {
            // Determine best hashed log file path using intelligent selection
            String hashedLogFileName = new File(hashedLogFilePath).getName();
            String actualHashedLogPath = findBestLogPath(hashedLogFileName, hashedLogFilePath);
            
            // Log path override when actual path differs from configured path (initial) or previous path (change)
            if (!suppressPathLogging) {
                if (lastHashedLogPath == null && !actualHashedLogPath.equals(hashedLogFilePath)) {
                    // Initial override from config
                    suppressPathLogging = true;
                    logCluster("LOG: Hashed path override: " + hashedLogFilePath + " -> " + actualHashedLogPath);
                    suppressPathLogging = false;
                } else if (lastHashedLogPath != null && !actualHashedLogPath.equals(lastHashedLogPath)) {
                    // Path changed from previous
                    suppressPathLogging = true;
                    logCluster("LOG: Hashed path changed: " + lastHashedLogPath + " -> " + actualHashedLogPath);
                    suppressPathLogging = false;
                }
            }
            lastHashedLogPath = actualHashedLogPath;

            // Check log size and truncate if exceeds configured size
            File logFile = new File(actualHashedLogPath);
            long maxLogSizeBytes = logFileSize * 1024 * 1024; // Convert MB to bytes
            if (logFile.exists() && logFile.length() > maxLogSizeBytes) {
                logFile.delete();
            }

            // Sanitize the message by removing sensitive information
            String sanitizedMessage = sanitizeMessage(message);

            String timestamp = getDualTimestamp();
            writer = new PrintWriter(new FileWriter(actualHashedLogPath, true));
            writer.println(timestamp + " | " + sanitizedMessage);
            writer.flush();
        } catch (Exception e) {
            // Write failed - invalidate cache to force re-discovery on next write
            invalidateLogPathCache();
        } finally {
            if (writer != null) {
                try {
                    writer.close();
                } catch (Exception e) {
                    // Ignore close errors
                }
            }
        }
    }

    private String sanitizeMessage(String message) {
        if (message == null) {
            return null;
        }
        
        String result = message;
        
        // Sanitize road names - replace content within road="..." with hash
        result = sanitizeQuotedField(result, "road=");
        
        // Sanitize music metadata
        result = sanitizeQuotedField(result, "title=");
        result = sanitizeQuotedField(result, "album=");
        result = sanitizeQuotedField(result, "artist=");
        result = sanitizeQuotedField(result, "composer=");
        
        // Sanitize SignPost content - replace content within SignPost = "..." with hash
        result = sanitizeSignPost(result);
        
        return result;
    }
    
    private String sanitizeQuotedField(String text, String fieldName) {
        int startPos = text.indexOf(fieldName + "\"");
        if (startPos < 0) {
            return text; // Field not found
        }
        
        int valueStart = startPos + fieldName.length() + 1; // Skip field name and opening quote
        int valueEnd = text.indexOf("\"", valueStart);
        
        if (valueEnd < 0) {
            return text; // No closing quote found
        }
        
        String originalValue = text.substring(valueStart, valueEnd);
        if (originalValue.length() == 0) {
            return text; // Empty value, keep as is
        }
        
        String hashedValue = "[HASH_" + Math.abs(originalValue.hashCode()) + "]";
        
        return text.substring(0, valueStart) + hashedValue + text.substring(valueEnd);
    }
    
    private String sanitizeSignPost(String text) {
        // Look for SignPost = "..." pattern
        int signPostPos = text.indexOf("SignPost = \"");
        if (signPostPos < 0) {
            return text;
        }
        
        int valueStart = signPostPos + 12; // Length of "SignPost = \""
        int valueEnd = text.indexOf("\"", valueStart);
        
        if (valueEnd < 0) {
            return text;
        }
        
        String originalValue = text.substring(valueStart, valueEnd);
        if (originalValue.length() == 0) {
            return text;
        }
        
        String hashedValue = "[HASH_" + Math.abs(originalValue.hashCode()) + "]";
        
        return text.substring(0, valueStart) + hashedValue + text.substring(valueEnd);
    }

    public void setLogger() {
        // LogChannel removed, method kept for API compatibility
        // Load config from JSON (once)
        loadConfigFromJSON();

		String v = getClass().getPackage().getImplementationVersion();

		if (v == null || v.length() == 0) {
			v = buildVersion;   // value read from JSON
		}

		if (v == null || v.length() == 0) {
			v = "__VERSION__";
		}
		logCluster("SYS: Android Auto Cluster Integration Initialized (" + v + ")");

		// Log StorageMountHandler status
		if (storageMountHandler != null) {
			logCluster("SYS: StorageMountHandler AVAILABLE");
		} else {
			logCluster("SYS: StorageMountHandler NULL");
		}
    }

    private void writeSessionStartMarkers() {
        // Check if file logging is enabled (master override)
        if (!fileLoggingEnabled) {
            return;  // Skip all session marker writing if file logging disabled
        }
        
        // Generate unique session ID for correlation between logs
        String sessionId = generateSessionId();
        
        String separator = "========================================";
        String sessionStart = "=== NEW SESSION STARTED (ID: " + sessionId + ") ===";
        
        // Write to regular log file
        writeDirectToLogFile(separator, false);
        writeDirectToLogFile(sessionStart, false);
        writeDirectToLogFile(separator, false);
        
        // Write to hashed log file if enabled
        if (fileHashingEnabled) {
            writeDirectToLogFile(separator, true);
            writeDirectToLogFile(sessionStart, true);
            writeDirectToLogFile(separator, true);
        }
    }
    
    private String generateSessionId() {
        // Create a unique session ID using random number (since system time resets on boot)
        java.util.Random rand = new java.util.Random();
        int sessionNum = rand.nextInt(99999);  // 0-99998
        // Java 1.4 compatible zero-padding
        String id = String.valueOf(sessionNum);
        while (id.length() < 5) {
            id = "0" + id;
        }
        return id;
    }
    
    private void writeDirectToLogFile(String message, boolean hashed) {
        PrintWriter writer = null;
        try {
            String logFileName = hashed ? new File(hashedLogFilePath).getName() : new File(logFilePath).getName();
            String configuredPath = hashed ? hashedLogFilePath : logFilePath;
            String actualLogPath = findBestLogPath(logFileName, configuredPath);

            String timestamp = getDualTimestamp();

            writer = new PrintWriter(new FileWriter(actualLogPath, true));
            writer.println(timestamp + " | " + message);
            writer.flush();
        } catch (Exception e) {
            // Write failed - invalidate cache to force re-discovery on next write
            invalidateLogPathCache();
        } finally {
            if (writer != null) {
                try {
                    writer.close();
                } catch (Exception e) {
                    // Ignore close errors
                }
            }
        }
    }

    /**
     * Logs directly to /tmp without path selection - used for bootstrap messages
     * to avoid recursion when logging from within findBestLogPath()
     */
    private void logDirect(String message) {
        PrintWriter writer = null;
        try {
            String timestamp = getDualTimestamp();
            writer = new PrintWriter(new FileWriter("/tmp/cluster.log", true));
            writer.println(timestamp + " | GAL: " + message);
            writer.flush();
        } catch (Exception e) {
            // Silently ignore
        } finally {
            if (writer != null) {
                try {
                    writer.close();
                } catch (Exception e) {
                    // Ignore
                }
            }
        }
    }

    public void setClusterService(CombiBAPServiceNavi service) {
        this.clusterService = service;
        if (service != null) {
            logCluster("SYS: CombiBAPServiceNavi service AVAILABLE");
        } else {
            logCluster("SYS: CombiBAPServiceNavi service NULL");
        }
    }
    public void setPopupService(PartialPopupBAPService service) {
        this.popupService = service;
        if (service != null) {
            logCluster("SYS: PartialPopupBAPService service AVAILABLE");
        } else {
            logCluster("SYS: PartialPopupBAPService service NULL");
        }
    }

    public void setStatisticsService(ICarStatisticsService service) {
        this.statisticsService = service;
        if (service != null) {
            logCluster("SYS: ICarStatisticsService AVAILABLE");
        } else {
            logCluster("SYS: ICarStatisticsService NULL");
        }
    }

    public void setSysServices(ISysServices service) {
        this.sysServices = service;
        // Reset unit determination to force re-query with new service
        unitsDetermined = false;
        // Reset car time check to test with new service
        carTimeChecked = false;
        carTimeAvailable = false;
        // Test car time immediately
        tryGetCarTime();
        if (carTimeDebugInfo != null) {
            logCluster("SYS: Car time check - " + carTimeDebugInfo);
        }
        // Resolve auto bargraph mode now that sysServices is available
        resolveCarConfig();
    }

    public void setCarCoreServices(ICarCoreServices service) {
        this.carCoreServices = service;
        // Reset drive side determination
        driveSideDetermined = false;
        logDriveSideDebug();
    }

    public void setExteriorLightService(ICarExteriorLightService service) {
        this.exteriorLightService = service;
        logDriveSideDebug();
    }

    public void setKombiMapViewerControl(DSIMapViewerControl service) {
        this.kombiMapViewerControl = service;
        logCluster("KOMBI_MAP: DSIMapViewerControl(KOMBI=3) registered");
    }

    public void setMapClusterService(Object service) {
        this.mapClusterService = service;
        logCluster("KOMBI_MAP: IMapClusterService registered (class=" +
                   (service != null ? service.getClass().getName() : "null") + ")");
    }

    public void setStorageMountHandler(StorageMountHandler handler) {
        this.storageMountHandler = handler;
        if (handler != null) {
            logCluster("SYS: StorageMountHandler AVAILABLE");
        } else {
            logCluster("SYS: StorageMountHandler NULL");
        }
    }

    public void setForceImperial(boolean force) {
        this.forceImperial = force;
    }

    public void unsetClusterService(CombiBAPServiceNavi service) {
        this.clusterService = null;
    }

    /**
     * Determines whether to use metric (km/m) or imperial (mi/ft) units.
     * Returns true for metric, false for imperial.
     *
     * Strategy (3-tier detection):
     * 1. Override: forceImperial (for testing)
     * 2. ISysServices.units(): Query car menu settings directly (PRIORITY)
     * 3. JSON fallback: Country code lookup (if services not available)
     * 4. Safe default: Metric (if all else fails)
     *
     * Results are cached but can be refreshed by querying services.
     */
    private boolean shouldUseMetric() {
        // Return cached result if already determined
        if (unitsDetermined) {
            return useMetricCached;
        }

        // Override: if forceImperial is true, always use imperial (for testing)
        if (forceImperial) {
            logCluster("UNIT_SELECTION: Using IMPERIAL (forceImperial=true - testing override)");
            useMetricCached = false;
            unitsDetermined = true;
            return false;
        }

        // TIER 1: ISysServices - Query car unit settings directly (PRIORITY)
        if (sysServices != null) {
            try {
                IUnitManager unitManager = sysServices.units();
                if (unitManager != null) {
                    // Try distance unit first
                    IUnitProvider distanceProvider = unitManager.distance();
                    if (distanceProvider != null) {
                        ReadOnlyProperty settingProp = distanceProvider.setting();
                        if (settingProp != null) {
                            UnitSetting setting = (UnitSetting) settingProp.get();
                            if (setting != null) {
                                int distanceUnit = setting.getUnitValue();

                                // Unit values confirmed: 1 = km (metric), 2 = mi (imperial)
                                if (distanceUnit == 1) {
                                    logCluster("UNIT_SELECTION: Using METRIC (ISysServices distanceUnit=1 [km] - from car settings)");
                                    useMetricCached = true;
                                    unitsDetermined = true;
                                    return true;
                                } else if (distanceUnit == 2) {
                                    logCluster("UNIT_SELECTION: Using IMPERIAL (ISysServices distanceUnit=2 [mi] - from car settings)");
                                    useMetricCached = false;
                                    unitsDetermined = true;
                                    return false;
                                } else {
                                    // Unexpected value - log and fall through to speed unit or JSON
                                    logCluster("UNIT_SELECTION: Unexpected distanceUnit=" + distanceUnit + ", trying speed unit");
                                }
                            }
                        }
                    }

                    // Fallback to speed unit if distance unavailable
                    IUnitProvider speedProvider = unitManager.speed();
                    if (speedProvider != null) {
                        ReadOnlyProperty settingProp = speedProvider.setting();
                        if (settingProp != null) {
                            UnitSetting setting = (UnitSetting) settingProp.get();
                            if (setting != null) {
                                int speedUnit = setting.getUnitValue();

                                // Unit values confirmed: 1 = km/h (metric), 2 = mph (imperial)
                                if (speedUnit == 1) {
                                    logCluster("UNIT_SELECTION: Using METRIC (ISysServices speedUnit=1 [km/h] - from car settings)");
                                    useMetricCached = true;
                                    unitsDetermined = true;
                                    return true;
                                } else if (speedUnit == 2) {
                                    logCluster("UNIT_SELECTION: Using IMPERIAL (ISysServices speedUnit=2 [mph] - from car settings)");
                                    useMetricCached = false;
                                    unitsDetermined = true;
                                    return false;
                                } else {
                                    // Unexpected value - log and fall through to JSON
                                    logCluster("UNIT_SELECTION: Unexpected speedUnit=" + speedUnit + ", falling back to JSON");
                                }
                            }
                        }
                    }
                }
                logCluster("UNIT_SELECTION: ISysServices available but unit data null, falling back to JSON");
            } catch (Exception e) {
                logCluster("UNIT_SELECTION: Error querying ISysServices: " + e.toString() + ", falling back to JSON");
            }
        } else {
            logCluster("UNIT_SELECTION: ISysServices not available, falling back to JSON");
        }

        // TIER 2: JSON country code lookup (fallback if services not available)
        String countryCode = getCountryCode();

        if (countryCode != null && countryCode.length() > 0) {
            logCluster("UNIT_SELECTION: Detected country code = " + countryCode);

            // Look up country in JSON database
            boolean isImperial = lookupImperialInJSON(countryCode);

            // Also look up drive side while we have country code (only if car services didn't provide it)
            if (!driveSideDetermined) {
                isRHD = lookupDriveSideInJSON(countryCode);
                driveSideDetermined = true;
            }

            if (isImperial) {
                logCluster("UNIT_SELECTION: Using IMPERIAL (country=" + countryCode + " from JSON fallback)");
                useMetricCached = false;
                unitsDetermined = true;
                return false;  // Imperial
            } else {
                logCluster("UNIT_SELECTION: Using METRIC (country=" + countryCode + " from JSON fallback)");
                useMetricCached = true;
                unitsDetermined = true;
                return true;   // Metric
            }
        }

        // TIER 3: Safe default (metric) if all detection methods fail
        // This is the safest default as most of the world uses metric
        logCluster("UNIT_SELECTION: Using METRIC (safe default - no services, no JSON, no country code)");
        useMetricCached = true;
        unitsDetermined = true;
        return true;
    }

    /**
     * Determines if the car has right-hand drive (RHD).
     * Returns true for RHD (driver on right, left-hand traffic countries like UK, JP, AU)
     * Returns false for LHD (driver on left, right-hand traffic countries like US, DE, FR)
     *
     * Strategy (v58+):
     * 0. forceRHD config override (HIGHEST PRIORITY)
     * 1. ICarCoreServices: Query exteriorLight().leftHandTraffic()
     * 2. JSON fallback: Country code lookup (if services not available)
     * 3. Safe default: false (LHD - most countries)
     */

    /**
     * Resolves per-car config (bargraphMode, mirrorZoom, mirrorPan) from mirrorCarConfig JSON.
     * Key is "carClass_generation" e.g. "5_3" for Cayenne E3.
     * Only runs when sysServices is available. Global JSON values are used as fallback
     * when no matching car entry is found.
     */
    private void resolveCarConfig() {
        if (sysServices == null) return;
        try {
            de.audi.mib.system.config.ICarType ct = sysServices.config().carType();
            int carClass = ct.carClass();
            int generation = ct.generation();
            String key = carClass + "_" + generation;

            if (mirrorCarConfigJson == null) {
                logCluster("CAR_VARIANT: no mirrorCarConfig in JSON, using global values (class=" + carClass + " gen=" + generation + ")");
                return;
            }

            // find "key": { ... } inside mirrorCarConfigJson
            int keyPos = mirrorCarConfigJson.indexOf("\"" + key + "\"");
            if (keyPos < 0) {
                logCluster("CAR_VARIANT: no entry for " + key + " in mirrorCarConfig, using global values");
                return;
            }

            int brace = mirrorCarConfigJson.indexOf("{", keyPos);
            if (brace < 0) return;
            int end = mirrorCarConfigJson.indexOf("}", brace);
            if (end < 0) return;
            String entry = mirrorCarConfigJson.substring(brace, end + 1);

            // extract name (for logging only)
            String name = key;
            int namePos = entry.indexOf("\"name\"");
            if (namePos >= 0) {
                int q1 = entry.indexOf("\"", namePos + 6);
                q1 = entry.indexOf("\"", q1 + 1);  // skip the colon+space to opening quote
                int q2 = entry.indexOf("\"", q1 + 1);
                if (q1 >= 0 && q2 > q1) name = entry.substring(q1 + 1, q2);
            }

            // extract bargraphMode
            int bmPos = entry.indexOf("\"bargraphMode\"");
            if (bmPos >= 0) {
                int q1 = entry.indexOf("\"", entry.indexOf(":", bmPos) + 1);
                int q2 = entry.indexOf("\"", q1 + 1);
                if (q1 >= 0 && q2 > q1) bargraphMode = entry.substring(q1 + 1, q2);
            }

            // extract zoomX/zoomY/panX/panY
            int zxPos = entry.indexOf("\"zoomX\"");
            if (zxPos >= 0) { int c = entry.indexOf(":", zxPos); int e2 = entry.indexOf(",", c+1); if (e2<0) e2=entry.indexOf("}",c+1); if (e2>c) { try { mirrorZoomX = Float.parseFloat(entry.substring(c+1,e2).trim()); } catch (Exception ex) {} } }
            int zyPos = entry.indexOf("\"zoomY\"");
            if (zyPos >= 0) { int c = entry.indexOf(":", zyPos); int e2 = entry.indexOf(",", c+1); if (e2<0) e2=entry.indexOf("}",c+1); if (e2>c) { try { mirrorZoomY = Float.parseFloat(entry.substring(c+1,e2).trim()); } catch (Exception ex) {} } }
            int pxPos = entry.indexOf("\"panX\"");
            if (pxPos >= 0) { int c = entry.indexOf(":", pxPos); int e2 = entry.indexOf(",", c+1); if (e2<0) e2=entry.indexOf("}",c+1); if (e2>c) { try { mirrorPanX = Float.parseFloat(entry.substring(c+1,e2).trim()); } catch (Exception ex) {} } }
            int pyPos = entry.indexOf("\"panY\"");
            if (pyPos >= 0) { int c = entry.indexOf(":", pyPos); int e2 = entry.indexOf(",", c+1); if (e2<0) e2=entry.indexOf("}",c+1); if (e2>c) { try { mirrorPanY = Float.parseFloat(entry.substring(c+1,e2).trim()); } catch (Exception ex) {} } }

            // extract per-car aaClusterMode override
            int aaPos = entry.indexOf("\"aaClusterMode\"");
            if (aaPos >= 0) {
                int q1 = entry.indexOf("\"", entry.indexOf(":", aaPos) + 1);
                int q2 = entry.indexOf("\"", q1 + 1);
                if (q1 >= 0 && q2 > q1) aaClusterMode = entry.substring(q1 + 1, q2);
            }

            logCluster("CAR_VARIANT: " + name + " (" + key + ") -> bargraphMode=" + bargraphMode
                       + " zoomX=" + mirrorZoomX + " zoomY=" + mirrorZoomY
                       + " panX=" + mirrorPanX + " panY=" + mirrorPanY
                       + " aaClusterMode=" + aaClusterMode);

            // If mirror is already running with stale zoom/pan (videoAvailable fired before
            // resolveCarConfig completed), restart it now with the correct values.
            if (videoCurrentlyAvailable && enableMapRender) {
                String cmd = "start mode=" + mirrorMode
                           + " zoomX=" + mirrorZoomX + " zoomY=" + mirrorZoomY
                           + " panX=" + mirrorPanX + " panY=" + mirrorPanY;
                logCluster("CAR_VARIANT: restarting mirror with resolved params — " + cmd);
                sendMirrorCommand(cmd);
            }

        } catch (Exception e) {
            logCluster("CAR_VARIANT: error detecting variant: " + e.toString() + " -> using global values");
        }
    }

    private boolean isDriveRightHandSide() {
        // Return cached result if already determined
        if (driveSideDetermined) {
            return isRHD;
        }

        // TIER 0: forceRHD config override (HIGHEST PRIORITY)
        if (forceRHD) {
            isRHD = true;
            driveSideDetermined = true;
            logCluster("DRIVE_SIDE: Using RHD (forceRHD config override)");
            return isRHD;
        }

        // TIER 1: ICarCoreServices - Query car hardware config directly (PRIORITY)
        if (carCoreServices != null) {
            try {
                ReadOnlyProperty driverSideLeftProp = carCoreServices.configuration().airConditionMaster().driverSideLeft();
                if (driverSideLeftProp != null) {
                    Boolean driverSideLeft = (Boolean) driverSideLeftProp.get();
                    if (driverSideLeft != null) {
                        isRHD = !driverSideLeft.booleanValue();
                        driveSideDetermined = true;
                        logCluster("DRIVE_SIDE: Using " + (isRHD ? "RHD" : "LHD") +
                                     " (AirConditionMaster driverSideLeft=" + driverSideLeft + ")");
                        return isRHD;
                    }
                }
                logCluster("DRIVE_SIDE: AirConditionMaster.driverSideLeft null, falling back to JSON");
            } catch (Exception e) {
                logCluster("DRIVE_SIDE: Error querying driverSideLeft: " + e.toString() + ", falling back to JSON");
            }
        } else {
            logCluster("DRIVE_SIDE: ICarCoreServices not available, falling back to JSON");
        }

        // TIER 2: JSON country code lookup (fallback if services not available)
        String countryCode = getCountryCode();
        if (countryCode != null && countryCode.length() > 0) {
            isRHD = lookupDriveSideInJSON(countryCode);
            driveSideDetermined = true;
            return isRHD;
        }

        // TIER 3: Safe default (LHD - most countries)
        logCluster("DRIVE_SIDE: Using LHD (safe default - no services, no JSON)");
        isRHD = false;
        driveSideDetermined = true;
        return false;
    }

    /**
     * Logs all available drive-side related properties for diagnostic purposes.
     */
    private void logDriveSideDebug() {
        try {
            logCluster("DRIVE_SIDE_DEBUG: === Comprehensive drive side check ===");

            // 1. ExtLightConfig.leftHandTraffic (from ICarCoreServices)
            if (carCoreServices != null) {
                try {
                    ReadOnlyProperty leftHandTrafficProp = carCoreServices.configuration().exteriorLight().leftHandTraffic();
                    if (leftHandTrafficProp != null) {
                        Boolean lht = (Boolean) leftHandTrafficProp.get();
                        logCluster("DRIVE_SIDE_DEBUG: ExtLightConfig.leftHandTraffic = " + lht);
                    } else {
                        logCluster("DRIVE_SIDE_DEBUG: ExtLightConfig.leftHandTraffic = null (property not available)");
                    }
                } catch (Exception e) {
                    logCluster("DRIVE_SIDE_DEBUG: ExtLightConfig.leftHandTraffic ERROR: " + e.toString());
                }
            } else {
                logCluster("DRIVE_SIDE_DEBUG: ICarCoreServices not available");
            }

            // 2. AirConditionMaster.driverSideLeft (from ICarCoreServices - physical steering side)
            if (carCoreServices != null) {
                try {
                    ReadOnlyProperty driverSideLeftProp = carCoreServices.configuration().airConditionMaster().driverSideLeft();
                    if (driverSideLeftProp != null) {
                        Boolean dsl = (Boolean) driverSideLeftProp.get();
                        logCluster("DRIVE_SIDE_DEBUG: AirConditionMaster.driverSideLeft = " + dsl);
                    } else {
                        logCluster("DRIVE_SIDE_DEBUG: AirConditionMaster.driverSideLeft = null");
                    }
                } catch (Exception e) {
                    logCluster("DRIVE_SIDE_DEBUG: AirConditionMaster.driverSideLeft ERROR: " + e.toString());
                }
            }

            // 3. IExteriorLightResource.touristState (from ICarExteriorLightService)
            if (exteriorLightService != null) {
                try {
                    ReadOnlyProperty touristProp = exteriorLightService.exteriorLight().touristState();
                    if (touristProp != null) {
                        Boolean tourist = (Boolean) touristProp.get();
                        logCluster("DRIVE_SIDE_DEBUG: ExteriorLight.touristState = " + tourist);
                    } else {
                        logCluster("DRIVE_SIDE_DEBUG: ExteriorLight.touristState = null (property not available)");
                    }
                } catch (Exception e) {
                    logCluster("DRIVE_SIDE_DEBUG: ExteriorLight.touristState ERROR: " + e.toString());
                }

                // 3. Log all other light properties that might be relevant
                try {
                    ReadOnlyProperty daylightProp = exteriorLightService.exteriorLight().daylight();
                    if (daylightProp != null) {
                        logCluster("DRIVE_SIDE_DEBUG: ExteriorLight.daylight = " + daylightProp.get());
                    }
                } catch (Exception e) {
                    // ignore
                }

                try {
                    ReadOnlyProperty headlightProp = exteriorLightService.exteriorLight().headLightSystem();
                    if (headlightProp != null) {
                        logCluster("DRIVE_SIDE_DEBUG: ExteriorLight.headLightSystem = " + headlightProp.get());
                    }
                } catch (Exception e) {
                    // ignore
                }

                try {
                    ReadOnlyProperty lightGuidanceProp = exteriorLightService.exteriorLight().lightGuidance();
                    if (lightGuidanceProp != null) {
                        logCluster("DRIVE_SIDE_DEBUG: ExteriorLight.lightGuidance = " + lightGuidanceProp.get());
                    }
                } catch (Exception e) {
                    // ignore
                }

                try {
                    ReadOnlyProperty lightAssistProp = exteriorLightService.exteriorLight().lightAssistance();
                    if (lightAssistProp != null) {
                        logCluster("DRIVE_SIDE_DEBUG: ExteriorLight.lightAssistance = " + lightAssistProp.get());
                    }
                } catch (Exception e) {
                    // ignore
                }
            } else {
                logCluster("DRIVE_SIDE_DEBUG: ICarExteriorLightService not available");
            }

            // 4. REGION property
            try {
                String region = System.getProperty("REGION");
                logCluster("DRIVE_SIDE_DEBUG: System.REGION = " + region);
            } catch (Exception e) {
                // ignore
            }

            // 5. Country code
            String cc = getCountryCode();
            logCluster("DRIVE_SIDE_DEBUG: getCountryCode() = " + cc);

            // 6. Current isDriveRightHandSide cached value
            logCluster("DRIVE_SIDE_DEBUG: isRHD=" + isRHD + " driveSideDetermined=" + driveSideDetermined);
            logCluster("DRIVE_SIDE_DEBUG: === End comprehensive check ===");

        } catch (Exception e) {
            logCluster("DRIVE_SIDE_DEBUG: Error during comprehensive check: " + e.toString());
        }
    }

    /**
     * Attempts to get country code from system properties.
     * Returns 2-letter ISO country code (e.g., "GB", "US", "DE") or null.
     */
    private String getCountryCode() {
        // Try system property REGION (might have country info)
        try {
            String region = System.getProperty("REGION");
            if (region != null) {
                logCluster("UNIT_SELECTION: System REGION = " + region);
            }
        } catch (Exception e) {
            // Ignore
        }

        // Try user.country (unreliable but worth trying)
        try {
            String userCountry = System.getProperty("user.country");
            if (userCountry != null && userCountry.length() == 2) {
                logCluster("UNIT_SELECTION: user.country = " + userCountry);
                // Don't trust "US" unless we have no other option
                if (!userCountry.equals("US")) {
                    return userCountry;
                }
            }
        } catch (Exception e) {
            // Ignore
        }

        // Try Locale.getDefault().getCountry()
        try {
            String country = java.util.Locale.getDefault().getCountry();
            if (country != null && country.length() == 2) {
                logCluster("UNIT_SELECTION: Locale country = " + country);
                // Don't trust "US" unless we have no other option
                if (!country.equals("US")) {
                    return country;
                }
            }
        } catch (Exception e) {
            // Ignore
        }

        // Try parsing HMI language (e.g., "en_GB" -> "GB")
        try {
            String hmiLang = System.getProperty("languages.hmi.builtin");
            if (hmiLang != null && hmiLang.indexOf("en_GB") >= 0) {
                logCluster("UNIT_SELECTION: Detected en_GB in HMI languages");
                return "GB";
            }
            // Try to extract other country codes from HMI languages
            // Format: "de_DE0,en_GB0,fr_FR0..."
            if (hmiLang != null && hmiLang.length() > 0) {
                // Try to find first language code (e.g., "de_DE")
                int underscorePos = hmiLang.indexOf('_');
                if (underscorePos >= 0 && underscorePos + 2 < hmiLang.length()) {
                    String potentialCountry = hmiLang.substring(underscorePos + 1, underscorePos + 3);
                    if (potentialCountry.length() == 2) {
                        logCluster("UNIT_SELECTION: Extracted country from HMI = " + potentialCountry);
                        return potentialCountry;
                    }
                }
            }
        } catch (Exception e) {
            // Ignore
        }

        logCluster("UNIT_SELECTION: Could not determine country code");
        return null;
    }

    /**
     * Looks up country code in embedded JSON to determine if it uses imperial units.
     * Returns true if imperial, false if metric or not found.
     */
    private boolean lookupImperialInJSON(String countryCode) {
        if (loadedConfigJson == null) {
            logCluster("UNIT_SELECTION: Config not loaded yet");
            return false;
        }
        try {
            String searchPattern = "\"code\": \"" + countryCode + "\"";
            int codePos = loadedConfigJson.indexOf(searchPattern);
            if (codePos < 0) {
                searchPattern = "\"code\":\"" + countryCode + "\"";
                codePos = loadedConfigJson.indexOf(searchPattern);
            }
            if (codePos < 0) {
                logCluster("UNIT_SELECTION: Country " + countryCode + " not found in config");
                return false;
            }
            int imperialPos = loadedConfigJson.indexOf("\"imperial\":", codePos);
            if (imperialPos < 0 || imperialPos > codePos + 200) {
                logCluster("UNIT_SELECTION: imperial field not found for " + countryCode);
                return false;
            }
            int truePos = loadedConfigJson.indexOf("true", imperialPos);
            int falsePos = loadedConfigJson.indexOf("false", imperialPos);
            if (truePos >= 0 && truePos < imperialPos + 20) {
                logCluster("UNIT_SELECTION: Country " + countryCode + " uses IMPERIAL (from config)");
                return true;
            } else if (falsePos >= 0 && falsePos < imperialPos + 20) {
                logCluster("UNIT_SELECTION: Country " + countryCode + " uses METRIC (from config)");
                return false;
            }
            logCluster("UNIT_SELECTION: Could not parse imperial value for " + countryCode);
            return false;
        } catch (Exception e) {
            logCluster("UNIT_SELECTION: Error: " + e.toString());
            return false;
        }
    }

    /**
     * Looks up country code in embedded JSON to determine drive side (LHD/RHD).
     * Returns true if RHD (right-hand drive), false if LHD (left-hand drive).
     */
    private boolean lookupDriveSideInJSON(String countryCode) {
        if (loadedConfigJson == null) {
            logCluster("DRIVE_SIDE: Config not loaded yet");
            return false;
        }
        try {
            String searchPattern = "\"code\": \"" + countryCode + "\"";
            int codePos = loadedConfigJson.indexOf(searchPattern);
            if (codePos < 0) {
                searchPattern = "\"code\":\"" + countryCode + "\"";
                codePos = loadedConfigJson.indexOf(searchPattern);
            }
            if (codePos < 0) {
                logCluster("DRIVE_SIDE: Country " + countryCode + " not found in config");
                return false;
            }
            int rhdPos = loadedConfigJson.indexOf("\"rhd\":", codePos);
            if (rhdPos < 0 || rhdPos > codePos + 200) {
                logCluster("DRIVE_SIDE: rhd field not found for " + countryCode);
                return false;
            }
            int truePos = loadedConfigJson.indexOf("true", rhdPos);
            int falsePos = loadedConfigJson.indexOf("false", rhdPos);
            if (truePos >= 0 && truePos < rhdPos + 20) {
                logCluster("DRIVE_SIDE: Country " + countryCode + " is RHD (from config)");
                return true;
            } else if (falsePos >= 0 && falsePos < rhdPos + 20) {
                logCluster("DRIVE_SIDE: Country " + countryCode + " is LHD (from config)");
                return false;
            }
            logCluster("DRIVE_SIDE: Could not parse rhd value for " + countryCode);
            return false;
        } catch (Exception e) {
            logCluster("DRIVE_SIDE: Error: " + e.toString());
            return false;
        }
    }

    /**
     * Resolves "auto" imperialSmallUnit using country data from the already-loaded config JSON.
     * Called once on first imperial distance display, result is cached.
     */
    private String resolveImperialSmallUnitFromConfig() {
        try {
            String cc = getCountryCode();
            if (cc == null || cc.length() == 0 || loadedConfigJson == null) {
                logCluster("IMPERIAL_SMALL_UNIT: No country code or config, defaulting to yards");
                return "yards";
            }

            // Search countries array in the already-loaded config JSON
            String searchPattern = "\"code\": \"" + cc + "\"";
            int codePos = loadedConfigJson.indexOf(searchPattern);
            if (codePos < 0) {
                // Try lowercase variant
                searchPattern = "\"code\":\"" + cc + "\"";
                codePos = loadedConfigJson.indexOf(searchPattern);
            }

            if (codePos < 0) {
                logCluster("IMPERIAL_SMALL_UNIT: Country " + cc + " not in config, defaulting to yards");
                return "yards";
            }

            int fieldPos = loadedConfigJson.indexOf("\"imperialSmallUnit\":", codePos);
            if (fieldPos < 0 || fieldPos > codePos + 250) {
                logCluster("IMPERIAL_SMALL_UNIT: No imperialSmallUnit for " + cc + ", defaulting to yards");
                return "yards";
            }

            String remainder = loadedConfigJson.substring(fieldPos + 20).trim();
            if (remainder.startsWith("\"feet\"") || remainder.startsWith("\"foot\"")) {
                logCluster("IMPERIAL_SMALL_UNIT: Country " + cc + " -> feet (from config)");
                return "feet";
            } else {
                logCluster("IMPERIAL_SMALL_UNIT: Country " + cc + " -> yards (from config)");
                return "yards";
            }
        } catch (Exception e) {
            logCluster("IMPERIAL_SMALL_UNIT: Error: " + e.toString() + ", defaulting to yards");
            return "yards";
        }
    }

    /**
     * Loads configuration options from embedded JSON file or external override.
     * First checks USB/SD card for config override, then falls back to embedded file.
     * Reads: enableFileLogging, enableRoundaboutLatching, enableHeartbeat, distanceUpdateThreshold,
     *        updateRateLimit, logFileSize, forceImperial, forceRHD
     */
    private void loadConfigFromJSON() {
        if (configLoaded) return;  // Only load once

        // First try to load from external media (USB/SD)
        String json = loadExternalConfigIfExists();
        
        // If no external config, load from installed file (written by install.sh)
        if (json == null) {
            json = loadInstalledConfig();
            if (json != null) {
                logCluster("CONFIG: Using installed config at " + CLUSTER_CONFIG_INSTALLED);
            }
        } else {
            logCluster("CONFIG: Using external config file");
        }
        
        if (json == null) {
            logCluster("CONFIG: No config file found, using defaults");
            return;  // Don't set configLoaded=true so we can retry later
        }

        parseConfigFromJSON(json);
        
        // Log final config status after parsing is complete
        logCluster("CONFIG: Loaded - fileLogging=" + fileLoggingEnabled +
                     ", fileHashing=" + fileHashingEnabled +
                     ", externalLogging=" + enableExternalLogging +
                     ", roundaboutLatching=" + roundaboutLatchingEnabled +
                     ", heartbeat=" + heartbeatEnabled + "@" + heartbeatInterval + "ms" +
                     ", forceImperial=" + forceImperial +
                     ", forceRHD=" + forceRHD +
                     ", logFilePath=" + logFilePath +
                     ", hashedLogFilePath=" + hashedLogFilePath +
                     ", logFileSize=" + logFileSize + "MB" +
                     ", version=" + buildVersion);

        // Log dynamic threshold configuration (for debugging/tuning)
        logCluster("CONFIG: Dynamic thresholds - " +
                     "VeryFar=" + veryFarDistanceThreshold + "m/" + veryFarRateLimit + "ms, " +
                     "Far=" + farDistanceThreshold + "m/" + farRateLimit + "ms, " +
                     "Approaching=" + approachingDistanceThreshold + "m/" + approachingRateLimit + "ms, " +
                     "Near=" + nearDistanceThreshold + "m/" + nearRateLimit + "ms, " +
                     "Close=" + closeDistanceThreshold + "m/" + closeRateLimit + "ms, " +
                     "VeryClose=" + veryCloseDistanceThreshold + "m/" + veryCloseRateLimit + "ms, " +
                     "Now=" + nowDistanceThreshold + "m/" + nowRateLimit + "ms");
                     
        configLoaded = true;

        // Resolve auto bargraph mode if sysServices already available at config load time
        resolveCarConfig();
    }

    /**
     * Loads embedded config from JAR resources
     */
    /**
     * Loads the installed config from /mnt/app/eso/bin/apps/cluster/cluster_config.json.
     * install.sh extracts this file from the deploy package; the JAR no longer
     * embeds it. Returns null if the file isn't present or unreadable, in
     * which case callers fall back to compiled-in defaults.
     */
    private String loadInstalledConfig() {
        java.io.BufferedReader reader = null;
        try {
            File f = new File(CLUSTER_CONFIG_INSTALLED);
            if (!f.exists() || !f.canRead()) {
                logCluster("CONFIG: " + CLUSTER_CONFIG_INSTALLED + " not present");
                return null;
            }
            reader = new java.io.BufferedReader(new java.io.FileReader(f));
            StringBuffer jsonContent = new StringBuffer();
            String line;
            while ((line = reader.readLine()) != null) {
                jsonContent.append(line);
            }
            return jsonContent.toString();
        } catch (Exception e) {
            logCluster("CONFIG: Error loading installed config: " + e.toString());
            return null;
        } finally {
            try { if (reader != null) reader.close(); } catch (Exception e) { /* ignore */ }
        }
    }
    
    /**
     * Parses configuration from JSON string
     */
    private void parseConfigFromJSON(String json) {
        this.loadedConfigJson = json; // cache for lazy country lookups
        try {
            // Parse config section (manual parsing for Java 1.4)
            int configStart = json.indexOf("\"config\"");
            if (configStart < 0) {
                logCluster("CONFIG: No config section in JSON, using defaults");
                return;
            }

            // Find enableFileLogging
            int fileLoggingPos = json.indexOf("\"enableFileLogging\"", configStart);
            if (fileLoggingPos >= 0 && fileLoggingPos < configStart + 500) {
                int colonPos = json.indexOf(":", fileLoggingPos);
                if (colonPos > 0) {
                    String valueSection = json.substring(colonPos + 1, Math.min(colonPos + 20, json.length()));
                    if (valueSection.indexOf("true") >= 0) {
                        fileLoggingEnabled = true;
                    } else if (valueSection.indexOf("false") >= 0) {
                        fileLoggingEnabled = false;
                    }
                }
            }

            // Find enableFileHashing - look for the exact pattern
            int hashingStart = json.indexOf("\"enableFileHashing\"");
            if (hashingStart >= 0) {
                int colonPos = json.indexOf(":", hashingStart);
                if (colonPos > 0) {
                    int start = colonPos + 1;
                    // Skip whitespace
                    while (start < json.length() && (json.charAt(start) == ' ' || json.charAt(start) == '\t' || json.charAt(start) == '\n')) {
                        start++;
                    }
                    if (start < json.length()) {
                        if (json.substring(start).startsWith("true")) {
                            fileHashingEnabled = true;
                        } else if (json.substring(start).startsWith("false")) {
                            fileHashingEnabled = false;
                        }
                    }
                }
            }

            // Find enableExternalLogging
            int externalLoggingPos = json.indexOf("\"enableExternalLogging\"", configStart);
            if (externalLoggingPos >= 0 && externalLoggingPos < configStart + 500) {
                int colonPos = json.indexOf(":", externalLoggingPos);
                if (colonPos > 0) {
                    String valueSection = json.substring(colonPos + 1, Math.min(colonPos + 20, json.length()));
                    if (valueSection.indexOf("true") >= 0) {
                        enableExternalLogging = true;
                    } else if (valueSection.indexOf("false") >= 0) {
                        enableExternalLogging = false;
                    }
                }
            }

            // Find enableRoundaboutLatching
            int latchingPos = json.indexOf("\"enableRoundaboutLatching\"", configStart);
            if (latchingPos >= 0 && latchingPos < configStart + 500) {
                int colonPos = json.indexOf(":", latchingPos);
                if (colonPos > 0) {
                    String valueSection = json.substring(colonPos + 1, Math.min(colonPos + 20, json.length()));
                    if (valueSection.indexOf("true") >= 0) {
                        roundaboutLatchingEnabled = true;
                    } else if (valueSection.indexOf("false") >= 0) {
                        roundaboutLatchingEnabled = false;
                    }
                }
            }

            // Find forceImperial
            int forceImperialPos = json.indexOf("\"forceImperial\"", configStart);
            if (forceImperialPos >= 0 && forceImperialPos < configStart + 500) {
                int colonPos = json.indexOf(":", forceImperialPos);
                if (colonPos > 0) {
                    String valueSection = json.substring(colonPos + 1, Math.min(colonPos + 20, json.length()));
                    if (valueSection.indexOf("true") >= 0) {
                        forceImperial = true;
                    } else if (valueSection.indexOf("false") >= 0) {
                        forceImperial = false;
                    }
                }
            }
            
            // Find forceRHD
            int forceRHDPos = json.indexOf("\"forceRHD\"", configStart);
            if (forceRHDPos >= 0 && forceRHDPos < configStart + 500) {
                int colonPos = json.indexOf(":", forceRHDPos);
                if (colonPos > 0) {
                    String valueSection = json.substring(colonPos + 1, Math.min(colonPos + 20, json.length()));
                    if (valueSection.indexOf("true") >= 0) {
                        forceRHD = true;
                    } else if (valueSection.indexOf("false") >= 0) {
                        forceRHD = false;
                    }
                }
            }

            // Find enableHeartbeat
            int heartbeatPos = json.indexOf("\"enableHeartbeat\"", configStart);
            if (heartbeatPos >= 0 && heartbeatPos < configStart + 500) {
                int colonPos = json.indexOf(":", heartbeatPos);
                if (colonPos > 0) {
                    String valueSection = json.substring(colonPos + 1, Math.min(colonPos + 20, json.length()));
                    if (valueSection.indexOf("true") >= 0) {
                        heartbeatEnabled = true;
                    } else if (valueSection.indexOf("false") >= 0) {
                        heartbeatEnabled = false;
                    }
                }
            }

            // Find maneuverStateMask (integer bitmask: bit0=state1..bit3=state4, 0=disabled)
            int maskPos = json.indexOf("\"maneuverStateMask\"", configStart);
            if (maskPos >= 0 && maskPos < configStart + 500) {
                int colonPos = json.indexOf(":", maskPos);
                if (colonPos > 0) {
                    int numStart = colonPos + 1;
                    while (numStart < json.length() && !Character.isDigit(json.charAt(numStart))) numStart++;
                    int numEnd = numStart;
                    while (numEnd < json.length() && Character.isDigit(json.charAt(numEnd))) numEnd++;
                    try {
                        maneuverStateMask = Integer.parseInt(json.substring(numStart, numEnd));
                        logCluster("CONFIG: maneuverStateMask = 0x" + Integer.toHexString(maneuverStateMask)
                                + " (states:" + (((maneuverStateMask & 1) != 0) ? " 1" : "")
                                + (((maneuverStateMask & 2) != 0) ? " 2" : "")
                                + (((maneuverStateMask & 4) != 0) ? " 3" : "")
                                + (((maneuverStateMask & 8) != 0) ? " 4" : "")
                                + ")");
                    } catch (Exception e) { /* ignore */ }
                }
            }

            // Find logFilePath
            int logFilePathPos = json.indexOf("\"logFilePath\"", configStart);
            if (logFilePathPos >= 0 && logFilePathPos < configStart + 500) {
                int colon = json.indexOf(":", logFilePathPos);
                int q1 = json.indexOf("\"", colon + 1);
                int q2 = json.indexOf("\"", q1 + 1);
                if (q1 > 0 && q2 > q1) {
                    String path = json.substring(q1 + 1, q2);
                    if (path != null && path.length() > 0) {
                        logFilePath = path;
                    }
                }
            }
            
            // Find hashedLogFilePath
            int hashedLogFilePathPos = json.indexOf("\"hashedLogFilePath\"", configStart);
            if (hashedLogFilePathPos >= 0 && hashedLogFilePathPos < configStart + 500) {
                int colon = json.indexOf(":", hashedLogFilePathPos);
                int q1 = json.indexOf("\"", colon + 1);
                int q2 = json.indexOf("\"", q1 + 1);
                if (q1 > 0 && q2 > q1) {
                    String path = json.substring(q1 + 1, q2);
                    if (path != null && path.length() > 0) {
                        hashedLogFilePath = path;
                    }
                }
            }

            // Find logFileSize (in MB)
            int logFileSizePos = json.indexOf("\"logFileSize\"", configStart);
            if (logFileSizePos >= 0 && logFileSizePos < configStart + 500) {
                int colon = json.indexOf(":", logFileSizePos);
                if (colon > 0) {
                    String valueSection = json.substring(colon + 1, Math.min(colon + 20, json.length()));
                    try {
                        int start = 0;
                        while (start < valueSection.length() && !Character.isDigit(valueSection.charAt(start))) {
                            start++;
                        }
                        int end = start;
                        while (end < valueSection.length() && Character.isDigit(valueSection.charAt(end))) {
                            end++;
                        }
                        if (start < end) {
                            String numStr = valueSection.substring(start, end);
                            long sizeMB = Long.parseLong(numStr);
                            if (sizeMB > 0 && sizeMB <= 100) {  // Reasonable limit: 1-100 MB
                                logFileSize = sizeMB;
                            }
                        }
                    } catch (Exception e) {
                        // Keep default if parsing fails
                    }
                }
            }

            // Parse heartbeat interval
            heartbeatInterval = parseIntValue(json, "heartbeatInterval", configStart, heartbeatInterval, 100, 10000);

            // Write session marker now - paths are finalised, first logCluster() call is below
            writeSessionStartMarkers();

            // Parse bargraph mode (for platforms that show either distance OR bargraph)
            int bargraphModeIdx = json.indexOf("\"bargraphMode\"", configStart);
            if (bargraphModeIdx >= 0 && bargraphModeIdx < configStart + 500) {
                int valueStart = json.indexOf("\"", bargraphModeIdx + 14);
                if (valueStart >= 0) {
                    int valueEnd = json.indexOf("\"", valueStart + 1);
                    if (valueEnd > valueStart) {
                        String mode = json.substring(valueStart + 1, valueEnd).trim();
                        if (mode.equals("auto") || mode.equals("always") || mode.equals("distance") || mode.equals("dynamic")) {
                            bargraphMode = mode;
                            logCluster("CONFIG: bargraphMode = " + bargraphMode);
                        }
                    }
                }
            }

            // Parse dynamic bargraph distance (meters - switch threshold for dynamic mode)
            dynamicBargraphDistance = parseIntValue(json, "dynamicBargraphDistance", configStart, dynamicBargraphDistance, 10, 500);
            logCluster("CONFIG: dynamicBargraphDistance = " + dynamicBargraphDistance + "m");

            // Parse dynamic bargraph percentage (for short maneuvers in dynamic mode)
            dynamicBargraphPercent = parseIntValue(json, "dynamicBargraphPercent", configStart, dynamicBargraphPercent, 10, 90);
            logCluster("CONFIG: dynamicBargraphPercent = " + dynamicBargraphPercent + "%");

            // Parse unit switching thresholds
            metricUnitThreshold = parseIntValue(json, "metricUnitThreshold", configStart, metricUnitThreshold, 50, 5000);
            logCluster("CONFIG: metricUnitThreshold = " + metricUnitThreshold + "m (switch to meters below this)");

            imperialUnitThreshold = parseIntValue(json, "imperialUnitThreshold", configStart, imperialUnitThreshold, 50, 5000);
            logCluster("CONFIG: imperialUnitThreshold = " + imperialUnitThreshold + "m (switch to small unit below this)");

            // Find imperialSmallUnit
            int smallUnitPos = json.indexOf("\"imperialSmallUnit\"", configStart);
            if (smallUnitPos >= 0 && smallUnitPos < configStart + 500) {
                int colonPos = json.indexOf(":", smallUnitPos);
                if (colonPos >= 0) {
                    String remainder = json.substring(colonPos + 1).trim();
                    if (remainder.startsWith("\"yards\"") || remainder.startsWith("\"yard\"")) {
                        imperialSmallUnit = "yards";
                    } else if (remainder.startsWith("\"feet\"") || remainder.startsWith("\"foot\"")) {
                        imperialSmallUnit = "feet";
                    } else if (remainder.startsWith("\"auto\"")) {
                        imperialSmallUnit = "auto";
                    }
                }
            }
            logCluster("CONFIG: imperialSmallUnit = " + imperialSmallUnit);

            // Parse destination display duration
            destinationDisplayDuration = parseIntValue(json, "destinationDisplayDuration", configStart, destinationDisplayDuration, 0, 10000);
            logCluster("CONFIG: destinationDisplayDuration = " + destinationDisplayDuration + "ms");

            // Parse welcome display duration

            // Parse dynamic threshold configuration (nested section)
            int thresholdsStart = json.indexOf("\"dynamicThresholds\"");
            if (thresholdsStart >= 0) {
                int thresholdsEnd = json.indexOf("}", thresholdsStart);
                if (thresholdsEnd > thresholdsStart) {
                    // Parse veryFar
                    int veryFarStart = json.indexOf("\"veryFar\"", thresholdsStart);
                    if (veryFarStart >= 0 && veryFarStart < thresholdsEnd) {
                        veryFarBoundary = parseIntValue(json, "boundary", veryFarStart, veryFarBoundary, 1, 50000);
                        veryFarDistanceThreshold = parseIntValue(json, "distance", veryFarStart, veryFarDistanceThreshold, 1, 1000);
                        veryFarRateLimit = parseIntValue(json, "rateLimit", veryFarStart, veryFarRateLimit, 0, 10000);
                    }

                    // Parse far
                    int farStart = json.indexOf("\"far\"", thresholdsStart);
                    if (farStart >= 0 && farStart < thresholdsEnd) {
                        farBoundary = parseIntValue(json, "boundary", farStart, farBoundary, 1, 50000);
                        farDistanceThreshold = parseIntValue(json, "distance", farStart, farDistanceThreshold, 1, 1000);
                        farRateLimit = parseIntValue(json, "rateLimit", farStart, farRateLimit, 0, 10000);
                    }

                    // Parse approaching
                    int approachingStart = json.indexOf("\"approaching\"", thresholdsStart);
                    if (approachingStart >= 0 && approachingStart < thresholdsEnd) {
                        approachingBoundary = parseIntValue(json, "boundary", approachingStart, approachingBoundary, 1, 50000);
                        approachingDistanceThreshold = parseIntValue(json, "distance", approachingStart, approachingDistanceThreshold, 1, 1000);
                        approachingRateLimit = parseIntValue(json, "rateLimit", approachingStart, approachingRateLimit, 0, 10000);
                    }

                    // Parse near
                    int nearStart = json.indexOf("\"near\"", thresholdsStart);
                    if (nearStart >= 0 && nearStart < thresholdsEnd) {
                        nearBoundary = parseIntValue(json, "boundary", nearStart, nearBoundary, 1, 50000);
                        nearDistanceThreshold = parseIntValue(json, "distance", nearStart, nearDistanceThreshold, 1, 1000);
                        nearRateLimit = parseIntValue(json, "rateLimit", nearStart, nearRateLimit, 0, 10000);
                    }

                    // Parse close
                    int closeStart = json.indexOf("\"close\"", thresholdsStart);
                    if (closeStart >= 0 && closeStart < thresholdsEnd) {
                        closeBoundary = parseIntValue(json, "boundary", closeStart, closeBoundary, 1, 50000);
                        closeDistanceThreshold = parseIntValue(json, "distance", closeStart, closeDistanceThreshold, 1, 1000);
                        closeRateLimit = parseIntValue(json, "rateLimit", closeStart, closeRateLimit, 0, 10000);
                    }

                    // Parse veryClose
                    int veryCloseStart = json.indexOf("\"veryClose\"", thresholdsStart);
                    if (veryCloseStart >= 0 && veryCloseStart < thresholdsEnd) {
                        veryCloseBoundary = parseIntValue(json, "boundary", veryCloseStart, veryCloseBoundary, 1, 50000);
                        veryCloseDistanceThreshold = parseIntValue(json, "distance", veryCloseStart, veryCloseDistanceThreshold, 1, 1000);
                        veryCloseRateLimit = parseIntValue(json, "rateLimit", veryCloseStart, veryCloseRateLimit, 0, 10000);
                    }

                    // Parse now
                    int nowStart = json.indexOf("\"now\"", thresholdsStart);
                    if (nowStart >= 0 && nowStart < thresholdsEnd) {
                        nowDistanceThreshold = parseIntValue(json, "distance", nowStart, nowDistanceThreshold, 1, 1000);
                        nowRateLimit = parseIntValue(json, "rateLimit", nowStart, nowRateLimit, 0, 10000);
                    }
                }
            }

	            // Map rendering config
            int mapRenderPos = json.indexOf("\"enableMapRender\"", configStart);
            if (mapRenderPos >= 0 && mapRenderPos < configStart + 2000) {
                int colonPos = json.indexOf(":", mapRenderPos);
                if (colonPos > 0) {
                    String rest = json.substring(colonPos + 1).trim();
                    enableMapRender = rest.startsWith("true");
                }
            }

            int mirrorFifoPos = json.indexOf("\"mirrorFifo\"", configStart);
            if (mirrorFifoPos >= 0 && mirrorFifoPos < configStart + 2000) {
                int colon = json.indexOf(":", mirrorFifoPos);
                int q1 = json.indexOf("\"", colon + 1);
                int q2 = json.indexOf("\"", q1 + 1);
                if (q1 > 0 && q2 > q1) mirrorFifo = json.substring(q1 + 1, q2);
            }

            int mirrorModePos = json.indexOf("\"mirrorMode\"", configStart);
            if (mirrorModePos >= 0 && mirrorModePos < configStart + 2000) {
                int colon = json.indexOf(":", mirrorModePos);
                int q1 = json.indexOf("\"", colon + 1);
                int q2 = json.indexOf("\"", q1 + 1);
                if (q1 > 0 && q2 > q1) mirrorMode = json.substring(q1 + 1, q2);
            }

            int mirrorZoomXPos = json.indexOf("\"mirrorZoomX\"", configStart);
            if (mirrorZoomXPos >= 0 && mirrorZoomXPos < configStart + 2000) {
                int colon = json.indexOf(":", mirrorZoomXPos);
                int end = json.indexOf(",", colon + 1);
                if (end < 0) end = json.indexOf("}", colon + 1);
                if (end > colon) { try { mirrorZoomX = Float.parseFloat(json.substring(colon + 1, end).trim()); } catch (Exception ex) {} }
            }

            int mirrorZoomYPos = json.indexOf("\"mirrorZoomY\"", configStart);
            if (mirrorZoomYPos >= 0 && mirrorZoomYPos < configStart + 2000) {
                int colon = json.indexOf(":", mirrorZoomYPos);
                int end = json.indexOf(",", colon + 1);
                if (end < 0) end = json.indexOf("}", colon + 1);
                if (end > colon) { try { mirrorZoomY = Float.parseFloat(json.substring(colon + 1, end).trim()); } catch (Exception ex) {} }
            }

            int mirrorPanXPos = json.indexOf("\"mirrorPanX\"", configStart);
            if (mirrorPanXPos >= 0 && mirrorPanXPos < configStart + 2000) {
                int colon = json.indexOf(":", mirrorPanXPos);
                int end = json.indexOf(",", colon + 1);
                if (end < 0) end = json.indexOf("}", colon + 1);
                if (end > colon) { try { mirrorPanX = Float.parseFloat(json.substring(colon + 1, end).trim()); } catch (Exception ex) {} }
            }

            int mirrorPanYPos = json.indexOf("\"mirrorPanY\"", configStart);
            if (mirrorPanYPos >= 0 && mirrorPanYPos < configStart + 2000) {
                int colon = json.indexOf(":", mirrorPanYPos);
                int end = json.indexOf(",", colon + 1);
                if (end < 0) end = json.indexOf("}", colon + 1);
                if (end > colon) { try { mirrorPanY = Float.parseFloat(json.substring(colon + 1, end).trim()); } catch (Exception ex) {} }
            }

            int aaModePos = json.indexOf("\"aaClusterMode\"", configStart);
            if (aaModePos >= 0 && aaModePos < configStart + 2000) {
                int colon = json.indexOf(":", aaModePos);
                int q1 = json.indexOf("\"", colon + 1);
                int q2 = json.indexOf("\"", q1 + 1);
                if (q1 > 0 && q2 > q1) aaClusterMode = json.substring(q1 + 1, q2);
            }

            // mirrorCarConfig — extract raw JSON object block for per-car overrides
            int carCfgPos = json.indexOf("\"mirrorCarConfig\"");
            if (carCfgPos >= 0) {
                int brace = json.indexOf("{", carCfgPos);
                if (brace >= 0) {
                    // find matching closing brace
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

		// Find buildVersion
			int versionPos = json.indexOf("\"buildVersion\"");
			if (versionPos >= 0) {
				int colon = json.indexOf(":", versionPos);
				int q1 = json.indexOf("\"", colon + 1);
				int q2 = json.indexOf("\"", q1 + 1);
				if (q1 > 0 && q2 > q1) {
					String v = json.substring(q1 + 1, q2);
					if (v != null && v.length() > 0) {
						buildVersion = v;
					}
				}
			}

        } catch (Exception e) {
            logCluster("CONFIG: Error parsing config: " + e.toString() + ", using defaults");
        }
    }

    /**
     * Helper method to parse integer values from JSON config.
     * Returns defaultValue if key not found or value out of range.
     */
    private int parseIntValue(String json, String key, int searchStart, int defaultValue, int minValue, int maxValue) {
        try {
            int keyPos = json.indexOf("\"" + key + "\"", searchStart);
            if (keyPos >= 0 && keyPos < searchStart + 2000) {  // Search within reasonable range
                int colon = json.indexOf(":", keyPos);
                if (colon > 0) {
                    String valueSection = json.substring(colon + 1, Math.min(colon + 20, json.length()));
                    int start = 0;
                    while (start < valueSection.length() && !Character.isDigit(valueSection.charAt(start))) {
                        start++;
                    }
                    int end = start;
                    while (end < valueSection.length() && Character.isDigit(valueSection.charAt(end))) {
                        end++;
                    }
                    if (start < end) {
                        String numStr = valueSection.substring(start, end);
                        int value = Integer.parseInt(numStr);
                        if (value >= minValue && value <= maxValue) {
                            return value;
                        }
                    }
                }
            }
        } catch (Exception e) {
            // Keep default if parsing fails
        }
        return defaultValue;
    }

    /**
     * Checks for external config file on USB/SD card and loads it if available.
     * Searches for cluster_config.json on removable media.
     */
    private String loadExternalConfigIfExists() {
        if (externalConfigChecked) {
            return externalConfigContent;
        }
        
        externalConfigChecked = true;
        
        try {
            // Try twice with small delay - first attempt might fail during early startup
            for (int attempt = 0; attempt < 2; attempt++) {
                for (int i = 0; i < EXTERNAL_MOUNT_POINTS.length; i++) {
                    String path = EXTERNAL_MOUNT_POINTS[i] + CLUSTER_CONFIG_FILE;
                    File configFile = new File(path);
                    
                    if (configFile.exists() && configFile.canRead()) {
                        try {
                            externalConfigContent = readFileContent(configFile);
                            if (externalConfigContent != null && externalConfigContent.length() > 0) {
                                logCluster("CONFIG: Loaded external config from " + path);
                                return externalConfigContent;
                            }
                        } catch (Exception e) {
                            // Continue to next location
                        }
                    }
                }
                
                // If first attempt failed and we have more attempts, wait briefly
                if (attempt == 0) {
                    try {
                        Thread.sleep(100);
                    } catch (InterruptedException e) {
                        break;
                    }
                }
            }
            return null;
            
        } catch (Exception e) {
            logCluster("CONFIG: Error checking for external config: " + e.toString());
            return null;
        }
    }
    
    /**
     * Attempts to find the best available writable mount point for log file writing.
     * Tries all external storage locations in order, then falls back to configured path.
     */
    private String findBestLogPath(String logFileName, String configuredPath) {
        // Return cached path if available and key matches
        if (cachedLogPath != null && cachedLogPathKey != null && cachedLogPathKey.equals(configuredPath)) {
            return cachedLogPath;
        }

        // Check if external logging is enabled in config
        if (!enableExternalLogging) {
            return configuredPath;  // Use configured path when external logging disabled
        }

        // Try external storage locations first
        for (int i = 0; i < EXTERNAL_MOUNT_POINTS.length; i++) {
            String mountPoint = EXTERNAL_MOUNT_POINTS[i];
            String testPath = mountPoint + "/" + logFileName;

            // Check if mount point exists
            File mountDir = new File(mountPoint);
            if (!mountDir.exists()) {
                continue;
            }

            // Try to remount as writable if StorageMountHandler available
            if (storageMountHandler != null) {
                try {
                    storageMountHandler.mountStorageRW(mountPoint);
                } catch (Exception e) {
                    continue;
                }
            }

            // Test writability
            try {
                File testFile = new File(testPath);
                if (testFile.exists()) {
                    if (testFile.canWrite()) {
                        // Only log when state changes
                        if (lastExternalLogPath == null || !lastExternalLogPath.equals(mountPoint)) {
                            logDirect("LOG_PATH: External storage found at " + mountPoint);
                            lastExternalLogPath = mountPoint;
                        }
                        // Cache the discovered path
                        cachedLogPath = testPath;
                        cachedLogPathKey = configuredPath;
                        return testPath;
                    }
                } else {
                    FileWriter writer = new FileWriter(testFile);
                    writer.write("test");
                    writer.close();
                    testFile.delete();
                    // Only log when state changes
                    if (lastExternalLogPath == null || !lastExternalLogPath.equals(mountPoint)) {
                        logDirect("LOG_PATH: External storage found at " + mountPoint);
                        lastExternalLogPath = mountPoint;
                    }
                    // Cache the discovered path
                    cachedLogPath = testPath;
                    cachedLogPathKey = configuredPath;
                    return testPath;
                }
            } catch (Exception e) {
                continue;
            }
        }

        // Fall back to configured path - only log when state changes
        if (lastExternalLogPath != null) {
            logDirect("LOG_PATH: No external storage available, using " + configuredPath);
            lastExternalLogPath = null;
        }
        // Cache the fallback path
        cachedLogPath = configuredPath;
        cachedLogPathKey = configuredPath;
        return configuredPath;
    }

    /**
     * Invalidates the cached log path, forcing re-discovery on next write.
     * Called when a write operation fails, indicating storage may have been removed.
     */
    private void invalidateLogPathCache() {
        cachedLogPath = null;
        cachedLogPathKey = null;
        lastExternalLogPath = null;  // Also reset state tracking for clean re-discovery
    }

    /**
     * Reads the full content of a file into a string
     */
    private String readFileContent(File file) {
        if (file == null || !file.exists()) {
            return null;
        }
        
        FileInputStream fis = null;
        InputStreamReader isr = null;
        BufferedReader reader = null;
        
        try {
            fis = new FileInputStream(file);
            isr = new InputStreamReader(fis, "UTF-8");
            reader = new BufferedReader(isr);
            
            StringBuffer content = new StringBuffer();
            String line;
            while ((line = reader.readLine()) != null) {
                content.append(line);
                content.append("\n");
            }
            
            return content.toString();
        } catch (Exception e) {
            logCluster("CONFIG: Error reading file " + file.getPath() + ": " + e.toString());
            return null;
        } finally {
            try {
                if (reader != null) reader.close();
                if (isr != null) isr.close();
                if (fis != null) fis.close();
            } catch (Exception e) {
                // Ignore close errors
            }
        }
    }

    /**
     * Starts BAP heartbeat to continuously override native nav messages.
     * Resends RGStatus=1, maneuver, and distance at configured interval.
     */
    private void startHeartbeat() {
        // Check if heartbeat is enabled in config
        if (!heartbeatEnabled) {
            logCluster("BAP_HEARTBEAT: Disabled by config (enableHeartbeat=false)");
            return;
        }

        // Stop any existing heartbeat first
        stopHeartbeat();

        logCluster("BAP_HEARTBEAT: Starting (resend every " + heartbeatInterval + "ms to override native nav)");

        heartbeatTimer = new Timer(true); // Daemon thread (Java 1.4 compatible)
        heartbeatTimer.schedule(new TimerTask() {
            public void run() {
                try {
                    if (clusterService == null || !navigationActive) {
                        return;
                    }

                    // Only send RGStatus=1 if we have real navigation data (maneuver or distance)
                    boolean hasNavigationData = (lastManeuverArray != null && lastRoadName != null) || (lastDisplayValue >= 0 && lastUnit >= 0);
                    if (hasNavigationData) {
                        clusterService.updateRGStatus(1);
                    }

                    // Resend last maneuver if available
                    if (lastManeuverArray != null && lastRoadName != null) {
                        clusterService.updateManeuverDescriptor(lastManeuverArray);
                        clusterService.updateTurnToInfo(lastRoadName, lastSignPost != null ? lastSignPost : "");
                    }

                    // Resend last distance if available
                    if (lastDisplayValue >= 0 && lastUnit >= 0) {
                        clusterService.updateDistanceToNextManeuver(lastDisplayValue, lastUnit, lastBargraphEnabled, lastBargraph);
                    }

                    // Resend destination distance/time (estimated from next-turn data)
                    clusterService.updateTimeToDestination(0, 0, lastTimeToDestination);
                    clusterService.updateDistanceToDestination(lastDistanceToDestination, DistanceUnit.METER, false);

                    // Don't spam logs with heartbeat details - only log when we have real data to resend
                    // logCluster("BAP_HEARTBEAT: Resent (time=" + lastTimeToDestination + "s dist=" + lastDistanceToDestination + "m)");
                } catch (Exception e) {
                    logCluster("BAP_HEARTBEAT: Error resending data: " + e.toString());
                }
            }
        }, heartbeatInterval, heartbeatInterval); // Start after interval, repeat every interval
    }

    /**
     * Stops BAP heartbeat timer.
     */
    private void stopHeartbeat() {
        if (heartbeatTimer != null) {
            logCluster("BAP_HEARTBEAT: Stopping");
            heartbeatTimer.cancel();
            heartbeatTimer = null;
        }
    }

    public void navFocusRequestNotification(NavigationFocusType navFocusType, int validFlag) {
        boolean isProjected = navFocusType.equals(NavigationFocusType.PROJECTED);
        this.navigationActive = isProjected;

        // Log Android Auto DSI input
        logCluster("DSI_IN: NAV_FOCUS | type=" + navFocusType.toString() + " active=" + isProjected + " valid=" + validFlag);

        if (isProjected) {
            cancelPendingClear();  // Cancel any pending delayed clear from previous arrival
            lastManeuverKey = null;
            maneuverInitialDistance = 0;
            // Tell cluster we're using Type 0 (BAP maneuver descriptors)
            // This enables arrow-based navigation display mode
            if (clusterService != null) {
                try {
                    clusterService.updateActiveRGType(0);
                    logCluster("BAP_RGTYPE: Set to Type 0 (BAP maneuver descriptors)");
                } catch (Exception e) {
                    logCluster("ERROR setting RGType/RGStatus: " + e.toString());
                }
            }
            // Don't initialize destination data - wait for real data from Android Auto
            // Initializing with 0/0 causes heartbeat to resend it, triggering "destination reached"
            this.lastTimeToDestination = 0;
            this.lastDistanceToDestination = 0;
            logCluster("BAP_DESTINATION: Waiting for real data (not initializing with 0/0)");
            // Show welcome message now that cluster is in nav mode
            initClusterDefaults();
            // Start heartbeat to continuously override native nav
            startHeartbeat();
        } else {
            // Stop heartbeat when switching to native nav
            stopHeartbeat();
            clearCluster();
            destinationEventSeen = false;  // Reset for next navigation session
        }
    }

    public void updateNavigationNextTurnEvent(String roadName, int turnSide, int eventCode,
                                             int angle, int num, int validFlag) {
        // Log Android Auto DSI input with analysis
        String description = describeTurnEvent(eventCode, turnSide, angle);
        logCluster("DSI_IN: TURN_EVENT | road=\"" + (roadName != null ? roadName : "") +
                  "\" | side=" + turnSide + " event=" + eventCode + " angle=" + angle +
                  " num=" + num + " valid=" + validFlag + " | " + description);

        // Check validFlag - accept real data (validFlag=1) and arrival confirmation (validFlag=2, event=0)
        if (validFlag == 2 && eventCode == 0) {
            // event=0 valid=2 can be startup OR arrival OR route cancel
            if (!destinationEventSeen) {
                logCluster("STARTUP: Ignoring event=0 valid=2 (no event=19 seen yet)");
                return;
            }
            // Arrival or cancel: show destination symbol briefly, non-blocking timer clears it
            logCluster("ARRIVAL: Navigation ended (event=0 valid=2), showing destination");

            if (clusterService == null) {
                return;
            }

            try {
                // Show destination symbol on cluster
                CombiBAPNaviManeuverDescriptor destManeuver = new CombiBAPNaviManeuverDescriptor(
                    MAIN_ELEMENT_DESTINATION,
                    0,  // No direction for destination
                    0,
                    new byte[0]
                );
                CombiBAPNaviManeuverDescriptor[] maneuverArray = new CombiBAPNaviManeuverDescriptor[] { destManeuver };

                clusterService.updateRGStatus(1);
                logCluster("BAP_RGSTATUS: Set to 1 (Route Guidance Active)");
                clusterService.updateManeuverDescriptor(maneuverArray);
                logCluster("BAP_MANEUVER: DESTINATION (arrival confirmation)");

                // Zero out distance/time
                lastDistanceToDestination = 0;
                lastTimeToDestination = 0;
                clusterService.updateTimeToDestination(0, 0, 0);
                clusterService.updateDistanceToDestination(0, DistanceUnit.METER, false);
                logCluster("BAP_DESTINATION: time=0s, distance=0m (arrived)");

                // Mark destination shown time for delayed clear
                destinationShownTime = System.currentTimeMillis();

                // Cache for heartbeat (will continue until NAV_FOCUS changes)
                lastManeuverArray = maneuverArray;
                lastRoadName = "";
                lastSignPost = "";

            } catch (Exception e) {
                logCluster("ERROR sending arrival confirmation: " + e.toString());
            }
            return;
        } else if (validFlag != 1) {
            // Reject other non-real data
            logCluster("TURN_EVENT: Skipping non-real data (validFlag=" + validFlag + ")");
            // Don't clear cluster - just skip this update
            return;
        }


        try {
            CombiBAPNaviManeuverDescriptor maneuver = convertToBAPManeuver(eventCode, turnSide, angle, num);

            if (maneuver != null) {
                String mainElementName = getMainElementName(maneuver.getMainElement());
                String directionName = getDirectionName(maneuver.getDirection());

                logCluster("BAP_MANEUVER: " + mainElementName + " | " + directionName +
                            " | mainElement=" + maneuver.getMainElement() +
                            " direction=" + maneuver.getDirection() +
                            " | road=\"" + (roadName != null ? roadName : "") + "\"");

                if (eventCode >= 11 && eventCode <= 13) {
                    logCluster("ROUNDABOUT_DEBUG: side=" + turnSide + " angle=" + angle + " num=" + num);
                }

                if (clusterService != null && navigationActive) {
                    try {
                        CombiBAPNaviManeuverDescriptor[] maneuverArray = new CombiBAPNaviManeuverDescriptor[1];
                        maneuverArray[0] = maneuver;

                        clusterService.updateRGStatus(1);
                        logCluster("BAP_RGSTATUS: Set to 1 (Route Guidance Active)");

                        clusterService.updateManeuverDescriptor(maneuverArray);

                        // Build SignPost text for roundabouts (events 11-13)
                        String signPostText = "";
                        if ((eventCode >= 11 && eventCode <= 13) && num > 0) {
                            signPostText = formatExitNumber(num);
                            logCluster("BAP_ROUNDABOUT: Exit number = " + num + " -> SignPost = \"" + signPostText + "\"");
                        }

                        String normalizedRoad = normalizeRoadName(roadName != null ? roadName : "");
                        clusterService.updateTurnToInfo(normalizedRoad, signPostText);

                        // Send destination distance/time ONLY if we have valid cached data (>10m)
                        // Prevents sending 0/0 when turn event arrives before distance data at nav start
                        if (lastDistanceToDestination > 10) {
                            clusterService.updateTimeToDestination(0, 0, lastTimeToDestination);
                            clusterService.updateDistanceToDestination(lastDistanceToDestination, DistanceUnit.METER, false);
                            logCluster("BAP_DESTINATION: time=" + lastTimeToDestination + "s, distance=" + lastDistanceToDestination + "m (from next turn)");
                        } else {
                            logCluster("BAP_DESTINATION: Skipped (cached distance too small: " + lastDistanceToDestination + "m, waiting for real data)");
                        }

                        // Cache for heartbeat
                        this.lastManeuverArray = maneuverArray;
                        this.lastRoadName = normalizedRoad;
                        this.lastSignPost = signPostText;
                        // Success - no log (reduces spam)
                    } catch (Exception e) {
                        logCluster("  -> ERROR sending maneuver to cluster: " + e.toString());
                    }
                } else {
                    logCluster("  -> NOT sent (clusterService=" + (clusterService != null ? "OK" : "null") +
                                " navActive=" + navigationActive + ")");
                }
            }

            this.lastEventCode = eventCode;
            this.lastTurnSide = turnSide;
        } catch (Exception e) {
            logCluster("ERROR in updateTurnEvent: " + e.toString());
        }
    }

    public void updateNavigationNextTurnDistance(int distance, int timeToTurn, int validFlag) {
        // Log Android Auto DSI input with proximity analysis
        String proximity = getProximityDescription(distance);
        logCluster("DSI_IN: DISTANCE | distance=" + distance + "m time=" + timeToTurn + "s valid=" + validFlag + " | " + proximity);

        // Check validFlag - only accept real data (validFlag=1)
        // Reject both invalid (0) and test/placeholder (2) data
        if (validFlag != 1) {
            logCluster("DISTANCE: Skipping non-real data (validFlag=" + validFlag + ")");
            // Don't reset lastDistance - just skip this update
            return;
        }

        // Reject obviously stale or nonsense time values (AA sometimes sends very large values when stuck)
        // Allow -1 which means "time not available" (AA doesn't always provide time estimates)
        if (timeToTurn > 24 * 60 * 60) {  // Only reject if > 24 hours (clearly stale)
            logCluster("BAP_DISTANCE: ignoring stale timeToTurn=" + timeToTurn + "s (distance=" + distance + "m)");
            return;
        }

        // Dynamic rate limiting based on proximity: closer = more frequent updates
        int dynamicRateLimit = getDynamicRateLimit(distance);
        long currentTime = System.currentTimeMillis();
        boolean throttled = (currentTime - lastUpdateTime < dynamicRateLimit && lastUpdateTime > 0);

        if (throttled) {
            // In "always" mode, allow bargraph-only updates even when throttled
            // This keeps bargraph smooth while text updates are rate-limited
            if (!bargraphMode.equals("always") && !bargraphMode.equals("auto")) {
                return;  // Throttle "distance" and "dynamic" modes completely
            }
            // Continue for "always" mode bargraph-only update
        } else {
            this.lastUpdateTime = currentTime;
        }

        // Track maneuver changes for dynamic bargraph
        String currentKey = getManeuverKey(lastRoadName != null ? lastRoadName : "", lastEventCode, lastTurnSide);

        // Reset after turn completion takes priority
        if (resetManeuverOnNextDistance) {
            boolean preserveBaseline = PRESERVE_BASELINE_ENABLED
                    && roundaboutLatchingEnabled
                    && (cachedRoundaboutDirection != -1 || resetCameFromRoundabout)
                    && currentKey.equals(lastManeuverKey);
            resetCameFromRoundabout = false;
            if (preserveBaseline) {
                logCluster("BARGRAPH_RESET: Roundabout latch active, deferring reset (baseline = " + maneuverInitialDistance + "m)");
            } else {
                maneuverInitialDistance = distance;
                lastManeuverKey = currentKey;
                resetManeuverOnNextDistance = false;
                waitingForPostTurnTurnEvent = true;
                lastUpdateTime = 0;  // Force immediate display update - bypass rate limiter after maneuver reset
                lastDistance = -1;    // Force threshold met on next update - prevents stale cached display
                throttled = false;    // Override throttle for this event - reset must show fresh values
                logCluster("BARGRAPH_RESET: New maneuver after turn completion, initial distance = " + distance + "m");
            }
        } else if (waitingForPostTurnTurnEvent) {
            // TURN_EVENT arrives after first post-turn DISTANCE - adopt new key without resetting baseline
            if (!currentKey.equals(lastManeuverKey)) {
                lastManeuverKey = currentKey;
                waitingForPostTurnTurnEvent = false;
                if (distance > maneuverInitialDistance) {
                    // New maneuver is farther than the old baseline - update to avoid stuck-at-0% bargraph
                    maneuverInitialDistance = distance;
                    logCluster("BARGRAPH_KEY: Settled after TURN_EVENT, baseline updated to " + distance + "m");
                } else {
                    logCluster("BARGRAPH_KEY: Settled after TURN_EVENT (baseline stays " + maneuverInitialDistance + "m)");
                }
            }
            // else: TURN_EVENT not yet arrived, keep waiting
        } else if (lastManeuverKey == null || !currentKey.equals(lastManeuverKey)) {
            lastManeuverKey = currentKey;
            maneuverInitialDistance = distance;
            lastUpdateTime = 0;  // Force immediate display update - bypass rate limiter after maneuver reset
            logCluster("BARGRAPH_RESET: New maneuver detected, initial distance = " + distance + "m");
        }

        // Dynamic distance threshold based on proximity: closer = smaller threshold (more precision)
        // Threshold applies to TEXT distance only - bargraph always updates for smooth progress
        int dynamicDistThreshold = getDynamicDistanceThreshold(distance);
        boolean thresholdMet = (Math.abs(distance - lastDistance) >= dynamicDistThreshold) || (lastDistance == -1);

        // Cache distance/time for destination calls, but ONLY if distance > 10m
        // Prevents sending 0/0 which triggers "destination reached" at nav start and turn completions
        boolean cacheUpdated = false;
        if (thresholdMet && distance > 10) {
            this.lastDistanceToDestination = distance;  // raw distance in meters
            // Estimate time: assume 30mph average = 13.4 m/s -> ~13 m/s for simplicity
            int estimatedSeconds = distance / 13;
            this.lastTimeToDestination = estimatedSeconds;
            cacheUpdated = true;
        }
        // Don't update cached values when distance <= 10m (keep last valid values)

        try {
            int unit;
            int displayValue;
            String displayStr;
            boolean useMetric = shouldUseMetric();

            // Skip sending to cluster if distance is 0 (at nav start or turn completion)
            // Sending 0 might trigger "destination reached"
            // Set flag so next distance update resets bargraph baseline
            if (distance == 0) {
                resetManeuverOnNextDistance = true;
                resetCameFromRoundabout = (lastEventCode >= 11 && lastEventCode <= 13);
                waitingForPostTurnTurnEvent = false;  // Clear any stale settlement flag
                logCluster("BAP_DISTANCE: Skipped sending 0m (turn completion - will reset bargraph on next update)");
                return;
            }

            // Calculate new display values if threshold met AND not throttled
            // In "always" mode when throttled, skip text updates but continue for bargraph
            if (thresholdMet && !throttled) {
                // Threshold met and not throttled - calculate new display values
                if (useMetric) {
                if (distance < metricUnitThreshold) {
                    // Below threshold: show meters rounded to nearest 10
                    unit = CombiBAPConstantsNavi.DISTANCETONEXTMANEUVER_DISTANCETONEXTMANEUVER_UNIT_METER;
                    int roundedMeters = (distance / 10) * 10;  // Round down to nearest 10
                    displayValue = roundedMeters * 10;  // Cluster expects tenths format
                    displayStr = roundedMeters + "m";
                } else if (distance < 20000) {
                    // Above threshold to 19.9km: show in tenths of km
                    unit = CombiBAPConstantsNavi.DISTANCETONEXTMANEUVER_DISTANCETONEXTMANEUVER_UNIT_KILOMETER;
                    displayValue = (distance + 50) / 100;
                    displayStr = (displayValue / 10) + "." + (displayValue % 10) + "km";
                } else {
                    // >= 20km: skip display update (out of BAP protocol range)
                    if (distance > 10) {
                        logCluster("BAP_DISTANCE: >20km (rawDistance=" + distance + "m) - skipping display (cached for destination: time=" + lastTimeToDestination + "s, dist=" + lastDistanceToDestination + "m)");
                    } else {
                        logCluster("BAP_DISTANCE: >20km (rawDistance=" + distance + "m) - skipping display (distance too small to cache: " + distance + "m)");
                    }
                    return;
                }
            } else {
                if (distance < imperialUnitThreshold) {
                    // Resolve "auto" once on first use, then cache
                    if (resolvedImperialSmallUnit == null) {
                        if ("auto".equals(imperialSmallUnit)) {
                            resolvedImperialSmallUnit = resolveImperialSmallUnitFromConfig();
                            logCluster("IMPERIAL_SMALL_UNIT: auto resolved to " + resolvedImperialSmallUnit);
                        } else {
                            resolvedImperialSmallUnit = imperialSmallUnit;
                        }
                    }
                    if ("yards".equals(resolvedImperialSmallUnit)) {
                        // Show yards rounded to nearest 10
                        unit = CombiBAPConstantsNavi.DISTANCETONEXTMANEUVER_DISTANCETONEXTMANEUVER_UNIT_YARD;
                        int yards = (distance * 10936 + 5000) / 10000;  // 1m = 1.0936yd
                        int roundedYards = (yards / 10) * 10;  // Round down to nearest 10
                        displayValue = roundedYards * 10;  // Cluster expects tenths format
                        displayStr = roundedYards + "yd";
                    } else {
                        // Show feet rounded to nearest 5
                        unit = CombiBAPConstantsNavi.DISTANCETONEXTMANEUVER_DISTANCETONEXTMANEUVER_UNIT_FEET;
                        int feet = (distance * 3281 + 500) / 1000;  // 1m = 3.2808ft
                        int roundedFeet = (feet / 5) * 5;  // Round down to nearest 5
                        displayValue = roundedFeet * 10;  // Cluster expects tenths format
                        displayStr = roundedFeet + "ft";
                    }
                } else if (distance < 16093) {
                    // Above threshold to 9.9mi: show in tenths of miles
                    unit = CombiBAPConstantsNavi.DISTANCETONEXTMANEUVER_DISTANCETONEXTMANEUVER_UNIT_MILE_UK_AND_US_STATUTE_MILE;
                    displayValue = (distance * 10 + 805) / 1609;
                    displayStr = (displayValue / 10) + "." + (displayValue % 10) + "mi";
                } else {
                    // >= 10mi: show whole miles as tenths (10mi=100, etc.)
                    int miles = (distance + 805) / 1609;
                    // Note: Cluster may have display limits, but we send all distances
                    unit = CombiBAPConstantsNavi.DISTANCETONEXTMANEUVER_DISTANCETONEXTMANEUVER_UNIT_MILE_UK_AND_US_STATUTE_MILE;
                    displayValue = miles * 10;
                    displayStr = miles + "mi";
                }
                }

                // Update lastDistance when threshold is met
                this.lastDistance = distance;
            } else {
                // Threshold not met - use cached text values, but update bargraph for smooth progress
                if (lastDisplayValue < 0 || lastUnit < 0) {
                    // No cached values yet, skip this update
                    return;
                }
                displayValue = lastDisplayValue;
                unit = lastUnit;
                displayStr = "(cached)";
            }

            int displayDist = displayValue;

            // Determine bargraph display mode (for platforms that show either distance OR bargraph)
            boolean bargraphEnabled;
            int bargraph;
            if (bargraphMode.equals("distance")) {
                bargraphEnabled = false;  // Never show bargraph, always distance text
                bargraph = 0;  // Not used
            } else if (bargraphMode.equals("dynamic")) {
                // Switch from distance to bargraph at fixed distance
                // For short maneuvers (<2x threshold), use configured percentage fallback
                int switchThreshold;
                if (maneuverInitialDistance < dynamicBargraphDistance * 2) {
                    // Short maneuver: use configured percentage of initial distance
                    switchThreshold = (int)(maneuverInitialDistance * dynamicBargraphPercent / 100.0);
                } else {
                    // Normal maneuver: use configured fixed distance
                    switchThreshold = dynamicBargraphDistance;
                }
                bargraphEnabled = (distance <= switchThreshold);

                if (bargraphEnabled) {
                    // Bargraph fills from 0% (at switchThreshold) to 100% (at 0m)
                    if (switchThreshold > 0) {
                        bargraph = (int)((1.0 - ((double)distance / (double)switchThreshold)) * 100.0);
                        bargraph = bargraph < 0 ? 0 : (bargraph > 100 ? 100 : bargraph);
                    } else {
                        bargraph = 100;
                    }
                } else {
                    bargraph = 0;  // Not shown yet
                }
            } else {  // "always"
                bargraphEnabled = true;  // Always show bargraph (Cayenne shows both)
                bargraph = calculateBargraph(distance, lastEventCode);  // From initial distance
            }

            // Text rounds to 0 but maneuver not done yet - show full bargraph for consistency
            // Guards: distance >= threshold (large-unit territory only, not genuine 0ft/0m near the turn)
            //         bargraph > 0 (not a freshly-reset maneuver where displayValue is stale from the
            //                       previous 0ft frame - avoids 0ft/100% spike on new-maneuver start)
            int unitSwitchThreshold = useMetricCached ? metricUnitThreshold : imperialUnitThreshold;
            if (displayValue == 0 && distance > 0 && distance >= unitSwitchThreshold && bargraph > 0) {
                bargraph = 100;
            }

            // During roundabout traversal (latching active, CONTINUE events) show 0/0
            // Avoids confusing bargraph spikes and erratic distances inside the roundabout
            if (roundaboutLatchingEnabled && lastEventCode == 12) {
                displayValue = 0;
                bargraph = 0;
                displayStr = "0(roundabout)";
            }

            if (thresholdMet) {
                logCluster("BAP_DISTANCE: " + displayStr + " | bargraph=" + bargraph + "/100" +
                            " | unit=" + unit + " | bapValue=" + displayValue + " | rawDistance=" + distance + "m | bgEnabled=" + bargraphEnabled);
            } else {
                logCluster("BAP_DISTANCE: Smooth bargraph update | bargraph=" + bargraph + "/100 | text=" + displayValue + " (cached) | rawDistance=" + distance + "m | bgEnabled=" + bargraphEnabled);
            }

            if (clusterService != null && navigationActive) {
                try {
                    clusterService.updateDistanceToNextManeuver(displayValue, unit, bargraphEnabled, bargraph);
                    sendManeuverState(getManeuverStateForDistance(distance));

                    // Cache for heartbeat (next-maneuver display values)
                    // Only cache text values when threshold met (new values), always cache bargraph
                    if (thresholdMet) {
                        this.lastDisplayValue = displayValue;
                        this.lastUnit = unit;
                    }
                    this.lastBargraph = bargraph;
                    this.lastBargraphEnabled = bargraphEnabled;

                    // Success - no log (reduces spam)
                    // Only log destination cache when it was actually updated
                    if (cacheUpdated) {
                        logCluster("  -> Cached for destination: time=" + lastTimeToDestination + "s, dist=" + lastDistanceToDestination + "m");
                    }
                } catch (Exception e) {
                    logCluster("  -> ERROR sending distance to cluster: " + e.toString());
                }
            } else {
                logCluster("  -> NOT sent (clusterService=" + (clusterService != null ? "OK" : "null") +
                            " navActive=" + navigationActive + ")");
            }
        } catch (Exception e) {
            logCluster("ERROR in updateDistance: " + e.toString());
        }
    }

    private int angleDegToBapDirectionByte(int angleDeg) {
        // Normalize to [0,360)
        int a = angleDeg % 360;
        if (a < 0) a += 360;
        // Map degrees -> 0..255 (BAP uses byte where 0x00=0�, 0x40=90�, 0x80=180�, 0xC0=270�)
        return (a * 256) / 360;
    }

    private CombiBAPNaviManeuverDescriptor convertToBAPManeuver(int eventCode, int turnSide, int angle, int num) {
        int mainElement = MAIN_ELEMENT_FOLLOW_STREET;
        int direction = 0;  // Initialize to straight/unknown

        // Map Android Auto eventCode to specific BAP MainElement types
        // Use turnSide to select direction-specific types (left vs right)
        // Per BAP spec section 13.2.1, turnSide: 0=unknown, 1=left, 2=right
        switch (eventCode) {
            case 3:  // SLIGHT_TURN
            case 4:  // TURN
            case 5:  // SHARP_TURN
                mainElement = MAIN_ELEMENT_TURN;
                break;

            case 6:  // U_TURN
                mainElement = MAIN_ELEMENT_UTURN;
                break;

            case 7:  // ON_RAMP - use TurnOnMainroad with direction from turnSide
                mainElement = MAIN_ELEMENT_TURN_ON_MAINROAD;
                break;

            case 8:  // OFF_RAMP - use direction-specific Exit types
            case 10: // MERGE
                if (turnSide == 2) {
                    mainElement = MAIN_ELEMENT_EXIT_RIGHT;
                } else if (turnSide == 1) {
                    mainElement = MAIN_ELEMENT_EXIT_LEFT;
                } else {
                    // side=0 on highway = stay straight, not an exit
                    mainElement = MAIN_ELEMENT_FOLLOW_STREET;
                }
                break;

            case 9:  // FORK - use Fork-2 (default, most common)
                mainElement = MAIN_ELEMENT_FORK_2;
                break;

            case 14:  // STRAIGHT (proto: old=14, new=KEEP_LEFT/KEEP_RIGHT/STRAIGHT by turn_side)
                if (turnSide == 0 && angle == 0 && num == 1) {
                    // Waze: num=1 = stay on road / keep lane
                    mainElement = MAIN_ELEMENT_FOLLOW_STREET;
                } else if (turnSide == 2) {
                    mainElement = MAIN_ELEMENT_EXIT_RIGHT;
                } else if (turnSide == 1) {
                    mainElement = MAIN_ELEMENT_EXIT_LEFT;
                } else {
                    // side=0: Waze doesn't set turn_side for STRAIGHT events
                    // Use car's physical drive side: RHD exits left, LHD exits right
                    if (isDriveRightHandSide()) {
                        mainElement = MAIN_ELEMENT_EXIT_LEFT;
                    } else {
                        mainElement = MAIN_ELEMENT_EXIT_RIGHT;
                    }
                }
                break;

            case 11:  // ROUNDABOUT_ENTER_AND_EXIT_CW
            case 12:  // ROUNDABOUT_ENTER_AND_EXIT_CCW
            case 13:  // ROUNDABOUT_ENTER (exit direction in turnSide)
                // Use direction-specific roundabout types
                if (turnSide == 2) {
                    mainElement = MAIN_ELEMENT_ROUNDABOUT_TRS_RIGHT;
                } else if (turnSide == 1) {
                    mainElement = MAIN_ELEMENT_ROUNDABOUT_TRS_LEFT;
                } else {
                    // side=0, num=0 - real exit with unknown side, default to left
                    mainElement = MAIN_ELEMENT_EXIT_LEFT;
                }
                break;

            case 16:  // FERRY_BOAT
            case 17:  // FERRY_TRAIN
                mainElement = MAIN_ELEMENT_FERRY;
                break;

            // case 19 removed - now handled before switch statement to allow normal turn processing

            default:
                mainElement = MAIN_ELEMENT_FOLLOW_STREET;
                break;
        }

        // Handle event=19 (DESTINATION) - set flag but continue showing turn maneuver
        // Only show DESTINATION symbol when event=0 valid=2 arrives after event=19
        if (eventCode == 19) {
            destinationEventSeen = true;
            logCluster("DESTINATION_FLAG: event=19 seen, will continue showing turn maneuver until event=0");
            // Treat as FOLLOW_STREET (event=1) so normal turn processing continues
            eventCode = 1;
        }

        // Use actual angle for precise direction (360� resolution in BAP's 0-255 byte format)

        // Android Auto often sends angle=0 even for turns - use turnSide as fallback
        // This is critical because without it, all turns show straight arrows!
        if (angle == 0 && ((eventCode >= 3 && eventCode <= 7) || eventCode == 9)) {
            // For turn/ramp/fork events with missing angle, use turnSide
            // Convert to BAP byte format (0-255) per spec 
            // BAP: 0x00=straight(0), 0x40=left(90), 0x80=back(180), 0xC0=right(270)
            switch (turnSide) {
                case 1:  // LEFT
                    if (eventCode == 3) {
                        direction = 32;   // 45deg SLIGHT left
                    } else {
                        direction = 64;   // 90deg TURN/SHARP left (event=4,5,6)
                    }
                    break;
                case 2:  // RIGHT
                    if (eventCode == 3) {
                        direction = 224;  // 315deg SLIGHT right (same as roundabout angle that works!)
                    } else {
                        direction = 192;  // 270deg TURN/SHARP right (event=4,5,6)
                    }
                    break;
                default:  // STRAIGHT or unknown
                    direction = 0;
                    break;
            }
        } else if (angle == 0 && (eventCode >= 11 && eventCode <= 13)) {
            // For roundabout events with missing angle, use exit number to estimate
            // Use turnSide to determine roundabout direction: side=1 = RHD/CW, side=2 = LHD/CCW
            if (turnSide == 1) {
                // RHD: Clockwise roundabouts - exits on left, first exit = left turn
                if (num >= 3) {
                    direction = 192;  // 270 = RIGHT (exits 3+)
                    logCluster("ROUNDABOUT_EXIT_NUM: RHD exit " + num + " -> 270deg RIGHT");
                } else if (num == 2) {
                    direction = 0;    // 0 = STRAIGHT (exit 2)
                    logCluster("ROUNDABOUT_EXIT_NUM: RHD exit " + num + " -> 0deg STRAIGHT");
                } else if (num == 1) {
                    direction = 64;   // 90 = LEFT (exit 1)
                    logCluster("ROUNDABOUT_EXIT_NUM: RHD exit " + num + " -> 90deg LEFT");
                }
            } else {
                // LHD: Counter-clockwise roundabouts - exits on right, first exit = right turn
                if (num >= 3) {
                    direction = 64;   // 90 = LEFT (exits 3+)
                    logCluster("ROUNDABOUT_EXIT_NUM: LHD exit " + num + " -> 90deg LEFT");
                } else if (num == 2) {
                    direction = 0;    // 0 = STRAIGHT (exit 2)
                    logCluster("ROUNDABOUT_EXIT_NUM: LHD exit " + num + " -> 0deg STRAIGHT");
                } else if (num == 1) {
                    direction = 192;  // 270 = RIGHT (exit 1)
                    logCluster("ROUNDABOUT_EXIT_NUM: LHD exit " + num + " -> 270deg RIGHT");
                }
            }

            if (num == 0 && eventCode != 12) {
                // num=0 on event 11/13: Waze has no exit number info
                // Keep roundabout mainElement (TRS_LEFT/TRS_RIGHT from switch) with direction=straight
                direction = 0;
                logCluster("ROUNDABOUT_STRAIGHT: num=0 angle=0, keeping mainElement=" + mainElement + " direction=0 (straight)");
            }
        } else {
            // Use angle when available (preferred for precision)
            // For roundabouts: convert Google's coordinate system to BAP
            // RHD countries (UK, AU, JP): Clockwise roundabouts, use (540 - angle) % 360
            // LHD countries (US, DE, FR): Counter-clockwise roundabouts, use angle directly
            if ((eventCode >= 11 && eventCode <= 13) && angle > 0) {
                int convertedAngle;
                // Use turnSide to determine roundabout direction:
                // side=1 = RHD country (clockwise roundabout), side=2 = LHD country (counter-clockwise)
                // This is more reliable than car API detection (leftHandTraffic can be wrong)
                if (turnSide == 1) {
                    // RHD: Clockwise roundabouts (UK, Australia, Japan, India, etc.)
                    convertedAngle = (540 - angle) % 360;
                    logCluster("ROUNDABOUT_ANGLE_CONVERT: RHD Google " + angle + "deg -> BAP " + convertedAngle + "deg");
                } else {
                    // LHD: Counter-clockwise roundabouts (USA, Germany, France, etc.)
                    // Google measures from the entry point (180° in BAP terms); BAP measures from straight-ahead (0°).
                    // Apply +180° offset to shift the reference point, matching RHD which uses (540-angle)%360.
                    convertedAngle = (angle + 180) % 360;
                    logCluster("ROUNDABOUT_ANGLE_CONVERT: LHD Google " + angle + "deg -> BAP " + convertedAngle + "deg");
                }
                direction = angleDegToBapDirectionByte(convertedAngle);
            } else {
                direction = angleDegToBapDirectionByte(angle);
            }
        }


        // Roundabout direction latching: Keep original direction throughout roundabout
        // Reality: Android Auto sends EXIT (13) first, then CONTINUE (12), not ENTER→CONTINUE→EXIT
        // Can be disabled via JSON config: "enableRoundaboutLatching": false
        if (roundaboutLatchingEnabled) {
            if (eventCode == 11 || eventCode == 13) {
                // ROUNDABOUT_ENTER or ROUNDABOUT_EXIT: Always cache direction (even if cache exists)
                // This ensures each new roundabout gets fresh direction, not stale from previous
                cachedRoundaboutDirection = direction;
                logCluster("ROUNDABOUT_LATCH: Cached direction=" + direction + " for event=" + eventCode);
            } else if (eventCode == 12) {
                // ROUNDABOUT_CONTINUE: Use cached direction if available
                if (cachedRoundaboutDirection >= 0) {
                    logCluster("ROUNDABOUT_LATCH: Using cached direction=" + cachedRoundaboutDirection + " (calculated was " + direction + ")");
                    direction = cachedRoundaboutDirection;
                } else {
                    // First event is CONTINUE with angle - cache it
                    cachedRoundaboutDirection = direction;
                    logCluster("ROUNDABOUT_LATCH: Cached direction=" + direction + " from CONTINUE");
                }
            } else if (eventCode < 11 || eventCode > 13) {
                // Non-roundabout event: Clear cache
                if (cachedRoundaboutDirection >= 0) {
                    logCluster("ROUNDABOUT_LATCH: Clearing cache on non-roundabout event=" + eventCode);
                    cachedRoundaboutDirection = -1;
                }
            }
        }

        return new CombiBAPNaviManeuverDescriptor(mainElement, direction, 0, new byte[0]);
    }

    private String getMainElementName(int mainElement) {
        switch (mainElement) {
            case MAIN_ELEMENT_FOLLOW_STREET: return "FOLLOW_STREET";
            case MAIN_ELEMENT_TURN: return "TURN";
            case MAIN_ELEMENT_EXIT_RIGHT: return "EXIT_RIGHT";
            case MAIN_ELEMENT_EXIT_LEFT: return "EXIT_LEFT";
            case MAIN_ELEMENT_ROUNDABOUT_TRS_RIGHT: return "ROUNDABOUT_RIGHT";
            case MAIN_ELEMENT_ROUNDABOUT_TRS_LEFT: return "ROUNDABOUT_LEFT";
            case MAIN_ELEMENT_UTURN: return "UTURN";
            case MAIN_ELEMENT_EXIT_ROUNDABOUT_TRS_RIGHT: return "EXIT_ROUNDABOUT_RIGHT";
            case MAIN_ELEMENT_EXIT_ROUNDABOUT_TRS_LEFT: return "EXIT_ROUNDABOUT_LEFT";
            case MAIN_ELEMENT_PREPARE_ROUNDABOUT: return "PREPARE_ROUNDABOUT";
            case MAIN_ELEMENT_MAINROAD_RIGHT: return "MAINROAD_RIGHT";
            case MAIN_ELEMENT_MAINROAD_LEFT: return "MAINROAD_LEFT";
            case MAIN_ELEMENT_FERRY: return "FERRY";
            case MAIN_ELEMENT_DESTINATION: return "DESTINATION";
            default: return "UNKNOWN(" + mainElement + ")";
        }
    }

    private String getDirectionName(int direction) {
        // Check if it's an old discrete direction constant (0-8)
        switch (direction) {
            case DIRECTION_STRAIGHT: return "STRAIGHT";
            case DIRECTION_LEFT: return "LEFT";
            case DIRECTION_RIGHT: return "RIGHT";
            case DIRECTION_SLIGHT_LEFT: return "SLIGHT_LEFT";
            case DIRECTION_SLIGHT_RIGHT: return "SLIGHT_RIGHT";
            case DIRECTION_SHARP_LEFT: return "SHARP_LEFT";
            case DIRECTION_SHARP_RIGHT: return "SHARP_RIGHT";
            case DIRECTION_UTURN_LEFT: return "UTURN_LEFT";
            case DIRECTION_UTURN_RIGHT: return "UTURN_RIGHT";
        }

        // Otherwise it's an angle-based direction byte (0-255)
        // Convert back to degrees for display
        int degrees = (direction * 360) / 256;
        return degrees + "deg";
    }

    /**
     * Get formatted timestamp string with both QNX time and actual car time (if available).
     * Format: "1970-01-01 00:01:28.802 [2024-11-15 14:32:45.123]" or just "1970-01-01 00:01:28.802"
     */
    private String getDualTimestamp() {
        try {
            SimpleDateFormat sdf = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS");
            String qnxTime = sdf.format(new Date());

            // Try to get actual car time (only check once to avoid spam)
            if (!carTimeChecked) {
                carTimeChecked = true;
                carTimeAvailable = tryGetCarTime() != null;
            }

            if (carTimeAvailable) {
                Long carTimeMs = tryGetCarTime();
                if (carTimeMs != null) {
                    String carTime = sdf.format(new Date(carTimeMs.longValue()));
                    return qnxTime + " [" + carTime + "]";
                }
            }

            return qnxTime;
        } catch (Exception e) {
            // Fallback if timestamp formatting fails
            return String.valueOf(System.currentTimeMillis());
        }
    }

    /**
     * Try to get actual car time from services.
     * Returns null if not available.
     */
    private Long tryGetCarTime() {
        if (sysServices == null) {
            carTimeDebugInfo = "sysServices is null";
            return null;
        }

        try {
            IClock clock = sysServices.clock();
            if (clock == null) {
                carTimeDebugInfo = "sysServices.clock() returned null";
                return null;
            }

            ReadOnlyProperty localTimeProp = clock.localTime();
            if (localTimeProp == null) {
                carTimeDebugInfo = "clock.localTime() returned null";
                return null;
            }

            Long carTime = (Long) localTimeProp.get();
            if (carTime == null) {
                carTimeDebugInfo = "localTime.get() returned null";
                return null;
            }

            carTimeDebugInfo = "AVAILABLE [" + carTime + "ms]";
            return carTime;
        } catch (Exception e) {
            carTimeDebugInfo = "Exception: " + e.toString();
            return null;
        }
    }

    private String formatExitNumber(int exitNum) {
        // Format roundabout exit number for SignPost display with directional arrow
        if (exitNum <= 0) {
            return "";
        }
		/*
        // Calculate direction based on exit number and drive side
        // Same logic as in convertToBAPManeuver for roundabouts
        int direction = 0;
        boolean currentIsRHD = isDriveRightHandSide();

        if (currentIsRHD) {
            // RHD: Clockwise roundabouts
            if (exitNum >= 3) {
                direction = 192;  // 270deg RIGHT
            } else if (exitNum == 2) {
                direction = 128;  // 180deg STRAIGHT
            } else if (exitNum == 1) {
                direction = 64;   // 90deg LEFT
            }
        } else {
            // LHD: Counter-clockwise roundabouts
            if (exitNum >= 3) {
                direction = 64;   // 90deg LEFT
            } else if (exitNum == 2) {
                direction = 128;  // 180deg STRAIGHT
            } else if (exitNum == 1) {
                direction = 192;  // 270deg RIGHT
            }
        }

        // Map direction to Unicode arrow
        String arrow;
        if (direction == 64) {
            arrow = "\u2190";  // ← LEFT
        } else if (direction == 128) {
            arrow = "\u2191";  // ↑ STRAIGHT
        } else if (direction == 192) {
            arrow = "\u2192";  // → RIGHT
        } else {
            arrow = "Exit";  // default (text)
        }
		
        return arrow + " " + String.valueOf(exitNum); */

		// Old implementation:
		// return "> " + String.valueOf(exitNum);

		// Use Unicode circled numbers for cleaner display
		switch (exitNum) {
			case 1: return "\u2460";  // ①
			case 2: return "\u2461";  // ②
			case 3: return "\u2462";  // ③
			case 4: return "\u2463";  // ④
			case 5: return "\u2464";  // ⑤
			case 6: return "\u2465";  // ⑥
			case 7: return "\u2466";  // ⑦
			case 8: return "\u2467";  // ⑧
			case 9: return "\u2468";  // ⑨
			case 10: return "\u2469"; // ⑩
			default: return String.valueOf(exitNum);  // Fallback for exits >10
		}
    }

    private String getManeuverKey(String road, int eventCode, int turnSide) {
        return road + "|" + eventCode + "|" + turnSide;
    }

    // Normalize road names for cluster display:
    // 1. Google Maps encodes abbreviated words (EAST, WEST, NORTH, SOUTH, TO, etc.) using Unicode
    //    small capital letters (e.g. U+1D00 LATIN LETTER SMALL CAPITAL A). Map to plain ASCII uppercase.
    // 2. Strip Miscellaneous Symbols and Dingbats (U+2600-U+27FF) — confirmed cluster font gap.
    //    e.g. U+2708 AIRPLANE (✈) appears in "A120 Stansted ✈" and renders as a square.
    //    Both SignPost and TurnToInfo share the same Unicode 3.0 spec; this block has no place
    //    in road names regardless of font support.
    private String normalizeRoadName(String road) {
        if (road == null || road.length() == 0) return road != null ? road : "";
        StringBuffer sb = new StringBuffer(road.length());
        for (int i = 0; i < road.length(); i++) {
            char c = road.charAt(i);
            switch (c) {
                case '\u1D00': sb.append('A'); break;
                case '\u0299': sb.append('B'); break;
                case '\u1D04': sb.append('C'); break;
                case '\u1D05': sb.append('D'); break;
                case '\u1D07': sb.append('E'); break;
                case '\uA730': sb.append('F'); break;
                case '\u0262': sb.append('G'); break;
                case '\u029C': sb.append('H'); break;
                case '\u026A': sb.append('I'); break;
                case '\u1D0A': sb.append('J'); break;
                case '\u1D0B': sb.append('K'); break;
                case '\u029F': sb.append('L'); break;
                case '\u1D0D': sb.append('M'); break;
                case '\u0274': sb.append('N'); break;
                case '\u1D0F': sb.append('O'); break;
                case '\u1D18': sb.append('P'); break;
                case '\u0280': sb.append('R'); break;
                case '\uA731': sb.append('S'); break;
                case '\u1D1B': sb.append('T'); break;
                case '\u1D1C': sb.append('U'); break;
                case '\u1D20': sb.append('V'); break;
                case '\u1D21': sb.append('W'); break;
                case '\u028F': sb.append('Y'); break;
                case '\u1D22': sb.append('Z'); break;
                default:
                    // Strip Miscellaneous Symbols (U+2600-U+26FF) and Dingbats (U+2700-U+27FF)
                    if (c < '\u2600' || c > '\u27FF') {
                        sb.append(c);
                    }
                    break;
            }
        }
        return sb.toString();
    }

    private int calculateBargraph(int distance, int eventCode) {
        int maxDistance = maneuverInitialDistance;
        if (maxDistance == 0) {
            maxDistance = 200;
            if (eventCode >= 7 && eventCode <= 10) {
                maxDistance = 500;
            } else if (eventCode >= 11 && eventCode <= 13) {
                maxDistance = 300;
            }
        }

        if (distance >= maxDistance) {
            return 0;
        }

        int bargraph = (int)((1.0 - ((double)distance / (double)maxDistance)) * 100.0);
        return bargraph < 0 ? 0 : (bargraph > 100 ? 100 : bargraph);
    }

    private void clearCluster() {
        if (clusterService == null) {
            return;
        }

        // If destination was just shown, delay the clear via a non-blocking timer
        // so the DSI thread is not blocked and can continue processing events
        if (destinationShownTime > 0) {
            long elapsed = System.currentTimeMillis() - destinationShownTime;
            destinationShownTime = 0;
            if (elapsed < destinationDisplayDuration) {
                long remainingDelay = destinationDisplayDuration - elapsed;
                logCluster("BAP_CLEAR: Scheduling delayed clear in " + remainingDelay + "ms to show destination");
                cancelPendingClear();
                pendingClearTimer = new java.util.Timer(true);
                pendingClearTimer.schedule(new java.util.TimerTask() {
                    public void run() {
                        doClearCluster();
                    }
                }, remainingDelay);
                return;
            }
        }

        doClearCluster();
    }

    private void cancelPendingClear() {
        if (pendingClearTimer != null) {
            logCluster("BAP_CLEAR: Cancelling pending delayed clear (navigation restarted)");
            pendingClearTimer.cancel();
            pendingClearTimer = null;
        }
    }

    private void doClearCluster() {
        if (clusterService == null) {
            return;
        }
        try {
            try {
                clusterService.updateRGStatus(0);
                lastManeuverStateBAP = -1;
                sendManeuverState(1);  // FOLLOW
            } catch (Exception e) {
                logCluster("ERROR clearing cluster: " + e.toString());
            }
            logCluster("BAP_CLEAR: Cleared cluster display (RGStatus=0)");
        } catch (Exception e) {
            logCluster("ERROR in clearCluster: " + e.toString());
        }
    }

    public void onAndroidAutoTerminated() {
        try {
            logCluster("SYS: ANDROID_AUTO_TERMINATED - Clearing cluster and resetting state");

            this.navigationActive = false;
            videoCurrentlyAvailable = false;
            clusterState = CL_IDLE;
            stopHeartbeat();  // Stop heartbeat timer
            clearCluster();

            if (enableMapRender) {
                logCluster("MIRROR: stopping (AA terminated)");
                sendMirrorCommand("stop");
                // wakeNativeNavCluster();  // disabled — does nothing useful in practice
            }
        } catch (Exception e) {
            logCluster("ERROR in onAndroidAutoTerminated: " + e.toString());
        }
    }

    /**
     * Tells the native nav HMI to suspend rendering on the cluster (displayable 33).
     * Should be called when our cluster_mirror takes over so wakeupMapViewer() later
     * has a matching suspend to resume from.
     */
    private void suspendNativeNavCluster() {
        if (kombiMapViewerControl != null) {
            try {
                kombiMapViewerControl.suspendMapViewer();
                logCluster("KOMBI_MAP: suspendMapViewer() sent");
            } catch (Exception e) {
                logCluster("KOMBI_MAP: suspendMapViewer() failed: " + e.toString());
            }
            try {
                kombiMapViewerControl.viewSetVisible(false);
                logCluster("KOMBI_MAP: viewSetVisible(false) sent");
            } catch (Exception e) {
                logCluster("KOMBI_MAP: viewSetVisible(false) failed: " + e.toString());
            }
        }
        // Also tell the cluster service to switch to hidden context (HMI lifecycle path)
        callMapClusterService("switchKombiMapToHiddenContext");
    }

    /**
     * Tells the native nav HMI to resume rendering on the cluster (displayable 33).
     * After cluster_mirror exits, displayable 33 is no longer registered with the
     * displaymanager because our window was the last one bound to it. The DSI
     * suspend/wake calls only flip render state — they don't trigger window
     * re-creation. The high-level IMapClusterService.switchKombiMapToAShownContext()
     * goes through the full HMI lifecycle (startActivity + enterMapScreen) which
     * does create a new screen window bound to displayable 33.
     */
    private void wakeNativeNavCluster() {
        if (kombiMapViewerControl != null) {
            try {
                kombiMapViewerControl.wakeupMapViewer();
                logCluster("KOMBI_MAP: wakeupMapViewer() sent");
            } catch (Exception e) {
                logCluster("KOMBI_MAP: wakeupMapViewer() failed: " + e.toString());
            }
            try {
                kombiMapViewerControl.viewSetVisible(true);
                logCluster("KOMBI_MAP: viewSetVisible(true) sent");
            } catch (Exception e) {
                logCluster("KOMBI_MAP: viewSetVisible(true) failed: " + e.toString());
            }
            try {
                kombiMapViewerControl.viewFreeze(false);
                logCluster("KOMBI_MAP: viewFreeze(false) sent");
            } catch (Exception e) {
                logCluster("KOMBI_MAP: viewFreeze(false) failed: " + e.toString());
            }
        }
        // Force a real state transition: hide → show. Just calling Show
        // when native nav thinks it's already shown is a no-op (startActivity
        // and enterMapScreen are guarded against repeat). Hiding first
        // forces it to teardown and rebuild on the subsequent show.
        callMapClusterService("switchKombiMapToHiddenContext");
        callMapClusterService("switchKombiMapToAShownContext");
    }

    /**
     * Calls a no-arg method on the IMapClusterService via reflection (the class
     * is in the navi app, not our compile-time classpath).
     */
    private void callMapClusterService(String methodName) {
        if (mapClusterService == null) {
            logCluster("KOMBI_MAP: " + methodName + " skipped - IMapClusterService not available");
            return;
        }
        try {
            java.lang.reflect.Method m = mapClusterService.getClass().getMethod(methodName, new Class[0]);
            m.invoke(mapClusterService, new Object[0]);
            logCluster("KOMBI_MAP: IMapClusterService." + methodName + "() invoked");
        } catch (NoSuchMethodException e) {
            logCluster("KOMBI_MAP: " + methodName + " not found on " + mapClusterService.getClass().getName());
        } catch (Exception e) {
            logCluster("KOMBI_MAP: " + methodName + " invoke failed: " + e.toString());
        }
    }

    public void asyncException(int errCode, String msg, int requestType) {
        logCluster("DSI_IN: ASYNC_EXCEPTION | errCode=" + errCode + " msg=\"" + msg + "\" requestType=" + requestType);
    }

    public void videoFocusRequestNotification(int focus, int validFlag) {
        logCluster("DSI_IN: VIDEO_FOCUS | focus=" + focus + " valid=" + validFlag);
    }

    public void videoAvailable(boolean available, int validFlag) {
        logCluster("DSI_IN: VIDEO_AVAILABLE | available=" + available + " valid=" + validFlag);

        videoCurrentlyAvailable = available;

        if (!enableMapRender) {
            logCluster("CLUSTER: map_rendering disabled in config, skipping");
            return;
        }

        boolean isH264 = "h264".equalsIgnoreCase(aaClusterMode);

        if (available) {
            if (isH264) {
                // h264 path. Warm child was prepared in onAndroidAutoActivated().
                // resume/pause keep the child alive across HMI cycles so the
                // initial IDR caught at session start remains valid. During
                // STAGE_PAUSED the h264 child itself paints the canim splash
                // — no separate canim process spawned, no disp-33 binding
                // handoff problem.
                switch (clusterState) {
                    case CL_PREPARED:
                        logCluster("CLUSTER h264: PREPARED → RENDERING (resume)");
                        sendMirrorCommand("resume");
                        clusterState = CL_RENDERING;
                        break;
                    case CL_PAUSED:
                        logCluster("CLUSTER h264: PAUSED → RENDERING (resume)");
                        sendMirrorCommand("resume");
                        clusterState = CL_RENDERING;
                        break;
                    case CL_RENDERING:
                        logCluster("CLUSTER h264: already RENDERING, ignoring videoAvailable(true)");
                        break;
                    default:
                        logCluster("CLUSTER h264: unexpected state=" + clusterState
                                   + " on videoAvailable(true); recovering with prepare+resume");
                        sendMirrorCommand("prepare capture=h264");
                        sendMirrorCommand("resume");
                        clusterState = CL_RENDERING;
                        break;
                }
            } else {
                // mirror: always start fresh. IDLE ↔ RENDERING via start/stop.
                String cmd = "start mode=" + mirrorMode
                           + " zoomX=" + mirrorZoomX + " zoomY=" + mirrorZoomY
                           + " panX=" + mirrorPanX + " panY=" + mirrorPanY;
                logCluster("CLUSTER mirror: start -- " + cmd);
                sendMirrorCommand(cmd);
                clusterState = CL_RENDERING;
            }
        } else {
            // videoAvailable(false)
            if (isH264) {
                // Pause warm h264 child. Daemon SIGUSR2s it; child enters
                // STAGE_PAUSED, paints canim itself. Same window, same
                // disp-33 binding — recovery on next resume is just a
                // VISIBLE flip plus stage flip, no binding handoff.
                if (clusterState == CL_RENDERING) {
                    logCluster("CLUSTER h264: RENDERING → PAUSED (pause)");
                    sendMirrorCommand("pause");
                    clusterState = CL_PAUSED;
                } else {
                    logCluster("CLUSTER h264: videoAvailable(false) in state=" + clusterState
                               + ", no command");
                }
            } else {
                // mirror: stop kills the child; daemon spawns canim splash
                // separately (mirror's process being dead means no binding
                // conflict with the canim_splash process).
                if (clusterState == CL_RENDERING) {
                    logCluster("CLUSTER mirror: RENDERING → IDLE (stop)");
                    sendMirrorCommand("stop");
                    clusterState = CL_IDLE;
                } else {
                    logCluster("CLUSTER mirror: videoAvailable(false) in state=" + clusterState
                               + ", no command");
                }
            }
        }
    }

    /**
     * Called once per phone-connect, before any DSI traffic. Pairs with
     * onAndroidAutoTerminated(). For h264 mode: spawn the cluster decoder
     * child early via `prepare` so it's warmed up before videoAvailable(true).
     * For mirror mode: no-op (mirror has no useful warm state — spawns at
     * videoAvailable(true)).
     */
    public void onAndroidAutoActivated() {
        logCluster("SYS: ANDROID_AUTO_ACTIVATED (aaClusterMode=" + aaClusterMode + ")");
        clusterState = CL_IDLE;
        if (!enableMapRender) {
            logCluster("CLUSTER: map_rendering disabled in config, skipping prepare");
            return;
        }
        if ("h264".equalsIgnoreCase(aaClusterMode)) {
            sendMirrorCommand("prepare capture=h264");
            clusterState = CL_PREPARED;
        }
        // mirror: stay CL_IDLE; videoAvailable(true) will start it.
    }

    private void sendMirrorCommand(final String cmd) {
        if (!new File(mirrorFifo).exists()) {
            logCluster("MIRROR: fifo not found, daemon not running? (" + mirrorFifo + ")");
            return;
        }
        try {
            /* APPEND mode so back-to-back commands (e.g. prepare → resume) don't
             * overwrite each other before the daemon polls. Daemon reads
             * line-by-line and processes each. */
            java.io.FileWriter fw = new java.io.FileWriter(mirrorFifo, true);
            fw.write(cmd + "\n");
            fw.flush();
            fw.close();
            logCluster("MIRROR: sent '" + cmd + "' to " + mirrorFifo);
        } catch (Exception e) {
            logCluster("MIRROR: failed to write to " + mirrorFifo + ": " + e.toString());
        }
    }

    public void audioFocusRequestNotification(int focus, int validFlag) {
        logCluster("DSI_IN: AUDIO_FOCUS | focus=" + focus + " valid=" + validFlag);
    }

    public void audioAvailable(int type, boolean available, int validFlag) {
        logCluster("DSI_IN: AUDIO_AVAILABLE | type=" + type + " available=" + available + " valid=" + validFlag);
    }

    public void voiceSessionNotification(int session, int validFlag) {
        logCluster("DSI_IN: VOICE_SESSION | session=" + session + " valid=" + validFlag);
    }

    public void microphoneRequestNotification(int request, int validFlag) {
        logCluster("DSI_IN: MICROPHONE_REQUEST | request=" + request + " valid=" + validFlag);
    }

    public void updateNowPlayingData(TrackData trackData, int validFlag) {
        logCluster("DSI_IN: NOW_PLAYING | trackData=" + (trackData != null ? trackData.toString() : "null") + " valid=" + validFlag);
    }

    public void updatePlaybackState(PlaybackInfo playbackInfo, int validFlag) {
        logCluster("DSI_IN: PLAYBACK_STATE | playbackInfo=" + (playbackInfo != null ? playbackInfo.toString() : "null") + " valid=" + validFlag);
    }

    public void updatePlayposition(int position, int validFlag) {
        // Disabled: too verbose, logs every second during media playback
        // logCluster("DSI_IN: PLAY_POSITION | position=" + position + "ms valid=" + validFlag);
    }

    public void updateCoverArtUrl(ResourceLocator url, int validFlag) {
        logCluster("DSI_IN: COVER_ART_URL | url=" + (url != null ? url.toString() : "null") + " valid=" + validFlag);
    }

    public void bluetoothPairingRequest(String deviceName, int validFlag) {
        logCluster("DSI_IN: BLUETOOTH_PAIRING | deviceName=\"" + (deviceName != null ? deviceName : "") + "\" valid=" + validFlag);
    }
    
    private void initClusterDefaults() {
        if (startupPopupShown || clusterService == null) {
            return;
        }

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

            // Schedule clear after timeout
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

        /* Old popup implementation - removed, cluster doesn't support it
        try {
            PartialPopupBAPContent popup = new PartialPopupBAPContent(2, 0, 
                "Welcome to Android Auto Cluster Integration! Visit our website for updates and support.");
            
            popup.setMaximumDisplayTime(5000); // 10 seconds
            popup.setIcon(1); // Information icon
            
            // Add interactive options
            popup.setSelectionOption1(1, 1, "AndroidAuto Cluster");
            popup.setSelectionOption2(2, 1, "(C)2026 fifthBro");
            popup.setSelectionOption3(3, 1, "https://tinyurl.com/androidauto-cluster");
            popup.setInitialCursorPosition(3); // Default to "Close"
            
            popupService.showPartialPopup(STARTUP_POPUP_ID, popup);
            startupPopupShown = true;
            
            logCluster("PU:STARTUP");
        } catch (Exception e) {
            logCluster("PU: ERROR: Failed");
        }
        */
    }

    // PartialPopupBAPServiceListener implementation (not used with CurrentPositionInfo approach)
    public void optionSelected(int popupId, int selectedOptionId) {
        if (popupId != STARTUP_POPUP_ID) return;
        
        logCluster("POPUP_ACTION: User selected option " + selectedOptionId + " from startup popup");
        
        switch (selectedOptionId) {
            case 1: // Visit Website
                logCluster("PU: ACTION 1");
                // Show URL popup
                try {
                    PartialPopupBAPContent urlPopup = new PartialPopupBAPContent(1, 
                        "Website: https://github.com/your-repo/android-auto-cluster");
                    urlPopup.setMaximumDisplayTime(8000);
                    urlPopup.setSelectionOption1(1, 1, "OK");
                    popupService.showPartialPopup(STARTUP_POPUP_ID + 1, urlPopup);
                } catch (Exception e) {
                    logCluster("PU: ERROR: Failed ACTION1");
                }
                break;
            case 2: // Documentation
                logCluster("PU: ACTION 2");
                try {
                    PartialPopupBAPContent docPopup = new PartialPopupBAPContent(1, 
                        "Documentation: Check README.md in the project repository");
                    docPopup.setMaximumDisplayTime(6000);
                    docPopup.setSelectionOption1(1, 1, "OK");
                    popupService.showPartialPopup(STARTUP_POPUP_ID + 2, docPopup);
                } catch (Exception e) {
                    logCluster("PU:ERROR: Failed to show ACTION2");
                }
                break;
            case 3: // Close
                logCluster("PU: ACTION 3");
                break;
        }
        
        // Hide the main popup
        if (popupService != null) {
            popupService.hidePartialPopup(STARTUP_POPUP_ID);
        }
    }
    
    public void cancelPopup(int popupId) {
        if (popupId >= STARTUP_POPUP_ID && popupId <= STARTUP_POPUP_ID + 10) {
            logCluster("PU: CANCELLED");
        }
    }
    
    public void notifyPartialPopupHidden() {
        logCluster("PU: HIDDEN");
    }
    
    public void notifyPartialPopupVisible() {
        logCluster("PU: VISIBLE");
    }

    public void updateStreamParameters(int w, int h, int m, int d, int f, int c, int v) {
        logCluster("DSI_IN: STREAM_PARAMS | width=" + w + " height=" + h + " m=" + m + " d=" + d + " f=" + f + " c=" + c + " v=" + v);
    }

    // Event analysis helpers (ported from AndroidAutoEventLogger)
    private String describeTurnEvent(int eventCode, int turnSide, int angle) {
        StringBuffer sb = new StringBuffer();

        // Describe turn side
        switch (turnSide) {
            case 0:
                sb.append("STRAIGHT");
                break;
            case 1:
                sb.append("LEFT");
                break;
            case 2:
                sb.append("RIGHT");
                break;
            default:
                sb.append("SIDE_").append(turnSide);
        }

        // Describe sharpness based on angle
        int absAngle = Math.abs(angle);
        if (absAngle > 120) {
            sb.append(" SHARP");
        } else if (absAngle < 30) {
            sb.append(" SLIGHT");
        }

        // Describe event type
        switch (eventCode) {
            case 1:
                sb.append(" TURN");
                break;
            case 2:
                sb.append(" KEEP");
                break;
            case 3:
                sb.append(" EXIT");
                break;
            case 4:
                sb.append(" MERGE");
                break;
            case 5:
                sb.append(" ROUNDABOUT");
                break;
            case 6:
                sb.append(" UTURN");
                break;
            case 7:
                sb.append(" ON_RAMP");
                break;
            case 8:
                sb.append(" DESTINATION");
                break;
            case 9:
                sb.append(" CONTINUE");
                break;
            default:
                sb.append(" EVENT_").append(eventCode);
        }

        return sb.toString();
    }

    private String getTurnSideName(int turnSide) {
        switch (turnSide) {
            case 0:
                return "straight";
            case 1:
                return "left";
            case 2:
                return "right";
            default:
                return "unknown_" + turnSide;
        }
    }

    private String getProximityDescription(int distance) {
        if (distance > 5000) {
            return "Very far (>5km)";
        }
        if (distance > 1000) {
            return "Far (>1km)";
        }
        if (distance > 500) {
            return "Approaching (<1km)";
        }
        if (distance > 200) {
            return "Near (<500m)";
        }
        if (distance > 100) {
            return "Close (<200m)";
        }
        if (distance > 50) {
            return "Very close (<100m)";
        }
        return "Now (<50m)";
    }

    /**
     * Returns dynamic distance threshold based on proximity.
     * Closer distances use smaller thresholds for more precision.
     * Values are configurable via JSON config.
     */
    private int getZone(int distance) {
        if (distance > veryFarBoundary)    return ZONE_VERY_FAR;
        if (distance > farBoundary)        return ZONE_FAR;
        if (distance > approachingBoundary) return ZONE_APPROACHING;
        if (distance > nearBoundary)       return ZONE_NEAR;
        if (distance > closeBoundary)      return ZONE_CLOSE;
        if (distance > veryCloseBoundary)  return ZONE_VERY_CLOSE;
        return ZONE_NOW;
    }

    private int getManeuverStateForDistance(int distance) {
        int zone = getZone(distance);
        int naturalState;
        if (zone <= ZONE_APPROACHING) naturalState = 1;  // FOLLOW  (>500m)
        else if (zone == ZONE_NEAR)   naturalState = 2;  // PREPARE (200-500m)
        else if (zone <= ZONE_VERY_CLOSE) naturalState = 3;  // DISTANCE (50-200m)
        else                          naturalState = 4;  // CALL_FOR_ACTION (<=50m)
        // Fall back to nearest lower enabled state (bit0=state1, bit1=state2, bit2=state3, bit3=state4)
        for (int s = naturalState; s >= 1; s--) {
            if ((maneuverStateMask & (1 << (s - 1))) != 0) return s;
        }
        return 1;  // safety fallback (shouldn't reach here if mask != 0)
    }

    private void sendManeuverState(int state) {
        if (maneuverStateMask == 0 || state == lastManeuverStateBAP || clusterService == null) return;
        try {
            clusterService.updateManeuverState(state);
            lastManeuverStateBAP = state;
            logCluster("MS:" + state);
        } catch (Exception e) {
            logCluster("MS:ERR " + e.toString());
        }
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
}
