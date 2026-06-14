/*
 * Copyright (c) 2026 fifthBro
 * https://fifthbro.github.io
 *
 * Licensed under CC BY-NC-SA 4.0
 * https://creativecommons.org/licenses/by-nc-sa/4.0/
 * NOT FOR COMMERCIAL USE
 */

package de.audi.app.terminalmode.smartphone.carplay;

import de.audi.app.terminalmode.IContext;
import de.audi.app.terminalmode.ISPIScreenConnectedProvider;
import de.audi.app.terminalmode.SmartphoneType;
import de.audi.app.terminalmode.TMTTSHandler;
import de.audi.app.terminalmode.TerminalModeUtils;
import de.audi.app.terminalmode.audio.IAudioManager;
import de.audi.app.terminalmode.audio.TMAudioConnection;
import de.audi.app.terminalmode.config.ITerminalModeConfiguration;
import de.audi.app.terminalmode.device.TMDevice;
import de.audi.app.terminalmode.diagnosis.IDiagnosisCommandProvider;
import de.audi.app.terminalmode.diagnosis.IDiagnosisDataProvider;
import de.audi.app.terminalmode.dsi.DSIResource;
import de.audi.app.terminalmode.dsi.IAppState;
import de.audi.app.terminalmode.dsi.IDSIAppState;
import de.audi.app.terminalmode.dsi.IDSIResource;
import de.audi.app.terminalmode.dsi.IResource;
import de.audi.app.terminalmode.dsi.carplay.CarPlayAppState;
import de.audi.app.terminalmode.dsi.carplay.CarPlayResource;
import de.audi.app.terminalmode.dsi.carplay.CarplayAudioType;
import de.audi.app.terminalmode.dsi.carplay.CarplayButton;
import de.audi.app.terminalmode.dsi.carplay.CarplayButtonState;
import de.audi.app.terminalmode.dsi.carplay.CarplayDSILifecycleController;
import de.audi.app.terminalmode.dsi.carplay.CarplayUtils;
import de.audi.app.terminalmode.dsi.carplay.ICarplayDSIController;
import de.audi.app.terminalmode.dsi.carplay.ICarplayDSIControllerListener;
import de.audi.app.terminalmode.dsi.carplay.MainAudioTypeUpdate;
import de.audi.app.terminalmode.dsi.carplay.ScreenId;
import de.audi.app.terminalmode.logging.LoggingUtil;
import de.audi.app.terminalmode.smartphone.AbstractDSISmartphoneManager;
import de.audi.app.terminalmode.smartphone.IDSISmartphoneManager;
import de.audi.app.terminalmode.smartphone.ILastmodehandler;
import de.audi.app.terminalmode.smartphone.IPhoneCallController;
import de.audi.app.terminalmode.smartphone.ISmartphoneProperties;
import de.audi.app.terminalmode.smartphone.ISpeechRequestHandler;
import de.audi.app.terminalmode.smartphone.SPIServiceState;
import de.audi.app.terminalmode.smartphone.carplay.CarPlaySpeechRequestHandler;
import de.audi.app.terminalmode.smartphone.carplay.CarplayRequest;
import de.audi.app.terminalmode.smartphone.carplay.CarplaySmartphoneProperties;
import de.audi.app.terminalmode.smartphone.carplay.RequestModeChangeAudio;
import de.audi.app.terminalmode.statemachine.Application;
import de.audi.app.terminalmode.statemachine.ApplicationOwner;
import de.audi.app.terminalmode.statemachine.ApplicationStateChange;
import de.audi.app.terminalmode.statemachine.IRequestor;
import de.audi.app.terminalmode.statemachine.IStateChange;
import de.audi.app.terminalmode.statemachine.IStateChangeAction;
import de.audi.app.terminalmode.statemachine.IStateHandler;
import de.audi.app.terminalmode.statemachine.ITMState;
import de.audi.app.terminalmode.statemachine.ITMStateListener;
import de.audi.app.terminalmode.statemachine.Resource;
import de.audi.app.terminalmode.statemachine.ResourceOwner;
import de.audi.app.terminalmode.statemachine.ResourceOwnerChange;
import de.audi.app.terminalmode.statemachine.ResourceState;
import de.audi.app.terminalmode.statemachine.ResourceStateChange;
import de.audi.app.terminalmode.statemachine.commands.AbstractCommand;
import de.audi.app.terminalmode.statemachine.commands.AbstractDSICommand;
import de.audi.app.terminalmode.statemachine.commands.AbstractStateHandlerCommand;
import de.audi.app.terminalmode.statemachine.commands.SendResponseModeChanged;
import de.audi.app.terminalmode.statemachine.commands.SetSampleRate;
import de.audi.app.terminalmode.statemachine.commands.UpdateMode;
import de.audi.atip.hmi.modelaccess.ChoiceModelApp;
import de.audi.atip.util.Util;
import de.audi.atip.utils.commandlist2.CommandList;
import de.audi.atip.utils.generics.Consumer;
import de.audi.atip.utils.generics.GMap;
import de.audi.atip.utils.generics.Generics;
import de.audi.atip.utils.reactive.bindings.SPIBoolean;
import de.audi.atip.utils.reactive.observables.Observables;
import de.audi.atip.utils.reactive.observables.Subscription;
import de.esolutions.fw.util.commons.Buffer;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Map;

/*
 * This class specifies class file version 48.0 but uses Java 6 signatures.  Assumed Java 6.
 */
