/*
 * Copyright (c) 2026 fifthBro
 * https://fifthbro.github.io
 *
 * Licensed under CC BY-NC-SA 4.0
 * https://creativecommons.org/licenses/by-nc-sa/4.0/
 * NOT FOR COMMERCIAL USE
 */

/**
 * Terminal Mode subsystem integration for Android Auto v2.
 * 
 * Manages the lifecycle and state transitions for Android Auto connectivity
 * within Audi's Terminal Mode architecture. Responsibilities include:
 * - DSI (Device System Interface) state management and event handling
 * - Android Auto activation/deactivation coordination
 * - Cluster integration service initialization and teardown
 * - Navigation service discovery and binding via OSGi
 * - State machine integration for mode changes and resource management
 * 
 * Integrates with AndroidAutoClusterIntegration for real-time navigation
 * data processing and cluster display updates.
 */
package de.audi.app.terminalmode.smartphone.androidauto2;

import de.audi.app.terminalmode.IContext;
import de.audi.app.terminalmode.ITMDeviceSubsystem;
import de.audi.app.terminalmode.ITerminalModeComponent;
import de.audi.app.terminalmode.SmartphoneManager;
import de.audi.app.terminalmode.dsi.IDSIControllerStateListener;
import de.audi.app.terminalmode.dsi.androidauto2.DSIAndroidAuto2DSIController;
import de.audi.app.terminalmode.dsi.androidauto2.DSIAndroidAuto2ListenerSafe;
import de.audi.app.terminalmode.dsi.androidauto2.DSIAndroidAuto2Safe;
import de.audi.app.terminalmode.events.IEventBus;
import de.audi.app.terminalmode.keyevents.ITerminalModeDSIKeyEventsController;
import de.audi.app.terminalmode.smartphone.androidauto2.AndroidAuto2DSIManager;
import de.audi.app.terminalmode.smartphone.androidauto2.AndroidAuto2EventListener;
import de.audi.app.terminalmode.smartphone.androidauto2.AndroidAuto2ListenerDistributor;
import de.audi.app.terminalmode.smartphone.androidauto2.AndroidAuto2RequestHandler;
import de.audi.app.terminalmode.smartphone.androidauto2.AndroidAutoSmartphoneProperties;
import de.audi.app.terminalmode.smartphone.androidauto2.audio.AndroidAuto2AudioHandler;
import de.audi.app.terminalmode.smartphone.androidauto2.mic.AndroidAuto2MicHandler;
import de.audi.app.terminalmode.smartphone.androidauto2.nav.AndroidAuto2NavHandler;
import de.audi.app.terminalmode.smartphone.androidauto2.nav.AndroidAutoClusterIntegration;
import de.audi.app.terminalmode.smartphone.androidauto2.video.AndroidAuto2VideoHandler;
import de.audi.atip.interapp.combi.bap.navi.CombiBAPServiceNavi;
import de.audi.atip.interapp.combi.bap.audio.CombiBAPServiceAudio;
import de.audi.app.car.api.services.ICarStatisticsService;
import de.audi.mib.system.ISysServices;
import de.audi.app.car.api.core.ICarCoreServices;
import de.audi.app.car.api.services.ICarExteriorLightService;
import org.dsi.ifc.map.DSIMapViewerControl;
import de.audi.app.car.adi.legacy.sportchrono.StorageMountHandler;
import org.osgi.framework.ServiceReference;
import org.osgi.framework.BundleContext;
import de.audi.app.terminalmode.smartphone.androidauto2.voice.AndroidAuto2VoiceSessionHandler;
import de.audi.app.terminalmode.statemachine.Application;
import de.audi.app.terminalmode.statemachine.ApplicationOwner;
import de.audi.app.terminalmode.statemachine.ApplicationStateChange;
import de.audi.app.terminalmode.statemachine.IRequestor;
import de.audi.app.terminalmode.statemachine.IStateChangeAction;
import de.audi.app.terminalmode.statemachine.IStateHandler;
import de.audi.app.terminalmode.statemachine.ITMState;
import de.audi.app.terminalmode.statemachine.Resource;
import de.audi.app.terminalmode.statemachine.ResourceOwner;
import de.audi.app.terminalmode.statemachine.ResourceOwnerChange;
import de.audi.app.terminalmode.statemachine.ResourceState;
import de.audi.app.terminalmode.statemachine.ResourceStateChange;
import de.audi.app.terminalmode.statemachine.commands.RequestModeChange;
import de.audi.atip.log.LogChannel;
import de.audi.atip.utils.ReflectionUtils;
import de.audi.atip.utils.commandlist2.CommandList;
import de.audi.atip.utils.commandlist2.ICommand;
import de.audi.atip.utils.generics.GMap;
import de.audi.atip.utils.generics.Generics;