public class CarPlayDSIManager
extends AbstractDSISmartphoneManager
implements ICarplayDSIControllerListener,
ITMStateListener,
IDiagnosisDataProvider,
IDiagnosisCommandProvider {
    private static final String LOGCLASS = "CarPlayDSIManager";
    private final CarplayDSILifecycleController carPlayDSILifecycleController;
    private final Map messageMap;
    private volatile CarPlayResource currentActiveResource;
    private volatile int activeApplications;
    private final ITMState currentState;
    private final ISpeechRequestHandler speechRequestHandler;
    private final IPhoneCallController phoneCallController;
    private final ICarplayDSIController carPlayDSIController;
    private final IStateHandler stateHandler;
    private final CarplaySmartphoneProperties carplaySmartphoneProperties;
    private volatile boolean waitForAudioType;
    private final IAudioManager audiomanager;
    private final ISmartphoneProperties smartphoneProperties;
    private volatile Subscription rvcSubscription;
    private volatile Subscription rvcSubscription2;
    private final ILastmodehandler lastmodehandler;
    private final ISPIScreenConnectedProvider screenConnectedProvider;
    private final ITerminalModeConfiguration configuration;
    private volatile boolean screenHiddenByPopup = false;
    private volatile boolean MUScreenBorrowRequestSend = false;
    private volatile boolean screenVisibleAfterPoup = false;
    private volatile boolean screenLastModeSentWithStartService = false;
    private volatile SPIBoolean sdsActive = SPIBoolean.INITIAL;
    private final TMTTSHandler ttsHandler;
    private final int REQUIRE_NOTHING = 0;
    private final int REQUIRE_REQUESTUI = 1;
    private final int REQUIRE_TAKE = 2;
    private int rvcOffConstraint = 0;
    private final String DIAG_UPDATE_TEXTINPUT = "textInput(boolean)";

    /* Cluster integration — mirror start/stop on screen resource changes */
    private CarPlayClusterIntegration clusterIntegration;

    public CarPlayDSIManager(IContext context, CarplayDSILifecycleController carplayDSILifecycleController, IStateHandler pStateHandler, CarplaySmartphoneProperties pCarplaySmartphoneProperties, IAudioManager pAudioManager, ISmartphoneProperties pSmartphoneProperties, ILastmodehandler pLastmodeHandler, ISPIScreenConnectedProvider pScreenConnectedProvider, ITerminalModeConfiguration pConfiguration, TMTTSHandler pTtsHandler) {
        super(context, SmartphoneType.CARPLAY);
        this.carPlayDSILifecycleController = carplayDSILifecycleController;
        this.messageMap = new HashMap();
        this.configuration = pConfiguration;
        this.stateHandler = pStateHandler;
        this.lastmodehandler = pLastmodeHandler;
        this.currentState = pStateHandler.getCurrentState();
        this.audiomanager = pAudioManager;
        this.carPlayDSIController = this.carPlayDSILifecycleController.getDioDsiController();
        this.speechRequestHandler = new CarPlaySpeechRequestHandler(context, this.carPlayDSIController);
        this.carplaySmartphoneProperties = pCarplaySmartphoneProperties;
        this.smartphoneProperties = pSmartphoneProperties;
        this.screenConnectedProvider = pScreenConnectedProvider;
        this.phoneCallController = new IPhoneCallController(){

            public void hook(boolean released) {
                CarPlayDSIManager.this.carPlayDSIController.postButtonEvent(CarplayButton.ACCEPT_CALL, released ? CarplayButtonState.RELEASED : CarplayButtonState.PRESSED);
            }

            public void hangup(boolean released) {
                CarPlayDSIManager.this.carPlayDSIController.postButtonEvent(CarplayButton.END_CALL, released ? CarplayButtonState.RELEASED : CarplayButtonState.PRESSED);
            }

            public void flash(boolean released) {
                CarPlayDSIManager.this.carPlayDSIController.postButtonEvent(CarplayButton.FLASH_CALL, released ? CarplayButtonState.RELEASED : CarplayButtonState.PRESSED);
            }
        };
        this.ttsHandler = pTtsHandler;
    }


    public void init() {
        super.init();
        this.carPlayDSILifecycleController.init();

        /* Register CarPlay cluster integration */
        try {
            this.clusterIntegration = new CarPlayClusterIntegration();
            this.clusterIntegration.loadConfig();

            /* Lookup services */
            try {
                org.osgi.framework.ServiceReference[] bapRefs = this.context.getServiceManager().getServiceReferences(de.audi.atip.interapp.combi.bap.navi.CombiBAPServiceNavi.class);
                if (bapRefs != null && bapRefs.length > 0) {
                    de.audi.atip.interapp.combi.bap.navi.CombiBAPServiceNavi svc = (de.audi.atip.interapp.combi.bap.navi.CombiBAPServiceNavi) this.context.getServiceManager().getService(bapRefs[0]);
                    if (svc != null) this.clusterIntegration.setClusterService(svc);
                }
            } catch (Exception ex) { this.logger.log(100000, "[%1.init] CombiBAPServiceNavi lookup failed: %2", (Object)LOGCLASS, (Object)ex.toString()); }

            try {
                org.osgi.framework.ServiceReference[] sysRefs = this.context.getServiceManager().getServiceReferences(de.audi.mib.system.ISysServices.class);
                if (sysRefs != null && sysRefs.length > 0) {
                    de.audi.mib.system.ISysServices svc = (de.audi.mib.system.ISysServices) this.context.getServiceManager().getService(sysRefs[0]);
                    if (svc != null) this.clusterIntegration.setSysServices(svc);
                }
            } catch (Exception ex) { this.logger.log(100000, "[%1.init] ISysServices lookup failed: %2", (Object)LOGCLASS, (Object)ex.toString()); }

            try {
                org.osgi.framework.ServiceReference[] coreRefs = this.context.getServiceManager().getServiceReferences(de.audi.app.car.api.core.ICarCoreServices.class);
                if (coreRefs != null && coreRefs.length > 0) {
                    de.audi.app.car.api.core.ICarCoreServices svc = (de.audi.app.car.api.core.ICarCoreServices) this.context.getServiceManager().getService(coreRefs[0]);
                    if (svc != null) this.clusterIntegration.setCarCoreServices(svc);
                }
            } catch (Exception ex) { this.logger.log(100000, "[%1.init] ICarCoreServices lookup failed: %2", (Object)LOGCLASS, (Object)ex.toString()); }

            try {
                org.osgi.framework.ServiceReference[] lightRefs = this.context.getServiceManager().getServiceReferences(de.audi.app.car.api.services.ICarExteriorLightService.class);
                if (lightRefs != null && lightRefs.length > 0) {
                    de.audi.app.car.api.services.ICarExteriorLightService svc = (de.audi.app.car.api.services.ICarExteriorLightService) this.context.getServiceManager().getService(lightRefs[0]);
                    if (svc != null) this.clusterIntegration.setExteriorLightService(svc);
                }
            } catch (Exception ex) { this.logger.log(100000, "[%1.init] ICarExteriorLightService lookup failed: %2", (Object)LOGCLASS, (Object)ex.toString()); }

            try {
                org.osgi.framework.ServiceReference[] popupRefs = this.context.getServiceManager().getServiceReferences(de.audi.atip.interapp.combi.bap.PartialPopupBAPService.class);
                if (popupRefs != null && popupRefs.length > 0) {
                    de.audi.atip.interapp.combi.bap.PartialPopupBAPService svc = (de.audi.atip.interapp.combi.bap.PartialPopupBAPService) this.context.getServiceManager().getService(popupRefs[0]);
                    if (svc != null) this.clusterIntegration.setPopupService(svc);
                }
            } catch (Exception ex) { this.logger.log(100000, "[%1.init] PartialPopupBAPService lookup failed: %2", (Object)LOGCLASS, (Object)ex.toString()); }

            this.clusterIntegration.init();

            this.context.getServiceManager().registerDSIListener(
                0, "org.dsi.ifc.carplay.DSICarplayListener", this.clusterIntegration);
            this.clusterIntegration.logCluster("DSI: registerDSIListener OK");

            this.logger.log(1000000, "[%1.init] CarPlayClusterIntegration registered", (Object)LOGCLASS);
        } catch (Exception e) {
            this.logger.log(100000, "[%1.init] Failed: %2", (Object)LOGCLASS, (Object)e.toString());
        }
        this.carPlayDSILifecycleController.setStateListener(this);
        this.stateHandler.addStateListener(this);
        this.context.getDiagnosisManager().addDataProvider(-1, this);
        this.context.getDiagnosisManager().addCommandProvider(-1, this);
        this.rvcSubscription = Observables.combineLatest(this.smartphoneProperties.getPropertyMURVCActive(), this.smartphoneProperties.getDisplayIsOff()).subscribe(new Observables.Consumer2(){

        
            public void accept(Boolean rvcActive, Boolean pDisplayOff) {
                CarPlayDSIManager.this.logger.log(1000000, "[%1.accept] rvc=%2 displayOff=%3", (Object)CarPlayDSIManager.LOGCLASS, (Object)rvcActive, (Object)pDisplayOff);
                if (rvcActive.booleanValue() || pDisplayOff.booleanValue()) {
                    CarPlayDSIManager.this.activeApplications |= 1;
                    if (rvcActive.booleanValue() && CarPlayDSIManager.this.MUScreenBorrowRequestSend) {
                        CarPlayDSIManager.this.logger.log(1000000, "[%1.accept] RVC on special popoup usecase additonal unborrow must be send", (Object)CarPlayDSIManager.LOGCLASS);
                        CarPlayDSIManager.this.carPlayDSIController.requestModeChange(new IDSIAppState[0], new IDSIResource[]{TerminalModeUtils.getUnborrowScreen()}, "Screen Setup");
                        CarPlayDSIManager.this.screenVisibleAfterPoup = false;
                        CarPlayDSIManager.this.MUScreenBorrowRequestSend = false;
                    }
                } else {
                    CarPlayDSIManager.this.activeApplications &= -2;
                }
            }

        
            public /* synthetic */ void accept(Object x0, Object x1) {
                this.accept((Boolean)x0, (Boolean)x1);
            }
        });
        this.rvcSubscription2 = Observables.combineLatest(this.smartphoneProperties.getPropertyMURVCActive(), this.smartphoneProperties.getPropertySPIServiceState()).subscribe(new Observables.Consumer2(){
            private boolean previousState = false;
            private boolean rvcOffWhileStarting = false;

        
            public void accept(Boolean rvcActive, SPIServiceState serviceState) {
                if (SPIServiceState.STARTED.is(serviceState)) {
                    if (this.previousState && !rvcActive.booleanValue() || this.rvcOffWhileStarting) {
                        if (this.rvcOffWhileStarting) {
                            this.rvcOffWhileStarting = false;
                        }
                        if (((TMDevice)CarPlayDSIManager.this.context.getDeviceManager().getProperties().activeDevice().get()).isScreenLastMode()) {
                            CarPlayDSIManager.this.logger.log(1000000, "[%1.accept#rvcSubscription2] LASTMODE TERMINAL MODE", (Object)CarPlayDSIManager.LOGCLASS);
                            CarPlayDSIManager.this.rvcOffConstraint = 1;
                        } else {
                            CarPlayDSIManager.this.logger.log(1000000, "[%1.accept#rvcSubscription2] LASTMODE NON SPI", (Object)CarPlayDSIManager.LOGCLASS);
                            CarPlayDSIManager.this.rvcOffConstraint = 2;
                        }
                        this.previousState = false;
                    }
                } else {
                    if (SPIServiceState.STARTING.is(serviceState) && this.previousState && !rvcActive.booleanValue()) {
                        this.rvcOffWhileStarting = true;
                    } else if (SPIServiceState.NOT_STARTED.is(serviceState)) {
                        CarPlayDSIManager.this.rvcOffConstraint = 0;
                    }
                    this.previousState = ((Boolean)rvcActive).booleanValue();
                }
            }

        
            public /* synthetic */ void accept(Object x0, Object x1) {
                this.accept((Boolean)x0, (SPIServiceState)x1);
            }
        });
    }


    public void deinit() {
        this.stateHandler.removeStateListener(this);
        this.carPlayDSILifecycleController.deinit();
        super.deinit();
        if (null != this.rvcSubscription) {
            this.rvcSubscription.cancel();
        }
        if (null != this.rvcSubscription2) {
            this.rvcSubscription2.cancel();
        }
    }

    private void updateTMStateWithAudioMediaState(ResourceState pResourceState) {
        ITMState updatedState = this.stateHandler.getCurrentState();
        updatedState.setStateForResource(Resource.AUDIO_MEDIA, pResourceState);
        this.stateHandler.updateState(updatedState);
    }


    public void activate() {
        if (this.active) {
            this.logger.log(1000000, "[%1.activate] Already active.", (Object)LOGCLASS);
            return;
        }
        this.active = true;
        super.activate();
        this.logger.log(1000000, "[%1.activate]", (Object)LOGCLASS);
        this.carplaySmartphoneProperties.activate();
        this.context.getTMKeyEventController().setDSIKeyEventController(this.carPlayDSILifecycleController.getKeyEventController());
        if (this.isDsiAvailable()) {
            this.smartphoneManagerListener.updateDSIState(true);
        }
        GMap stateChangeMap = Generics.newHashMap();
        stateChangeMap.put(new ResourceStateChange(Resource.AUDIO_MEDIA, ResourceState.NORMAL, ResourceState.PAUSED), new IStateChangeAction.StateChangeActionOverride(){

            public void execute(CommandList commandList, ITMState pNewState, IRequestor pRequestor, long pMsgId) {
                CarPlayDSIManager.this.logger.log(1000000, "[%1.changeState] Audio_Media: Normal -> Paused", (Object)CarPlayDSIManager.LOGCLASS);
                if (ResourceOwner.DEVICE.is(pNewState.getOwnerForResource(Resource.AUDIO_PHONE)) || ResourceOwner.DEVICE.is(pNewState.getOwnerForResource(Resource.AUDIO_SPEECH)) || ResourceOwner.DEVICE.is(pNewState.getOwnerForResource(Resource.AUDIO_RINGTONE))) {
                    return;
                }
                CarPlayDSIManager.this.sdsActive = (SPIBoolean)CarPlayDSIManager.this.carplaySmartphoneProperties.getPropertySDSActive().get();
                CarPlayDSIManager.this.logger.log(1000000, "[%1.execute] statusSDS=%2", (Object)CarPlayDSIManager.LOGCLASS, (Object)CarPlayDSIManager.this.sdsActive);
                if (CarPlayDSIManager.this.sdsActive.isValid() && CarPlayDSIManager.this.sdsActive.getBoolean()) {
                    String LOGCMD = "UpdateTMStateOnly";
                    commandList.add(new AbstractStateHandlerCommand(CarPlayDSIManager.this.logger, "UpdateTMStateOnly", CarPlayDSIManager.this.context, CarPlayDSIManager.this.stateHandler){
                        private static final String LOGINFO = "(Audio_Media: Normal -> Paused)";

                        public void execute() {
                            this.logger.log(1000000, "### CMD ### [%1][%2] ANONYMOUS: %3", (Object)CarPlayDSIManager.LOGCLASS, (Object)"UpdateTMStateOnly", (Object)LOGINFO);
                            CarPlayDSIManager.this.updateTMStateWithAudioMediaState(ResourceState.PAUSED);
                            this.finish();
                        }
                    });
                    return;
                }
                commandList.add(new RequestModeChangeAudio(CarPlayDSIManager.this.logger, CarPlayDSIManager.this.context, CarPlayDSIManager.this, pNewState, CarPlayDSIManager.this.stateHandler){

                    public void execute() {
                        this.logger.log(1000000, "### DSI ### [%1.execute]", (Object)CarPlayDSIManager.LOGCLASS);
                        CarPlayDSIManager.this.carPlayDSIController.requestModeChange(new IDSIAppState[0], new IDSIResource[]{new DSIResource.Builder().setpOwner(1).setpResourceId(2).setDsiResourceId(1).setDsiResourceOwner(1).setDsiTakeType(3).setDsiTransferPriority(1).setDsiTakeConstraint(0).setDsiBorrowConstraint(0).setDsiUnborrowConstraint(2).build()}, "audio paused");
                        CarPlayDSIManager.this.updateTMStateWithAudioMediaState(ResourceState.PAUSED);
                        this.finish();
                    }
                });
            }
        });
        stateChangeMap.put(new ResourceStateChange(Resource.AUDIO_MEDIA, ResourceState.NORMAL, ResourceState.PAUSED_BY_MUTE), new IStateChangeAction.StateChangeActionOverride(){

            public void execute(CommandList commandList, ITMState pNewState, IRequestor pRequestor, long pMsgId) {
                CarPlayDSIManager.this.logger.log(1000000, "[%1.changeState] Audio_Media: Normal -> Paused by mute", (Object)CarPlayDSIManager.LOGCLASS);
                if (ResourceOwner.DEVICE.is(pNewState.getOwnerForResource(Resource.AUDIO_PHONE)) || ResourceOwner.DEVICE.is(pNewState.getOwnerForResource(Resource.AUDIO_SPEECH)) || ResourceOwner.DEVICE.is(pNewState.getOwnerForResource(Resource.AUDIO_RINGTONE))) {
                    return;
                }
                commandList.add(new RequestModeChangeAudio(CarPlayDSIManager.this.logger, CarPlayDSIManager.this.context, CarPlayDSIManager.this, pNewState, CarPlayDSIManager.this.stateHandler){

                    public void execute() {
                        this.logger.log(1000000, "### DSI ### [%1.execute]", (Object)CarPlayDSIManager.LOGCLASS);
                        CarPlayDSIManager.this.carPlayDSIController.requestModeChange(new IDSIAppState[0], new IDSIResource[]{new DSIResource.Builder().setpOwner(1).setpResourceId(2).setDsiResourceId(1).setDsiResourceOwner(1).setDsiTakeType(3).setDsiTransferPriority(1).setDsiTakeConstraint(0).setDsiBorrowConstraint(0).setDsiUnborrowConstraint(2).build()}, "audio paused by mute");
                        ITMState updatedState = this.getStateHandler().getCurrentState();
                        updatedState.setStateForResource(Resource.AUDIO_MEDIA, ResourceState.PAUSED_BY_MUTE);
                        this.getStateHandler().updateState(updatedState);
                        this.finish();
                    }
                });
            }
        });
        stateChangeMap.put(new ResourceStateChange(Resource.AUDIO_MEDIA, ResourceState.PAUSED, ResourceState.NORMAL), new IStateChangeAction.StateChangeActionOverride(){

            public void execute(CommandList commandList, final ITMState pNewState, IRequestor pRequestor, long pMsgId) {
                CarPlayDSIManager.this.logger.log(1000000, "[%1.changeState] Audio_Media: Paused -> Normal", (Object)CarPlayDSIManager.LOGCLASS);
                if (CarPlayDSIManager.this.sdsActive.isValid() && CarPlayDSIManager.this.sdsActive.getBoolean()) {
                    String LOGCMD = "UpdateTMStateOnly";
                    commandList.add(new AbstractStateHandlerCommand(CarPlayDSIManager.this.logger, "UpdateTMStateOnly", CarPlayDSIManager.this.context, CarPlayDSIManager.this.stateHandler){
                        private static final String LOGINFO = "(Audio_Media: Paused -> Normal)";

                        public void execute() {
                            this.logger.log(1000000, "### CMD ### [%1][%2] ANONYMOUS: %3", (Object)CarPlayDSIManager.LOGCLASS, (Object)"UpdateTMStateOnly", (Object)LOGINFO);
                            CarPlayDSIManager.this.updateTMStateWithAudioMediaState(ResourceState.NORMAL);
                            CarPlayDSIManager.this.sdsActive = SPIBoolean.INITIAL;
                            this.finish();
                        }
                    });
                    return;
                }
                commandList.add(new RequestModeChangeAudio(CarPlayDSIManager.this.logger, CarPlayDSIManager.this.context, CarPlayDSIManager.this, pNewState, CarPlayDSIManager.this.stateHandler){

                    public void execute() {
                        this.logger.log(1000000, "### DSI ### [%1.execute]", (Object)CarPlayDSIManager.LOGCLASS);
                        if (!pNewState.isResourceOwnerMainUnit(Resource.AUDIO_MEDIA)) {
                            CarPlayDSIManager.this.carPlayDSIController.requestModeChange(new IDSIAppState[0], new IDSIResource[]{new DSIResource.Builder().setpOwner(2).setpResourceId(2).setDsiResourceId(1).setDsiResourceOwner(2).setDsiTakeType(4).setDsiTransferPriority(0).setDsiTakeConstraint(0).setDsiBorrowConstraint(0).setDsiUnborrowConstraint(0).build()}, "audio play");
                        }
                        CarPlayDSIManager.this.updateTMStateWithAudioMediaState(ResourceState.NORMAL);
                        this.finish();
                    }
                });
            }
        });
        stateChangeMap.put(new ResourceStateChange(Resource.AUDIO_MEDIA, ResourceState.PAUSED_BY_MUTE, ResourceState.NORMAL), new IStateChangeAction.StateChangeActionOverride(){

            public void execute(CommandList commandList, final ITMState pNewState, IRequestor pRequestor, long pMsgId) {
                CarPlayDSIManager.this.logger.log(1000000, "[%1.changeState] Audio_Media: Paused_by_mute -> Normal", (Object)CarPlayDSIManager.LOGCLASS);
                if (IRequestor.MAINUNIT == pRequestor) {
                    commandList.add(new RequestModeChangeAudio(CarPlayDSIManager.this.logger, CarPlayDSIManager.this.context, CarPlayDSIManager.this, pNewState, CarPlayDSIManager.this.stateHandler){

                        public void execute() {
                            this.logger.log(1000000, "### DSI ### [%1.execute]", (Object)CarPlayDSIManager.LOGCLASS);
                            if (!pNewState.isResourceOwnerMainUnit(Resource.AUDIO_MEDIA)) {
                                CarPlayDSIManager.this.carPlayDSIController.requestModeChange(new IDSIAppState[0], new IDSIResource[]{new DSIResource.Builder().setpOwner(2).setpResourceId(2).setDsiResourceId(1).setDsiResourceOwner(2).setDsiTakeType(4).setDsiBorrowConstraint(0).setDsiTakeConstraint(0).setDsiUnborrowConstraint(0).setDsiTransferPriority(0).build()}, "audio play after mute");
                            }
                            ITMState updatedState = this.getStateHandler().getCurrentState();
                            updatedState.setStateForResource(Resource.AUDIO_MEDIA, ResourceState.NORMAL);
                            this.getStateHandler().updateState(updatedState);
                            this.finish();
                        }
                    });
                }
            }
        });
        stateChangeMap.put(new ResourceStateChange(Resource.AUDIO_MEDIA, ResourceState.PAUSED, ResourceState.PAUSED_BY_MUTE), new IStateChangeAction.StateChangeActionOverride(){

            public void execute(CommandList commandList, ITMState pNewState, IRequestor pRequestor, long pMsgId) {
                CarPlayDSIManager.this.logger.log(1000000, "[%1.changeState] Audio_Media: Paused -> Paused by mute", (Object)CarPlayDSIManager.LOGCLASS);
                commandList.add(new RequestModeChangeAudio(CarPlayDSIManager.this.logger, CarPlayDSIManager.this.context, CarPlayDSIManager.this, pNewState, CarPlayDSIManager.this.stateHandler){

                    public void execute() {
                        this.logger.log(1000000, "### DSI ### [%1.execute]", (Object)CarPlayDSIManager.LOGCLASS);
                        CarPlayDSIManager.this.carPlayDSIController.requestModeChange(new IDSIAppState[0], new IDSIResource[]{new DSIResource.Builder().setpOwner(1).setpResourceId(2).setDsiResourceId(1).setDsiResourceOwner(1).setDsiTakeType(3).setDsiTransferPriority(1).setDsiTakeConstraint(0).setDsiBorrowConstraint(0).setDsiUnborrowConstraint(2).build()}, "Audio_Media: Paused -> Paused by mute");
                        ITMState updatedState = this.getStateHandler().getCurrentState();
                        updatedState.setStateForResource(Resource.AUDIO_MEDIA, ResourceState.PAUSED_BY_MUTE);
                        this.getStateHandler().updateState(updatedState);
                        this.finish();
                    }

                    public boolean updateStates(IAppState[] appState, IResource[] resources, long msgId) {
                        return false;
                    }
                });
            }
        });
        this.addRequestModeChanges(stateChangeMap);
        this.stateHandler.addStateChangeActions(stateChangeMap);
    }

    private void addRequestModeChanges(GMap stateChangeMap) {
        stateChangeMap.put(new ResourceOwnerChange(Resource.SCREEN, ResourceOwner.MAINUNIT, ResourceOwner.DEVICE), new IStateChangeAction.StateChangeActionHook(){

            public void execute(CommandList commandList, ITMState pNewState, IRequestor pRequestor, long pMsgId) {
                CarPlayDSIManager.this.logger.log(1000000, "[%1.changeState] Screen: MU -> Device", (Object)CarPlayDSIManager.LOGCLASS);
                Buffer buffer = new Buffer(100);
                buffer.append(", popup=");
                buffer.append(CarPlayDSIManager.this.screenVisibleAfterPoup);
                buffer.append(", requestor=");
                buffer.append(pRequestor.toString());
                buffer.append(", spiConnected=");
                buffer.append(CarPlayDSIManager.this.screenConnectedProvider.isAnySPIConnected());
                buffer.append(", sLastmodeStartS=");
                buffer.append(CarPlayDSIManager.this.screenLastModeSentWithStartService);
                buffer.append(", configLastmode=");
                buffer.append(CarPlayDSIManager.this.context.getConfiguration().useLastmodehandling());
                if (CarPlayDSIManager.this.screenVisibleAfterPoup || IRequestor.DEVICE.isNot(pRequestor) && (!CarPlayDSIManager.this.screenConnectedProvider.isAnySPIConnected() || !CarPlayDSIManager.this.screenLastModeSentWithStartService || !CarPlayDSIManager.this.context.getConfiguration().useLastmodehandling())) {
                    CarPlayDSIManager.this.logger.log(1000000, "[%1.changeState] update for carplay%2", (Object)CarPlayDSIManager.LOGCLASS, (Object)buffer.toString());
                    commandList.add(new RequestModeChangeCarPlay(CarPlayDSIManager.this.context, pNewState, CarPlayDSIManager.this.stateHandler));
                } else {
                    CarPlayDSIManager.this.logger.log(1000000, "[%1.changeState] no update for carplay%2", (Object)CarPlayDSIManager.LOGCLASS, (Object)buffer.toString());
                    if (IRequestor.DEVICE.isNot(pRequestor) && CarPlayDSIManager.this.screenLastModeSentWithStartService) {
                        CarPlayDSIManager.this.screenLastModeSentWithStartService = false;
                        CarPlayDSIManager.this.logger.log(1000000, "[%1.changeState] Reseting last mode flag%2", (Object)CarPlayDSIManager.LOGCLASS, (Object)buffer.toString());
                    }
                }
            }
        });
        stateChangeMap.put(new ResourceOwnerChange(Resource.SCREEN, ResourceOwner.DEVICE, ResourceOwner.MAINUNIT), new IStateChangeAction.StateChangeActionHook(){

            public void execute(CommandList commandList, ITMState pNewState, IRequestor pRequestor, long pMsgId) {
                CarPlayDSIManager.this.logger.log(1000000, "[%1.changeState] Screen: Device -> MU", (Object)CarPlayDSIManager.LOGCLASS);
                if (!pRequestor.isOneOf(IRequestor.DEVICE, IRequestor.DEVICE_WHILE_DISCONNECTING) && !CarPlayDSIManager.this.currentState.isAccessRestricted(Resource.SCREEN)) {
                    commandList.add(new RequestModeChangeCarPlay(CarPlayDSIManager.this.context, pNewState, CarPlayDSIManager.this.stateHandler));
                }
                CarPlayDSIManager.this.screenLastModeSentWithStartService = false;
            }
        });
        stateChangeMap.put(new ResourceOwnerChange(Resource.AUDIO_MEDIA, ResourceOwner.DEVICE, ResourceOwner.MAINUNIT), new IStateChangeAction.StateChangeActionHook(){

            public void execute(CommandList commandList, ITMState pNewState, IRequestor pRequestor, long pMsgId) {
                CarPlayDSIManager.this.logger.log(1000000, "[%1.changeState] audio media: device -> MU", (Object)CarPlayDSIManager.LOGCLASS);
                if (pRequestor.is(IRequestor.MAINUNIT)) {
                    boolean wasScreenChanged;
                    boolean bl = wasScreenChanged = CarPlayDSIManager.this.currentState.getOwnerForResource(Resource.SCREEN).is(ResourceOwner.DEVICE) && pNewState.getOwnerForResource(Resource.SCREEN).is(ResourceOwner.MAINUNIT);
                    if (wasScreenChanged) {
                        CarPlayDSIManager.this.logger.log(1000000, "[%1.changeState] audio media: DO NOT CHANGE AUDIO BECAUSE IT WAS CHANGED ALREADY", (Object)CarPlayDSIManager.LOGCLASS);
                    } else {
                        commandList.add(new RequestAudio(CarPlayDSIManager.this.context, CarPlayDSIManager.this.stateHandler));
                    }
                }
            }
        });
        stateChangeMap.put(new ApplicationStateChange(Application.SPEECH, ApplicationOwner.DEVICE, ApplicationOwner.NOBODY), new IStateChangeAction.StateChangeActionHook(){

            public void execute(CommandList commandList, ITMState pNewState, IRequestor pRequestor, long pMsgId) {
                CarPlayDSIManager.this.logger.log(1000000, "[%1.changeState] speech: device -> nobody", (Object)CarPlayDSIManager.LOGCLASS);
                if (pRequestor.is(IRequestor.MAINUNIT)) {
                    commandList.add(new RequestModeChangeCarPlay(CarPlayDSIManager.this.context, pNewState, CarPlayDSIManager.this.stateHandler));
                }
            }
        });
        stateChangeMap.put(new ApplicationStateChange(Application.SPEECH, ApplicationOwner.NOBODY, ApplicationOwner.MAINUNIT), new IStateChangeAction.StateChangeActionHook(){

            public void execute(CommandList commandList, ITMState pNewState, IRequestor pRequestor, long pMsgId) {
                CarPlayDSIManager.this.logger.log(1000000, "[%1.changeState] speech: nobody -> mainunit", (Object)CarPlayDSIManager.LOGCLASS);
                if (pRequestor.is(IRequestor.MAINUNIT)) {
                    CarPlayDSIManager.this.carplaySmartphoneProperties.getPropertySDSActive().set(CarPlayDSIManager.LOGCLASS, SPIBoolean.TRUE);
                }
            }
        });
        stateChangeMap.put(new ApplicationStateChange(Application.SPEECH, ApplicationOwner.MAINUNIT, ApplicationOwner.NOBODY), new IStateChangeAction.StateChangeActionHook(){

            public void execute(CommandList commandList, ITMState pNewState, IRequestor pRequestor, long pMsgId) {
                CarPlayDSIManager.this.logger.log(1000000, "[%1.changeState] speech: mainunit -> nobody", (Object)CarPlayDSIManager.LOGCLASS);
                if (pRequestor.is(IRequestor.MAINUNIT)) {
                    CarPlayDSIManager.this.carplaySmartphoneProperties.getPropertySDSActive().set(CarPlayDSIManager.LOGCLASS, SPIBoolean.FALSE);
                }
            }
        });
        stateChangeMap.put(new ApplicationStateChange(Application.SPEECH, ApplicationOwner.MAINUNIT, ApplicationOwner.DEVICE), new IStateChangeAction.StateChangeActionHook(){

            public void execute(CommandList commandList, ITMState pNewState, IRequestor pRequestor, long pMsgId) {
                CarPlayDSIManager.this.logger.log(1000000, "[%1.changeState] speech: mainunit -> device", (Object)CarPlayDSIManager.LOGCLASS);
                CarPlayDSIManager.this.smartphoneProperties.getNativeSpeechActive().subscribe(new Consumer(){

                
                    public void accept(Boolean data) {
                        if (!data.booleanValue()) {
                            CarPlayDSIManager.this.carplaySmartphoneProperties.getPropertySDSActive().set(CarPlayDSIManager.LOGCLASS, SPIBoolean.FALSE);
                        }
                    }

                
                    public /* synthetic */ void accept(Object x0) {
                        this.accept((Boolean)x0);
                    }
                });
            }
        });
        stateChangeMap.put(new ApplicationStateChange(Application.NAVI, ApplicationOwner.DEVICE, ApplicationOwner.MAINUNIT), new IStateChangeAction.StateChangeActionHook(){

            public void execute(CommandList commandList, ITMState pNewState, IRequestor pRequestor, long pMsgId) {
                CarPlayDSIManager.this.logger.log(1000000, "[%1.changeState] Navi: Device -> MU", (Object)CarPlayDSIManager.LOGCLASS);
                if (pRequestor.is(IRequestor.MAINUNIT)) {
                    commandList.add(new RequestModeChangeCarPlay(CarPlayDSIManager.this.context, pNewState, CarPlayDSIManager.this.stateHandler));
                }
            }
        });
        stateChangeMap.put(new ApplicationStateChange(Application.NAVI, ApplicationOwner.NOBODY, ApplicationOwner.MAINUNIT), new IStateChangeAction.StateChangeActionHook(){

            public void execute(CommandList commandList, ITMState pNewState, IRequestor pRequestor, long pMsgId) {
                CarPlayDSIManager.this.logger.log(1000000, "[%1.changeState] Navi: Nobody -> MU", (Object)CarPlayDSIManager.LOGCLASS);
                if (pRequestor.is(IRequestor.MAINUNIT)) {
                    commandList.add(new RequestModeChangeCarPlay(CarPlayDSIManager.this.context, pNewState, CarPlayDSIManager.this.stateHandler));
                }
            }
        });
        stateChangeMap.put(new ApplicationStateChange(Application.NAVI, ApplicationOwner.MAINUNIT, ApplicationOwner.NOBODY), new IStateChangeAction.StateChangeActionHook(){

            public void execute(CommandList commandList, ITMState pNewState, IRequestor pRequestor, long pMsgId) {
                CarPlayDSIManager.this.logger.log(1000000, "[%1.changeState] Navi: Mainunit -> Nobody", (Object)CarPlayDSIManager.LOGCLASS);
                if (IRequestor.MAINUNIT == pRequestor) {
                    commandList.add(new RequestModeChangeCarPlay(CarPlayDSIManager.this.context, pNewState, CarPlayDSIManager.this.stateHandler));
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

        // Mirror stop + cluster state reset, analogous to AA2Subsystem.deactivate()
        // calling clusterIntegration.onAndroidAutoTerminated().
        if (this.clusterIntegration != null) {
            this.clusterIntegration.onCarplayTerminated();
        }

        this.carplaySmartphoneProperties.deactivate();
        this.stateHandler.removeStateChangeActions();
        super.deactivate();
        this.active = false;
        this.smartphoneManagerListener.updateDSIState(false);
    }


    public void startService(ITMState pState) {
        this.updateActiveApplications(pState);
        this.carPlayDSIController.startService(this.getAppStates(pState), this.filterAudioResources(this.getResources(pState, true)));
        this.screenLastModeSentWithStartService = this.lastmodehandler.isScreenLastmode();
    }


    public void responseUpdateMode(ITMState pState, long pMsgId) {
        this.updateActiveApplications(pState);
        CarplayRequest carplayRequest = (CarplayRequest)this.messageMap.get(Util.createLong(pMsgId));
        this.logger.log(1000000, "[%1.responseUpdateMode] msgId=%2", (Object)LOGCLASS, pMsgId);
        if (null != carplayRequest && 2 == carplayRequest.getType()) {
            this.carPlayDSIController.responseUpdateMainAudioType((CarplayAudioType)carplayRequest.getParameter("AUDIOTYPE"));
        } else if (-1L != pMsgId) {
            this.carPlayDSIController.responseUpdateMode(this.filterAudioResources(this.getResources(pState, true)), this.getAppStates(pState));
        }
    }


    public void requestModeChange(ITMState pState, String reason, IDSISmartphoneManager.RequestModeChangeCallback callback) {
        throw new UnsupportedOperationException("CarPlay will access this directly. Method to be removed from the interface!");
    }


    public void requestConstraintsChange(ITMState tmState, Resource resource, boolean isRestricted) {
        this.logger.log(1000000, "[%1.requestConstraintsChange] %2 %3", (Object)LOGCLASS, (Object)(isRestricted ? "restricted" : "allowed"), (Object)resource);
        this.updateActiveApplications(tmState);
        this.carPlayDSIController.requestModeChange(new IDSIAppState[0], this.getResourcesForContraintsChange(resource, isRestricted), "requestConstraintsChange");
        if (!isRestricted && Resource.SCREEN.is(resource)) {
            this.screenLastModeSentWithStartService = false;
            if (ResourceOwner.DEVICE.is(tmState.getOwnerForResource(Resource.SCREEN))) {
                this.requestUI(ScreenId.SCREENID_CARPLAY);
            } else if (this.rvcOffConstraint == 1) {
                this.rvcOffConstraint = 0;
                this.requestUI(ScreenId.SCREENID_CARPLAY);
            }
        }
    }

    private IDSIResource[] getResourcesForContraintsChange(Resource resource, boolean isRestricted) {
        if (resource.is(Resource.SCREEN)) {
            if (isRestricted) {
                return new IDSIResource[]{new DSIResource.Builder().setpOwner(1).setpResourceId(1).setDsiResourceId(2).setDsiResourceOwner(1).setDsiTakeType(3).setDsiBorrowConstraint(0).setDsiTakeConstraint(0).setDsiUnborrowConstraint(3).setDsiTransferPriority(1).build()};
            }
            if (this.rvcOffConstraint == 2) {
                this.rvcOffConstraint = 0;
                return new IDSIResource[]{new DSIResource.Builder().setpOwner(1).setpResourceId(1).setDsiResourceId(2).setDsiResourceOwner(1).setDsiTakeType(1).setDsiTransferPriority(1).setDsiTakeConstraint(2).setDsiBorrowConstraint(1).setDsiUnborrowConstraint(0).build()};
            }
            return new IDSIResource[]{new DSIResource.Builder().setpOwner(1).setpResourceId(1).setDsiResourceId(2).setDsiResourceOwner(0).setDsiTakeType(4).setDsiBorrowConstraint(0).setDsiTakeConstraint(0).setDsiUnborrowConstraint(0).setDsiTransferPriority(0).build()};
        }
        if (resource.is(Resource.AUDIO_MEDIA)) {
            if (isRestricted) {
                return new IDSIResource[]{new DSIResource.Builder().setpOwner(1).setpResourceId(3).setDsiResourceId(1).setDsiResourceOwner(1).setDsiTakeType(3).setDsiBorrowConstraint(0).setDsiTakeConstraint(0).setDsiUnborrowConstraint(3).setDsiTransferPriority(1).build()};
            }
            return new IDSIResource[]{new DSIResource.Builder().setpOwner(1).setpResourceId(3).setDsiResourceId(1).setDsiResourceOwner(0).setDsiTakeType(4).setDsiBorrowConstraint(0).setDsiTakeConstraint(0).setDsiUnborrowConstraint(0).setDsiTransferPriority(0).build()};
        }
        return new IDSIResource[0];
    }

    private IDSIResource[] filterAudioResources(IDSIResource[] resources) {
        return this.filterAudioResources(resources, true);
    }

    private IDSIResource[] filterAudioResources(IDSIResource[] resources, boolean offset) {
        IDSIResource[] dsiResources = new IDSIResource[2];
        for (int i = 0; i < resources.length; ++i) {
            if (3 == resources[i].getResourceId()) {
                if (this.waitForAudioType && resources[i].getOwner() != 2) {
                    dsiResources[1] = this.createResource(3, 2, offset);
                    continue;
                }
                dsiResources[1] = resources[i];
                continue;
            }
            if (1 != resources[i].getResourceId()) continue;
            dsiResources[0] = resources[i];
        }
        return dsiResources;
    }

    private IDSIResource[] getResources(ITMState pTMState, boolean offset) {
        IDSIResource[] resources = new IDSIResource[]{pTMState.getStateForResource(Resource.AUDIO_MEDIA).is(ResourceState.PAUSED_BY_MUTE) ? new DSIResource.Builder().setpOwner(1).setpResourceId(3).setDsiResourceId(1).setDsiResourceOwner(1).setDsiTakeType(3).setDsiBorrowConstraint(0).setDsiTakeConstraint(0).setDsiUnborrowConstraint(1).setDsiTransferPriority(1).build() : (pTMState.getStateForResource(Resource.AUDIO_MEDIA).is(ResourceState.PAUSED) ? new DSIResource.Builder().setpOwner(1).setpResourceId(3).setDsiResourceId(1).setDsiResourceOwner(1).setDsiTakeType(3).setDsiBorrowConstraint(0).setDsiTakeConstraint(0).setDsiUnborrowConstraint(2).setDsiTransferPriority(1).build() : this.createResource(3, pTMState.isResourceOwnerMainUnit(Resource.AUDIO_MEDIA) || pTMState.getStateForResource(Resource.AUDIO_MEDIA).is(new ResourceState[]{ResourceState.PAUSED, ResourceState.PAUSED_BY_MUTE}) ? 1 : 2, offset)), this.createResource(4, pTMState.isResourceOwnerMainUnit(Resource.AUDIO_PHONE) ? 1 : 2, offset), this.createResource(5, pTMState.isResourceOwnerMainUnit(Resource.AUDIO_SPEECH) ? 1 : 2, offset), this.createResource(7, pTMState.isResourceOwnerMainUnit(Resource.AUDIO_RINGTONE) ? 1 : 2, offset), this.createResource(1, pTMState.isResourceOwnerMainUnit(Resource.SCREEN) ? 1 : 2, offset)};
        return resources;
    }

    private void requestUI(ScreenId pUiId) {
        this.logger.log(1000000, "[%1.requestUI] %2", (Object)LOGCLASS, (Object)pUiId);
        this.carPlayDSIController.requestUI(pUiId);
    }


    protected IDSIAppState createAppState(int pAppStateId, int pOwnerId, int pSpeechMode) {
        return new CarPlayAppState(pAppStateId, pOwnerId, pSpeechMode);
    }

    private IDSIResource createResource(int pResourceId, int pOwnerId, boolean offset) {
        return new CarPlayResource(pResourceId, pOwnerId, offset ? this.getActiveApplications() : this.activeApplications);
    }

    private int getActiveApplications() {
        return this.activeApplications + this.context.getConfiguration().applicationOffset();
    }

    private void updateActiveApplications(ITMState pState) {
        this.activeApplications = pState.isAppOwnerMainUnit(Application.SPEECH) ? (this.activeApplications |= 0x10) : (this.activeApplications &= 0xFFFFFFEF);
        this.activeApplications = pState.isAppOwnerMainUnit(Application.PHONE) ? (this.activeApplications |= 2) : (this.activeApplications &= 0xFFFFFFFD);
        this.activeApplications = pState.isAppOwnerMainUnit(Application.NAVI) ? (this.activeApplications |= 8) : (this.activeApplications &= 0xFFFFFFF7);
        this.activeApplications = pState.isResourceOwnerMainUnit(Resource.AUDIO_MEDIA) ? (this.activeApplications |= 4) : (this.activeApplications &= 0xFFFFFFFB);
        this.logger.log(1000000, "[%1.updateActiveApplications] %2", (Object)LOGCLASS, (Object)LoggingUtil.activeApplicationBitsToString(this.activeApplications));
    }


    public void updateMode(IAppState[] appState, IResource[] resources) {
        if (this.clusterIntegration != null) {
            for (int ri = 0; ri < resources.length; ri++) {
                int rid = resources[ri].getResourceId();
                int own = resources[ri].getOwner();
                this.clusterIntegration.logCluster("DSI: updateMode resource[" + ri + "] id=" + rid + " owner=" + own);
                this.clusterIntegration.onResourceChanged(rid, own);
            }
        }
        ArrayList resourceList = new ArrayList(resources.length);
        for (int i = 0; i < resources.length; ++i) {
            if (resources[i].getResourceId() == 2) {
                if (resources[i].getOwner() != 1) {
                    if (resources[i].getOwner() != 2) continue;
                    this.waitForAudioType = true;
                    continue;
                }
                resourceList.add(new CarPlayResource(3, 1, this.getActiveApplications()));
                resourceList.add(new CarPlayResource(4, 1, this.getActiveApplications()));
                resourceList.add(new CarPlayResource(5, 1, this.getActiveApplications()));
                resourceList.add(new CarPlayResource(7, 1, this.getActiveApplications()));
                this.currentActiveResource = null;
                this.waitForAudioType = false;
                continue;
            }
            if (((Boolean)this.smartphoneProperties.getDisplayIsOff().get()).booleanValue() && resources[i].getResourceId() == 1) {
                this.logger.log(1000000, "[%1.updateMode] Display OFF -> no update for the screen.", (Object)LOGCLASS);
                continue;
            }
            resourceList.add(resources[i]);
        }
        CarplayRequest carplayRequest = new CarplayRequest(++this.messageIdCounter, 1);
        this.messageMap.put(Util.createLong(carplayRequest.getMessageId()), carplayRequest);
        this.updateMode(carplayRequest.getMessageId(), appState, (IResource[])resourceList.toArray(new IResource[resourceList.size()]));
    }


    public void updateTextInputState(boolean textInputActive) {
        this.smartphoneManagerListener.updateTextInputState(textInputActive);
    }


    public void duckAudio(int duration, double volume) {
        this.logger.log(1000000, "[%1.duckAudio] dur=%2 vol=%3", (Object)LOGCLASS, (Object)String.valueOf(duration), (Object)String.valueOf(volume));
        this.smartphoneManagerListener.duckAudio(duration, volume);
    }


    public void unduckAudio(int duration) {
        this.logger.log(1000000, "[%1.unduckAudio] dur=%2", (Object)LOGCLASS, (long)duration);
        this.smartphoneManagerListener.unduckAudio(duration);
    }


    public void updateMainAudioType(MainAudioTypeUpdate audioTypeUpdate) {
        this.logger.log(1000000, "[%1.updateMainAudioType] %2 %3", (Object)LOGCLASS, (Object)TerminalModeUtils.logAudioType(audioTypeUpdate.getAudioType()), (long)audioTypeUpdate.getSampleRate());
        this.waitForAudioType = false;
        int resourceId = 0;
        CarplayAudioType carplayAudioType = CarplayAudioType.NONE;
        TMAudioConnection audioConnection = TMAudioConnection.INVALID;
        switch (audioTypeUpdate.getAudioType()) {
            case 1: {
                resourceId = 7;
                carplayAudioType = CarplayAudioType.ALERT;
                audioConnection = TMAudioConnection.RINGTONE;
                break;
            }
            case 2: {
                resourceId = 3;
                carplayAudioType = CarplayAudioType.MEDIA;
                audioConnection = TMAudioConnection.MEDIA;
                this.ttsHandler.terminateTTS();
                break;
            }
            case 4: {
                resourceId = 5;
                carplayAudioType = CarplayAudioType.SPEECHRECOGNITION;
                break;
            }
            case 3: {
                resourceId = 4;
                carplayAudioType = CarplayAudioType.TELEPHONY;
                audioConnection = TMAudioConnection.PHONE;
                break;
            }
        }
        if (5 == resourceId && ((Boolean)this.smartphoneProperties.getDisplayIsOff().get()).booleanValue() && null != this.context.getFramework().getPowerMgr()) {
            this.context.getFramework().getMsgDistrib().sendMessage(1025);
            this.context.getFramework().getPowerMgr().keyPTTPressed();
        }
        if (this.configuration.isSetSampleRateRequired() && TMAudioConnection.INVALID.isNot(audioConnection) && -1 != audioTypeUpdate.getSampleRate()) {
            this.context.getCommandListHelper().create().addSingle(new SetSampleRate(this.context, this.audiomanager, audioConnection, audioTypeUpdate.getSampleRate())).execute("CarplayDSIManager.updateMainAudioType");
        }
        CarplayRequest carplayRequest = new CarplayRequest(++this.messageIdCounter, 2);
        carplayRequest.addParameter("AUDIOTYPE", carplayAudioType);
        this.messageMap.put(Util.createLong(carplayRequest.getMessageId()), carplayRequest);
        CarPlayResource oldResource = this.currentActiveResource;
        this.currentActiveResource = 0 != resourceId ? new CarPlayResource(resourceId, 2, this.getActiveApplications()) : null;
        if (null != oldResource && (oldResource.getResourceId() != 3 || null == this.currentActiveResource)) {
            CarPlayResource oldResourceForUpdate = new CarPlayResource(oldResource.getResourceId(), 1, this.getActiveApplications());
            if (null != this.currentActiveResource) {
                this.updateMode(carplayRequest.getMessageId(), new IAppState[0], new IResource[]{oldResourceForUpdate, this.currentActiveResource});
            } else {
                this.updateMode(carplayRequest.getMessageId(), new IAppState[0], new IResource[]{oldResourceForUpdate});
            }
        } else {
            this.updateMode(carplayRequest.getMessageId(), new IAppState[0], new IResource[]{this.currentActiveResource});
        }
    }

    public void updateMode(long pMsgId, IAppState[] appState, IResource[] resources) {
        this.logger.log(1000000, "[%1.updateMode]", (Object)LOGCLASS);
        AbstractCommand currentCommand = this.getCurrentCommand();
        if (null == currentCommand || !currentCommand.updateStates(appState, resources, pMsgId)) {
            this.context.getCommandListHelper().create().addSingle(new UpdateMode(this.context, this, appState, resources, pMsgId, this.stateHandler)).addSingle(new SendResponseModeChanged(this.context, this, appState, resources, pMsgId, this.stateHandler)).execute("SmartphoneManager.updateMode");
        }
    }

    private AbstractCommand getCurrentCommand() {
        return (AbstractCommand)this.context.getCommandListManager().getActiveCommand().get();
    }


    public void seek(boolean pForward, boolean startSeekflag) {
    }


    public void skip(boolean pForward) {
    }


    public void skipButton(boolean pForward, boolean pReleased) {
        this.carPlayDSIController.postButtonEvent(pForward ? CarplayButton.SKIP_FORWARD : CarplayButton.SKIP_BACKWARD, pReleased ? CarplayButtonState.RELEASED : CarplayButtonState.PRESSED);
    }


    public void resume() {
    }


    public void pause(boolean pByMute) {
    }


    public void oemAppSelected() {
        this.logger.log(1000000, "[%1.oemAppSelected]", (Object)LOGCLASS);
        ChoiceModelApp choiceModel = this.context.getChoiceModel(3200014);
        choiceModel.setValue(choiceModel.getValue() == 0 ? 1 : 0);
    }


    public void updateState(ITMState pState) {
        this.currentState.update(pState);
    }


    public String getDiagKey() {
        return "getActiveConstraints";
    }


    public String getDiagValue() {
        Buffer buffer = new Buffer(200);
        int activeApps = this.getActiveApplications();
        buffer.append("Active apps: ");
        buffer.append(activeApps);
        buffer.append("\nScreen\n transfer type: ");
        buffer.append(CarplayUtils.logTransferType(CarplayUtils.getDsiTransferType(activeApps, true, false)));
        buffer.append("\ntake constraint: ");
        buffer.append(CarplayUtils.logResourceSharingPolicy(CarplayUtils.getDsiTakeConstraint(activeApps, true, false, false, false)));
        buffer.append("\nborrow constraint: ");
        buffer.append(CarplayUtils.logResourceSharingPolicy(CarplayUtils.getDsiBorrowConstraint(activeApps, true, false)));
        buffer.append("\nunborrow constraint: ");
        buffer.append(CarplayUtils.logResourceSharingPolicy(CarplayUtils.getDsiUnborrowConstraint(activeApps, true, false)));
        buffer.append("\nAudio\n transfer type: ");
        buffer.append(CarplayUtils.logTransferType(CarplayUtils.getDsiTransferType(activeApps, false, false)));
        buffer.append("\n take constraint: ");
        buffer.append(CarplayUtils.logResourceSharingPolicy(CarplayUtils.getDsiTakeConstraint(activeApps, false, false, false, false)));
        buffer.append("\n borrow constraint: ");
        buffer.append(CarplayUtils.logResourceSharingPolicy(CarplayUtils.getDsiBorrowConstraint(activeApps, false, false)));
        buffer.append("\n unborrow constraint: ");
        buffer.append(CarplayUtils.logResourceSharingPolicy(CarplayUtils.getDsiUnborrowConstraint(activeApps, false, false)));
        return buffer.toString();
    }


    public String[] getDiagKeys() {
        return new String[]{"textInput(boolean)"};
    }


    public void executeDiagCommand(String key, String[] parameters) {
        if ("textInput(boolean)".equals(key) && null != parameters && parameters.length >= 1) {
            boolean textInput = Boolean.valueOf(parameters[0]).booleanValue();
            this.updateTextInputState(textInput);
        }
    }

    public void requestNightMode(boolean useNightMode) {
        this.carPlayDSIController.requestNightMode(useNightMode);
    }


    public ISpeechRequestHandler getSpeechRequestHandler() {
        return this.speechRequestHandler;
    }


    public IPhoneCallController getPhoneCallController() {
        return this.phoneCallController;
    }


    public ILastmodehandler getLastmodeHandler() {
        return this.lastmodehandler;
    }


    public void screenChangeAction() {
        this.logger.log(1000000, "[%1.execute] Apple screen is coming", (Object)LOGCLASS);
        this.context.getFramework().getHMIService().removeCurrentPopup(0);
    }


    public void setScreenHiddenByPopup() {
        if (ResourceOwner.DEVICE.is(this.currentState.getOwnerForResource(Resource.SCREEN))) {
            this.screenHiddenByPopup = true;
        }
    }


    public void setScreenVisibleAfterPopup() {
        this.screenVisibleAfterPoup = true;
    }

    private class RequestAudio
    extends AbstractDSICommand {
        private static final String LOGCLASS = "RequestModeChangeCarPlay";
        private final IStateHandler stateHandler;

        public RequestAudio(IContext context, IStateHandler pStateHandler) {
            super(context.getLogger().main(), LOGCLASS, context, context.getSmartphoneDSIManager());
            this.stateHandler = pStateHandler;
        }

        public void execute() {
            this.logger.log(1000000, "### CMD ### [%1]", (Object)LOGCLASS);
            TMDevice activeDevice = this.context.getDeviceManager().getActiveDevice();
            if (null == activeDevice || activeDevice.connectionState().is(TMDevice.ConnectionState.NOT_ATTACHED)) {
                this.stateHandler.setOwnerForResource(Resource.AUDIO_MEDIA, ResourceOwner.MAINUNIT);
                this.finish();
            }
            CarPlayDSIManager.this.carPlayDSIController.requestModeChange(new IDSIAppState[0], new IDSIResource[]{new DSIResource.Builder().setpOwner(1).setpResourceId(3).setDsiResourceId(1).setDsiResourceOwner(1).setDsiTakeType(1).setDsiTransferPriority(1).setDsiTakeConstraint(2).setDsiBorrowConstraint(1).setDsiUnborrowConstraint(0).build()}, "Audio Teardown");
            this.stateHandler.setOwnerForResource(Resource.AUDIO_MEDIA, ResourceOwner.MAINUNIT);
            this.finish();
        }
    }

    private class RequestModeChangeCarPlay
    extends AbstractDSICommand {
        private static final String LOGCLASS = "RequestModeChangeCarPlay";
        private final ITMState newState;
        private final IStateHandler stateHandler;

        public RequestModeChangeCarPlay(IContext context, ITMState pNewState, IStateHandler pStateHandler) {
            super(context.getLogger().main(), LOGCLASS, context, context.getSmartphoneDSIManager());
            this.newState = pNewState;
            this.stateHandler = pStateHandler;
        }

        public void execute() {
            this.logger.log(1000000, "### CMD ### [%1]", (Object)LOGCLASS);
            TMDevice activeDevice = this.context.getDeviceManager().getActiveDevice();
            if (null == activeDevice || activeDevice.connectionState().is(TMDevice.ConnectionState.NOT_ATTACHED)) {
                this.stateHandler.updateState(this.newState);
                this.finish();
            }
            this.requestModeChangeCP(this.newState, "user action");
            this.stateHandler.updateState(this.newState);
            this.finish();
        }

        private void requestModeChangeCP(ITMState pState, String reason) {
            CarPlayDSIManager.this.updateActiveApplications(pState);
            if (TerminalModeUtils.equalsResourcesOwner(CarPlayDSIManager.this.currentState.getResourceOwner(), pState.getResourceOwner())) {
                CarPlayDSIManager.this.carPlayDSIController.requestModeChange(CarPlayDSIManager.this.getAppStates(pState), new IDSIResource[0], reason);
                return;
            }
            if (CarPlayDSIManager.this.currentState.isResourceOwnerMainUnit(Resource.SCREEN) && pState.isResourceOwnerDevice(Resource.SCREEN)) {
                if (this.context.getConfiguration().applicationOffset() > 0) {
                    CarPlayDSIManager.this.carPlayDSIController.requestModeChange(CarPlayDSIManager.this.getAppStates(pState), CarPlayDSIManager.this.filterAudioResources(CarPlayDSIManager.this.getResources(pState, false), false), reason);
                }
                if (!((Boolean)CarPlayDSIManager.this.smartphoneProperties.getDisplayIsOff().get()).booleanValue()) {
                    SPIBoolean hfpCallActive = (SPIBoolean)CarPlayDSIManager.this.smartphoneProperties.getPropertyMUHFPPhonecallActive().get();
                    if (!hfpCallActive.isInitializing() && hfpCallActive.getBoolean()) {
                        CarPlayDSIManager.this.carPlayDSIController.requestModeChange(new IDSIAppState[0], new IDSIResource[]{TerminalModeUtils.getUnborrowScreen()}, "mainunit call active and screen entered");
                    }
                    if (CarPlayDSIManager.this.screenVisibleAfterPoup) {
                        CarPlayDSIManager.this.carPlayDSIController.requestModeChange(new IDSIAppState[0], new IDSIResource[]{TerminalModeUtils.getUnborrowScreen()}, "Screen Setup");
                        CarPlayDSIManager.this.screenVisibleAfterPoup = false;
                        CarPlayDSIManager.this.MUScreenBorrowRequestSend = false;
                        return;
                    }
                    CarPlayDSIManager.this.requestUI(ScreenId.SCREENID_CARPLAY);
                }
                if (CarPlayDSIManager.this.currentState.getOwnerForResource(Resource.AUDIO_MEDIA) == pState.getOwnerForResource(Resource.AUDIO_MEDIA)) {
                    return;
                }
            }
            if (CarPlayDSIManager.this.currentState.isResourceStatePausedByMute(Resource.AUDIO_MEDIA) && pState.isResourceOwnerMainUnit(Resource.AUDIO_MEDIA)) {
                return;
            }
            CarPlayDSIManager.this.waitForAudioType = pState.getOwnerForResource(Resource.AUDIO_MEDIA).isNot(ResourceOwner.MAINUNIT) && pState.getStateForResource(Resource.AUDIO_MEDIA).is(ResourceState.NORMAL);
            IDSIResource[] filteredAudioResources = CarPlayDSIManager.this.filterAudioResources(CarPlayDSIManager.this.getResources(pState, true));
            if (CarPlayDSIManager.this.currentState.getOwnerForResource(Resource.AUDIO_MEDIA) == pState.getOwnerForResource(Resource.AUDIO_MEDIA) && CarPlayDSIManager.this.currentState.getStateForResource(Resource.AUDIO_MEDIA) == CarPlayDSIManager.this.currentState.getStateForResource(Resource.AUDIO_MEDIA)) {
                if (CarPlayDSIManager.this.screenHiddenByPopup) {
                    CarPlayDSIManager.this.carPlayDSIController.requestModeChange(new IDSIAppState[0], new IDSIResource[]{TerminalModeUtils.getBorrowScreenNotification()}, "Screen teardown");
                    CarPlayDSIManager.this.screenHiddenByPopup = false;
                    CarPlayDSIManager.this.MUScreenBorrowRequestSend = true;
                    return;
                }
                filteredAudioResources = new IDSIResource[]{filteredAudioResources[0]};
            }
            CarPlayDSIManager.this.carPlayDSIController.requestModeChange(CarPlayDSIManager.this.getAppStates(pState), filteredAudioResources, reason);
        }
    }
}