public class AndroidAuto2Subsystem
implements ITMDeviceSubsystem,
ITerminalModeComponent,
IDSIControllerStateListener {
    private static final String LOGCLASS = ReflectionUtils.getSimpleName((Class)AndroidAuto2Subsystem.class);
    private final ITerminalModeDSIKeyEventsController keyEventController;
    private final DSIAndroidAuto2DSIController dsiController;
    private final LogChannel logger;
    private final IContext context;
    private final AndroidAuto2DSIManager androidAuto2DSIManager;
    private final SmartphoneManager smartphoneManagerListener;
    private volatile boolean active;
    private volatile boolean dsiAvailable;
    private final IStateHandler stateHandler;
    private final DSIAndroidAuto2Safe dsiAndroidAuto2;
    private final AndroidAutoSmartphoneProperties androidAutoSmartphoneProperties;
    private final IEventBus eventBus;
    private final AndroidAutoClusterIntegration clusterIntegration;

    public AndroidAuto2Subsystem(ITerminalModeDSIKeyEventsController iTerminalModeDSIKeyEventsController, DSIAndroidAuto2DSIController dSIAndroidAuto2DSIController, LogChannel logChannel, IContext iContext, SmartphoneManager smartphoneManager, AndroidAuto2ListenerDistributor androidAuto2ListenerDistributor, AndroidAuto2RequestHandler androidAuto2RequestHandler, AndroidAuto2EventListener androidAuto2EventListener, IStateHandler iStateHandler, AndroidAuto2AudioHandler androidAuto2AudioHandler, AndroidAuto2MicHandler androidAuto2MicHandler, AndroidAuto2NavHandler androidAuto2NavHandler, AndroidAuto2VideoHandler androidAuto2VideoHandler, AndroidAuto2VoiceSessionHandler androidAuto2VoiceSessionHandler, AndroidAuto2DSIManager androidAuto2DSIManager, IEventBus iEventBus, DSIAndroidAuto2Safe dSIAndroidAuto2Safe, AndroidAutoSmartphoneProperties androidAutoSmartphoneProperties) {
        this.keyEventController = iTerminalModeDSIKeyEventsController;
        this.dsiController = dSIAndroidAuto2DSIController;
        this.logger = logChannel;
        this.context = iContext;
        this.smartphoneManagerListener = smartphoneManager;
        this.eventBus = iEventBus;
        this.dsiAndroidAuto2 = dSIAndroidAuto2Safe;
        this.androidAuto2DSIManager = androidAuto2DSIManager;
        this.androidAutoSmartphoneProperties = androidAutoSmartphoneProperties;

        // Initialize StorageMountHandler FIRST (before AndroidAutoClusterIntegration)
        // so it's available when constructor starts logging
        StorageMountHandler storageMountHandler = null;
        try {
            // Step 1: Check if required ASI services are available
            logChannel.log(1000000, "[%1.<init>] Checking ASI service availability for StorageMountHandler...", (Object)LOGCLASS);

            ServiceReference[] asiAdminRefs = null;
            ServiceReference[] vCardExportRefs = null;

            // Load ASI classes dynamically to avoid compile-time dependency
            try {
                Class asiAdminClass = Class.forName("de.esolutions.fw.asi.ASIServiceAdmin");
                asiAdminRefs = iContext.getServiceManager().getServiceReferences(asiAdminClass);
            } catch (ClassNotFoundException cnfe) {
                logChannel.log(100000, "[%1.<init>] ASIServiceAdmin class not found", (Object)LOGCLASS);
            } catch (Exception e) {
                logChannel.log(100000, "[%1.<init>] Cannot query ASIServiceAdmin: %2", (Object)LOGCLASS, (Object)e.toString());
            }

            try {
                Class vCardExportClass = Class.forName("de.esolutions.fw.comm.asi.honcho.organizer.VCardExport");
                vCardExportRefs = iContext.getServiceManager().getServiceReferences(vCardExportClass);
            } catch (ClassNotFoundException cnfe) {
                logChannel.log(100000, "[%1.<init>] VCardExport class not found", (Object)LOGCLASS);
            } catch (Exception e) {
                logChannel.log(100000, "[%1.<init>] Cannot query VCardExport: %2", (Object)LOGCLASS, (Object)e.toString());
            }

            boolean asiAdminAvailable = (asiAdminRefs != null && asiAdminRefs.length > 0);
            boolean vCardExportAvailable = (vCardExportRefs != null && vCardExportRefs.length > 0);

            logChannel.log(1000000, "[%1.<init>]   ASIServiceAdmin: %2", (Object)LOGCLASS, (Object)(asiAdminAvailable ? "AVAILABLE" : "NOT FOUND"));
            logChannel.log(1000000, "[%1.<init>]   VCardExport: %2", (Object)LOGCLASS, (Object)(vCardExportAvailable ? "AVAILABLE" : "NOT FOUND"));

            if (!vCardExportAvailable) {
                logChannel.log(1000000, "[%1.<init>] VCardExport not found yet - ServiceTracker will wait for it", (Object)LOGCLASS);
            }

            if (!asiAdminAvailable) {
                logChannel.log(100000, "[%1.<init>] ASIServiceAdmin not available, cannot initialize StorageMountHandler", (Object)LOGCLASS);
            } else {
                // Step 2: Try to get BundleContext
                Object serviceManager = iContext.getServiceManager();
                BundleContext bundleContext = null;

                // Try direct cast first
                if (serviceManager instanceof BundleContext) {
                    bundleContext = (BundleContext) serviceManager;
                    logChannel.log(1000000, "[%1.<init>] ServiceManager is a BundleContext (direct cast)", (Object)LOGCLASS);
                } else {
                    // Try reflection to find getBundleContext() method
                    logChannel.log(1000000, "[%1.<init>] ServiceManager type: %2", (Object)LOGCLASS, (Object)serviceManager.getClass().getName());
                    try {
                        java.lang.reflect.Method getBundleContextMethod = serviceManager.getClass().getMethod("getBundleContext", new Class[0]);
                        bundleContext = (BundleContext) getBundleContextMethod.invoke(serviceManager, new Object[0]);
                        logChannel.log(1000000, "[%1.<init>] Got BundleContext via reflection", (Object)LOGCLASS);
                    } catch (NoSuchMethodException nsme) {
                        logChannel.log(100000, "[%1.<init>] ServiceManager has no getBundleContext() method", (Object)LOGCLASS);
                    }
                }

                // Step 3: Initialize StorageMountHandler if we have BundleContext
                if (bundleContext != null) {
                    storageMountHandler = new StorageMountHandler(bundleContext, logChannel);
                    storageMountHandler.init();
                    logChannel.log(1000000, "[%1.<init>] StorageMountHandler initialized successfully", (Object)LOGCLASS);
                } else {
                    logChannel.log(100000, "[%1.<init>] Could not obtain BundleContext, StorageMountHandler not initialized", (Object)LOGCLASS);
                }
            }
        } catch (Exception e) {
            logChannel.log(100000, "[%1.<init>] Could not initialize StorageMountHandler: %2", (Object)LOGCLASS, (Object)e.toString());
        }

        // Create AndroidAutoClusterIntegration with StorageMountHandler
        this.clusterIntegration = new AndroidAutoClusterIntegration(storageMountHandler);
        this.clusterIntegration.setLogger();

        // Lookup CombiBAPServiceNavi from OSGi registry
        try {
            ServiceReference[] refs = iContext.getServiceManager().getServiceReferences(CombiBAPServiceNavi.class);
            if (refs != null && refs.length > 0) {
                CombiBAPServiceNavi bapService = (CombiBAPServiceNavi) iContext.getServiceManager().getService(refs[0]);
                if (bapService != null) {
                    this.clusterIntegration.setClusterService(bapService);
                    logChannel.log(1000000, "[%1.<init>] CombiBAPServiceNavi found and set", (Object)LOGCLASS);
                } else {
                    logChannel.log(100000, "[%1.<init>] CombiBAPServiceNavi service reference found but getService returned null", (Object)LOGCLASS);
                }
            } else {
                logChannel.log(100000, "[%1.<init>] CombiBAPServiceNavi NOT found in OSGi registry", (Object)LOGCLASS);
            }
        } catch (Exception e) {
            logChannel.log(100000, "[%1.<init>] Error looking up CombiBAPServiceNavi: %2", (Object)LOGCLASS, (Object)e.toString());
        }

        // Check if CombiBAPServiceAudio is available (for audio BAP integration)
        try {
            ServiceReference[] audioRefs = iContext.getServiceManager().getServiceReferences(CombiBAPServiceAudio.class);
            if (audioRefs != null && audioRefs.length > 0) {
                logChannel.log(1000000, "[%1.<init>] CombiBAPServiceAudio AVAILABLE", (Object)LOGCLASS);
                this.clusterIntegration.logCluster("SYS: CombiBAPServiceAudio service AVAILABLE");
            } else {
                logChannel.log(100000, "[%1.<init>] CombiBAPServiceAudio NOT found in OSGi registry", (Object)LOGCLASS);
                this.clusterIntegration.logCluster("SYS: CombiBAPServiceAudio service NOT FOUND");
            }
        } catch (Exception e) {
            logChannel.log(100000, "[%1.<init>] Error looking up CombiBAPServiceAudio: %2", (Object)LOGCLASS, (Object)e.toString());
            this.clusterIntegration.logCluster("SYS: CombiBAPServiceAudio service ERROR");
        }

        // Lookup ICarStatisticsService from OSGi registry for unit detection
        try {
            ServiceReference[] popupRefs = iContext.getServiceManager().getServiceReferences(de.audi.atip.interapp.combi.bap.PartialPopupBAPService.class);
            if (popupRefs != null && popupRefs.length > 0) {
                de.audi.atip.interapp.combi.bap.PartialPopupBAPService popupService = 
                    (de.audi.atip.interapp.combi.bap.PartialPopupBAPService) iContext.getServiceManager().getService(popupRefs[0]);
                if (popupService != null) {
                    this.clusterIntegration.setPopupService(popupService);
                    logChannel.log(1000000, "[%1.<init>] PartialPopupBAPService found and set", (Object)LOGCLASS);
                } else {
                    logChannel.log(100000, "[%1.<init>] PartialPopupBAPService service reference found but getService returned null", (Object)LOGCLASS);
                }
            } else {
                logChannel.log(100000, "[%1.<init>] PartialPopupBAPService NOT found in OSGi registry", (Object)LOGCLASS);
            }
        } catch (Exception e) {
            logChannel.log(100000, "[%1.<init>] Error looking up PartialPopupBAPService: %2", (Object)LOGCLASS, (Object)e.toString());
        }

        // Lookup ICarStatisticsService from OSGi registry for unit detection
        try {
            ServiceReference[] statsRefs = iContext.getServiceManager().getServiceReferences(ICarStatisticsService.class);
            if (statsRefs != null && statsRefs.length > 0) {
                ICarStatisticsService statsService = (ICarStatisticsService) iContext.getServiceManager().getService(statsRefs[0]);
                if (statsService != null) {
                    this.clusterIntegration.setStatisticsService(statsService);
                    logChannel.log(1000000, "[%1.<init>] ICarStatisticsService found and set", (Object)LOGCLASS);
                } else {
                    logChannel.log(100000, "[%1.<init>] ICarStatisticsService service reference found but getService returned null", (Object)LOGCLASS);
                }
            } else {
                logChannel.log(100000, "[%1.<init>] ICarStatisticsService NOT found in OSGi registry", (Object)LOGCLASS);
            }
        } catch (Exception e) {
            logChannel.log(100000, "[%1.<init>] Error looking up ICarStatisticsService: %2", (Object)LOGCLASS, (Object)e.toString());
        }

        // Lookup ISysServices from OSGi registry for unit detection 
        try {
            ServiceReference[] sysRefs = iContext.getServiceManager().getServiceReferences(ISysServices.class);
            if (sysRefs != null && sysRefs.length > 0) {
                ISysServices sysService = (ISysServices) iContext.getServiceManager().getService(sysRefs[0]);
                if (sysService != null) {
                    this.clusterIntegration.setSysServices(sysService);
                    logChannel.log(1000000, "[%1.<init>] ISysServices found and set", (Object)LOGCLASS);
                } else {
                    logChannel.log(100000, "[%1.<init>] ISysServices service reference found but getService returned null", (Object)LOGCLASS);
                }
            } else {
                logChannel.log(100000, "[%1.<init>] ISysServices NOT found in OSGi registry", (Object)LOGCLASS);
            }
        } catch (Exception e) {
            logChannel.log(100000, "[%1.<init>] Error looking up ISysServices: %2", (Object)LOGCLASS, (Object)e.toString());
        }

        // Lookup ICarCoreServices from OSGi registry for drive side detection
        try {
            ServiceReference[] coreRefs = iContext.getServiceManager().getServiceReferences(ICarCoreServices.class);
            if (coreRefs != null && coreRefs.length > 0) {
                ICarCoreServices coreService = (ICarCoreServices) iContext.getServiceManager().getService(coreRefs[0]);
                if (coreService != null) {
                    this.clusterIntegration.setCarCoreServices(coreService);
                    logChannel.log(1000000, "[%1.<init>] ICarCoreServices found and set", (Object)LOGCLASS);
                } else {
                    logChannel.log(100000, "[%1.<init>] ICarCoreServices service reference found but getService returned null", (Object)LOGCLASS);
                }
            } else {
                logChannel.log(100000, "[%1.<init>] ICarCoreServices NOT found in OSGi registry", (Object)LOGCLASS);
            }
        } catch (Exception e) {
            logChannel.log(100000, "[%1.<init>] Error looking up ICarCoreServices: %2", (Object)LOGCLASS, (Object)e.toString());
        }

        // Lookup ICarExteriorLightService from OSGi registry for tourist state detection
        try {
            ServiceReference[] lightRefs = iContext.getServiceManager().getServiceReferences(ICarExteriorLightService.class);
            if (lightRefs != null && lightRefs.length > 0) {
                ICarExteriorLightService lightService = (ICarExteriorLightService) iContext.getServiceManager().getService(lightRefs[0]);
                if (lightService != null) {
                    this.clusterIntegration.setExteriorLightService(lightService);
                    logChannel.log(1000000, "[%1.<init>] ICarExteriorLightService found and set", (Object)LOGCLASS);
                } else {
                    logChannel.log(100000, "[%1.<init>] ICarExteriorLightService service reference found but getService returned null", (Object)LOGCLASS);
                }
            } else {
                logChannel.log(100000, "[%1.<init>] ICarExteriorLightService NOT found in OSGi registry", (Object)LOGCLASS);
            }
        } catch (Exception e) {
            logChannel.log(100000, "[%1.<init>] Error looking up ICarExteriorLightService: %2", (Object)LOGCLASS, (Object)e.toString());
        }

        // Lookup DSIMapViewerControl with KOMBI instance (=3) so we can ask the
        // native nav to re-create its window on displayable 33 after AA/CarPlay exits.
        try {
            ServiceReference[] mapRefs = iContext.getServiceManager().getServiceReferences(DSIMapViewerControl.class);
            if (mapRefs != null && mapRefs.length > 0) {
                DSIMapViewerControl kombiMap = null;
                int chosenInstance = -1;
                int i;
                for (i = 0; i < mapRefs.length; i++) {
                    Object instProp = mapRefs[i].getProperty("DEVICE_INSTANCE");
                    int inst = (instProp instanceof Integer) ? ((Integer) instProp).intValue() : -1;
                    if (inst == 3) {
                        kombiMap = (DSIMapViewerControl) iContext.getServiceManager().getService(mapRefs[i]);
                        chosenInstance = inst;
                        break;
                    }
                }
                if (kombiMap != null) {
                    this.clusterIntegration.setKombiMapViewerControl(kombiMap);
                    logChannel.log(1000000, "[%1.<init>] DSIMapViewerControl(KOMBI=3) found and set", (Object)LOGCLASS);
                } else {
                    logChannel.log(100000, "[%1.<init>] DSIMapViewerControl found but no KOMBI instance (3) registered", (Object)LOGCLASS);
                }
            } else {
                logChannel.log(100000, "[%1.<init>] DSIMapViewerControl NOT found in OSGi registry", (Object)LOGCLASS);
            }
        } catch (Exception e) {
            logChannel.log(100000, "[%1.<init>] Error looking up DSIMapViewerControl: %2", (Object)LOGCLASS, (Object)e.toString());
        }

        // Lookup IMapClusterService (de.audi.tghu.navi.app.cluster.IMapClusterService)
        // by class-name string so we can call switchKombiMapToAShownContext() to
        // trigger the full HMI re-entry sequence (which recreates displayable 33).
        // Class is not in our compile-time classpath; use reflection.
        try {
            String mapClusterClassName = "de.audi.tghu.navi.app.cluster.IMapClusterService";
            Class mapClusterClass = Class.forName(mapClusterClassName);
            ServiceReference[] mcRefs = iContext.getServiceManager().getServiceReferences(mapClusterClass);
            if (mcRefs != null && mcRefs.length > 0) {
                Object mapClusterSvc = iContext.getServiceManager().getService(mcRefs[0]);
                if (mapClusterSvc != null) {
                    this.clusterIntegration.setMapClusterService(mapClusterSvc);
                    logChannel.log(1000000, "[%1.<init>] IMapClusterService found and set", (Object)LOGCLASS);
                } else {
                    logChannel.log(100000, "[%1.<init>] IMapClusterService ref found but getService returned null", (Object)LOGCLASS);
                }
            } else {
                logChannel.log(100000, "[%1.<init>] IMapClusterService NOT found in OSGi registry", (Object)LOGCLASS);
            }
        } catch (ClassNotFoundException e) {
            logChannel.log(100000, "[%1.<init>] IMapClusterService class not loaded: %2", (Object)LOGCLASS, (Object)e.toString());
        } catch (Exception e) {
            logChannel.log(100000, "[%1.<init>] Error looking up IMapClusterService: %2", (Object)LOGCLASS, (Object)e.toString());
        }

        androidAuto2ListenerDistributor.addListener(new DSIAndroidAuto2ListenerSafe[]{androidAuto2RequestHandler, androidAuto2EventListener, androidAuto2AudioHandler, androidAuto2MicHandler, androidAuto2NavHandler, androidAuto2VideoHandler, androidAuto2VoiceSessionHandler, this.clusterIntegration});
        this.stateHandler = iStateHandler;
        this.logger.log(1000000, "[%1.<init>] AndroidAutoClusterIntegration registered with listener distributor", (Object)LOGCLASS);
    }

    public void init() {
        this.dsiController.init();
        this.dsiController.setStateListener((IDSIControllerStateListener)this);
    }

    public void deinit() {
        this.dsiController.deinit();
    }

    public void activate() {
        if (this.active) {
            this.logger.log(1000000, "[%1.activate] Already active.", (Object)LOGCLASS);
            return;
        }
        this.active = true;
        this.logger.log(1000000, "[%1.activate]", (Object)LOGCLASS);

        // Notify cluster integration that AA session is starting — gives the
        // h264 cluster decoder daemon time to warm up before videoAvailable(true)
        // arrives. Mirrors the deactivate() → onAndroidAutoTerminated() pattern.
        if (this.clusterIntegration != null) {
            this.clusterIntegration.onAndroidAutoActivated();
        }

        this.context.getTMKeyEventController().setDSIKeyEventController(this.keyEventController);
        if (this.isDsiAvailable()) {
            this.smartphoneManagerListener.updateDSIState(true);
        }
        this.androidAuto2DSIManager.activate();
        this.androidAutoSmartphoneProperties.activate();
        GMap gMap = Generics.newHashMap();
        gMap.put((Object)new ResourceOwnerChange(Resource.NOTIFICATION, ResourceOwner.DEVICE, ResourceOwner.MAINUNIT), (Object)new IStateChangeAction.StateChangeActionOverride(){

            public void execute(CommandList commandList, ITMState iTMState, IRequestor iRequestor, long l) {
                AndroidAuto2Subsystem.this.logger.log(1000000, "[%1.changeState] Notification: MU", (Object)LOGCLASS);
                AndroidAuto2Subsystem.this.context.getChoiceModel(3200015).setValue(1);
            }
        });
        gMap.put((Object)new ResourceOwnerChange(Resource.NOTIFICATION, ResourceOwner.MAINUNIT, ResourceOwner.DEVICE), (Object)new IStateChangeAction.StateChangeActionOverride(){

            public void execute(CommandList commandList, ITMState iTMState, IRequestor iRequestor, long l) {
                AndroidAuto2Subsystem.this.logger.log(1000000, "[%1.changeState] Notification: Device", (Object)LOGCLASS);
                AndroidAuto2Subsystem.this.context.getChoiceModel(3200015).setValue(0);
            }
        });
        gMap.put((Object)new ResourceStateChange(Resource.AUDIO_MEDIA, ResourceState.NORMAL, ResourceState.PAUSED), (Object)new IStateChangeAction.StateChangeActionOverride(){

            public void execute(CommandList commandList, ITMState iTMState, IRequestor iRequestor, long l) {
                AndroidAuto2Subsystem.this.logger.log(1000000, "[%1.changeState] Audio_Media: Normal -> Paused", (Object)LOGCLASS);
                AndroidAuto2Subsystem.this.androidAuto2DSIManager.pause(false);
                AndroidAuto2Subsystem.this.stateHandler.setStateForResource(Resource.AUDIO_MEDIA, ResourceState.PAUSED);
            }
        });
        gMap.put((Object)new ResourceStateChange(Resource.AUDIO_MEDIA, ResourceState.NORMAL, ResourceState.PAUSED_BY_MUTE), (Object)new IStateChangeAction.StateChangeActionOverride(){

            public void execute(CommandList commandList, ITMState iTMState, IRequestor iRequestor, long l) {
                AndroidAuto2Subsystem.this.logger.log(1000000, "[%1.changeState] Audio_Media: Normal -> Paused by mute", (Object)LOGCLASS);
                AndroidAuto2Subsystem.this.androidAuto2DSIManager.pause(true);
                AndroidAuto2Subsystem.this.stateHandler.setStateForResource(Resource.AUDIO_MEDIA, ResourceState.PAUSED_BY_MUTE);
            }
        });
        gMap.put((Object)new ResourceStateChange(Resource.AUDIO_MEDIA, ResourceState.PAUSED, ResourceState.NORMAL), (Object)new IStateChangeAction.StateChangeActionOverride(){

            public void execute(CommandList commandList, ITMState iTMState, IRequestor iRequestor, long l) {
                AndroidAuto2Subsystem.this.logger.log(1000000, "[%1.changeState] Audio_Media: Paused -> Normal", (Object)LOGCLASS);
                AndroidAuto2Subsystem.this.stateHandler.setStateForResource(Resource.AUDIO_MEDIA, ResourceState.NORMAL);
                if (iTMState.isResourceOwnerDevice(Resource.AUDIO_MEDIA)) {
                    AndroidAuto2Subsystem.this.androidAuto2DSIManager.resume();
                }
            }
        });
        gMap.put((Object)new ResourceStateChange(Resource.AUDIO_MEDIA, ResourceState.PAUSED_BY_MUTE, ResourceState.NORMAL), (Object)new IStateChangeAction.StateChangeActionOverride(){

            public void execute(CommandList commandList, ITMState iTMState, IRequestor iRequestor, long l) {
                AndroidAuto2Subsystem.this.logger.log(1000000, "[%1.changeState] Audio_Media: Paused_by_mute -> Normal", (Object)LOGCLASS);
                AndroidAuto2Subsystem.this.stateHandler.setStateForResource(Resource.AUDIO_MEDIA, ResourceState.NORMAL);
                if (iTMState.isResourceOwnerDevice(Resource.AUDIO_MEDIA) && iTMState.isAppFree(Application.SPEECH)) {
                    AndroidAuto2Subsystem.this.androidAuto2DSIManager.resume();
                }
            }
        });
        this.addRequestModeChanges(gMap);
        this.stateHandler.addStateChangeActions(gMap);
    }

    private void addRequestModeChanges(GMap gMap) {
        gMap.put((Object)new ResourceOwnerChange(Resource.SCREEN, ResourceOwner.MAINUNIT, ResourceOwner.DEVICE), (Object)new IStateChangeAction.StateChangeActionHook(){

            public void execute(CommandList commandList, ITMState iTMState, IRequestor iRequestor, long l) {
                AndroidAuto2Subsystem.this.logger.log(1000000, "[%1.changeState] Screen: MU -> Device", (Object)LOGCLASS);
                AndroidAuto2Subsystem.this.keyEventController.clearOutstandingEvents();
                if (IRequestor.DEVICE.isNot((Object)iRequestor)) {
                    commandList.add((ICommand)new RequestModeChange(AndroidAuto2Subsystem.this.context, iTMState, l, AndroidAuto2Subsystem.this.stateHandler));
                }
            }
        });
        gMap.put((Object)new ResourceOwnerChange(Resource.SCREEN, ResourceOwner.DEVICE, ResourceOwner.MAINUNIT), (Object)new IStateChangeAction.StateChangeActionHook(){

            public void execute(CommandList commandList, ITMState iTMState, IRequestor iRequestor, long l) {
                AndroidAuto2Subsystem.this.logger.log(1000000, "[%1.changeState] Screen: Device -> MU", (Object)LOGCLASS);
                if (!iRequestor.isOneOf((Object)IRequestor.DEVICE, (Object)IRequestor.DEVICE_WHILE_DISCONNECTING) && !AndroidAuto2Subsystem.this.stateHandler.getCurrentState().isAccessRestricted(Resource.SCREEN)) {
                    commandList.add((ICommand)new RequestModeChange(AndroidAuto2Subsystem.this.context, iTMState, l, AndroidAuto2Subsystem.this.stateHandler));
                }
            }
        });
        gMap.put((Object)new ResourceOwnerChange(Resource.AUDIO_MEDIA, ResourceOwner.DEVICE, ResourceOwner.MAINUNIT), (Object)new IStateChangeAction.StateChangeActionHook(){

            public void execute(CommandList commandList, ITMState iTMState, IRequestor iRequestor, long l) {
                AndroidAuto2Subsystem.this.logger.log(1000000, "[%1.changeState] audio media: device -> MU", (Object)LOGCLASS);
                if (iRequestor.is((Object)IRequestor.MAINUNIT)) {
                    commandList.add((ICommand)new RequestModeChange(AndroidAuto2Subsystem.this.context, iTMState, l, AndroidAuto2Subsystem.this.stateHandler));
                }
            }
        });
        gMap.put((Object)new ApplicationStateChange(Application.SPEECH, ApplicationOwner.DEVICE, ApplicationOwner.NOBODY), (Object)new IStateChangeAction.StateChangeActionHook(){

            public void execute(CommandList commandList, ITMState iTMState, IRequestor iRequestor, long l) {
                AndroidAuto2Subsystem.this.logger.log(1000000, "[%1.changeState] speech: device -> nobody", (Object)LOGCLASS);
                if (iTMState.isAppFree(Application.SPEECH) && iTMState.isResourceOwnerDevice(Resource.AUDIO_MEDIA) && !iTMState.getStateForResource(Resource.AUDIO_MEDIA).is((Object)ResourceState.PAUSED_BY_MUTE)) {
                    AndroidAuto2Subsystem.this.androidAuto2DSIManager.resume();
                }
                if (iRequestor.is((Object)IRequestor.MAINUNIT)) {
                    commandList.add((ICommand)new RequestModeChange(AndroidAuto2Subsystem.this.context, iTMState, -1L, AndroidAuto2Subsystem.this.stateHandler));
                }
            }
        });
        gMap.put((Object)new ApplicationStateChange(Application.SPEECH, ApplicationOwner.NOBODY, ApplicationOwner.MAINUNIT), (Object)new IStateChangeAction.StateChangeActionHook(){

            public void execute(CommandList commandList, ITMState iTMState, IRequestor iRequestor, long l) {
                AndroidAuto2Subsystem.this.logger.log(1000000, "[%1.changeState] speech: nobody -> mainunit", (Object)LOGCLASS);
                if (iRequestor.is((Object)IRequestor.MAINUNIT)) {
                    commandList.add((ICommand)new RequestModeChange(AndroidAuto2Subsystem.this.context, iTMState, l, AndroidAuto2Subsystem.this.stateHandler));
                }
            }
        });
        gMap.put((Object)new ApplicationStateChange(Application.SPEECH, ApplicationOwner.MAINUNIT, ApplicationOwner.NOBODY), (Object)new IStateChangeAction.StateChangeActionHook(){

            public void execute(CommandList commandList, ITMState iTMState, IRequestor iRequestor, long l) {
                AndroidAuto2Subsystem.this.logger.log(1000000, "[%1.changeState] speech: mainunit -> nobody", (Object)LOGCLASS);
                if (iRequestor.is((Object)IRequestor.MAINUNIT)) {
                    commandList.add((ICommand)new RequestModeChange(AndroidAuto2Subsystem.this.context, iTMState, l, AndroidAuto2Subsystem.this.stateHandler));
                }
            }
        });
        gMap.put((Object)new ApplicationStateChange(Application.NAVI, ApplicationOwner.DEVICE, ApplicationOwner.MAINUNIT), (Object)new IStateChangeAction.StateChangeActionHook(){

            public void execute(CommandList commandList, ITMState iTMState, IRequestor iRequestor, long l) {
                AndroidAuto2Subsystem.this.logger.log(1000000, "[%1.changeState] Navi: Device -> MU", (Object)LOGCLASS);
                if (iRequestor.is((Object)IRequestor.MAINUNIT)) {
                    commandList.add((ICommand)new RequestModeChange(AndroidAuto2Subsystem.this.context, iTMState, l, AndroidAuto2Subsystem.this.stateHandler));
                }
            }
        });
        gMap.put((Object)new ApplicationStateChange(Application.NAVI, ApplicationOwner.NOBODY, ApplicationOwner.MAINUNIT), (Object)new IStateChangeAction.StateChangeActionHook(){

            public void execute(CommandList commandList, ITMState iTMState, IRequestor iRequestor, long l) {
                AndroidAuto2Subsystem.this.logger.log(1000000, "[%1.changeState] Navi: Nobody -> MU", (Object)LOGCLASS);
                if (iRequestor.is((Object)IRequestor.MAINUNIT)) {
                    commandList.add((ICommand)new RequestModeChange(AndroidAuto2Subsystem.this.context, iTMState, l, AndroidAuto2Subsystem.this.stateHandler));
                }
            }
        });
        gMap.put((Object)new ApplicationStateChange(Application.NAVI, ApplicationOwner.MAINUNIT, ApplicationOwner.NOBODY), (Object)new IStateChangeAction.StateChangeActionHook(){

            public void execute(CommandList commandList, ITMState iTMState, IRequestor iRequestor, long l) {
                AndroidAuto2Subsystem.this.logger.log(1000000, "[%1.changeState] Navi: Mainunit -> Nobody", (Object)LOGCLASS);
                if (IRequestor.MAINUNIT == iRequestor) {
                    commandList.add((ICommand)new RequestModeChange(AndroidAuto2Subsystem.this.context, iTMState, l, AndroidAuto2Subsystem.this.stateHandler));
                }
            }
        });
    }

    public void deactivate() {
        if (!this.active) {
            this.logger.log(1000000, "[%1.deactivate] Mgr is not active.", (Object)LOGCLASS);
            return;
        }
        this.logger.log(1000000, "[%1.deactivate]", (Object)LOGCLASS);
        
        // Clear cluster display when Android Auto deactivates
        if (this.clusterIntegration != null) {
            this.clusterIntegration.onAndroidAutoTerminated();
        }
        
        this.stateHandler.removeStateChangeActions();
        this.androidAuto2DSIManager.deactivate();
        this.androidAutoSmartphoneProperties.deactivate();
        this.active = false;
        this.smartphoneManagerListener.updateDSIState(false);
    }

    public final void dsiUnavailable() {
        this.logger.log(1000000, "[%1.dsiUnavailable]", (Object)LOGCLASS);
        
        // Clear cluster display when DSI becomes unavailable
        if (this.clusterIntegration != null) {
            this.clusterIntegration.onAndroidAutoTerminated();
        }
        
        this.dsiAvailable = false;
        this.smartphoneManagerListener.updateDSIState(false);
    }

    public final void dsiAvailable() {
        this.logger.log(1000000, "[%1.dsiAvailable]", (Object)LOGCLASS);
        this.dsiAvailable = true;
        if (this.active) {
            this.smartphoneManagerListener.updateDSIState(true);
        }
    }

    protected final boolean isDsiAvailable() {
        return this.dsiAvailable;
    }
}
