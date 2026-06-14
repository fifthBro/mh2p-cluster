/**
 * OSGi Bundle Activator for Android Auto Cluster Integration.
 *
 * Uses ServiceTracker to monitor:
 * - CombiBAPServiceNavi (for cluster output)
 * - LogChannel (for logging)
 * - ISysServices (for distance/speed units via units().distance/speed())
 * - ICarCoreServices (for LHD/RHD via configuration().exteriorLight().leftHandTraffic())
 *
 * Uses reflection to dynamically register with AndroidAuto2ListenerDistributor
 * to avoid compile-time dependencies.
 */
 
package de.audi.app.terminalmode.smartphone.androidauto2.nav;

import org.osgi.framework.BundleActivator;
import org.osgi.framework.BundleContext;
import org.osgi.framework.ServiceReference;
import org.osgi.util.tracker.ServiceTracker;
import org.osgi.util.tracker.ServiceTrackerCustomizer;

import de.audi.atip.interapp.combi.bap.navi.CombiBAPServiceNavi;
import de.audi.atip.log.LogChannel;
import de.audi.mib.system.ISysServices;
import de.audi.app.car.api.core.ICarCoreServices;
import de.audi.tghu.navi.app.cluster.IMapClusterService;

import java.lang.reflect.Method;

public class AndroidAutoClusterActivator implements BundleActivator {

    private BundleContext context;
    private ServiceTracker clusterServiceTracker;
    private ServiceTracker loggerTracker;
    private ServiceTracker sysServicesTracker;
    private ServiceTracker carCoreServicesTracker;
    private ServiceTracker mapClusterServiceTracker;

    private AndroidAutoClusterIntegration clusterIntegration;
    private ClusterMapController mapController;
    private CombiBAPServiceNavi clusterService;
    private LogChannel logger;
    private boolean listenerRegistered = false;

    public void start(BundleContext context) throws Exception {
        this.context = context;
        this.clusterIntegration = new AndroidAutoClusterIntegration();
        this.mapController = new ClusterMapController();

        // Track LogChannel first
        loggerTracker = new ServiceTracker(context, LogChannel.class.getName(), new LoggerCustomizer());
        loggerTracker.open();

        // Track CombiBAPServiceNavi
        clusterServiceTracker = new ServiceTracker(context, CombiBAPServiceNavi.class.getName(), new ClusterServiceCustomizer());
        clusterServiceTracker.open();

        // Track ISysServices for unit detection
        sysServicesTracker = new ServiceTracker(context, ISysServices.class.getName(), new SysServicesCustomizer());
        sysServicesTracker.open();

        // Track ICarCoreServices for drive side detection
        carCoreServicesTracker = new ServiceTracker(context, ICarCoreServices.class.getName(), new CarCoreServicesCustomizer());
        carCoreServicesTracker.open();

        // Track IMapClusterService for navigation map control
        mapClusterServiceTracker = new ServiceTracker(context, IMapClusterService.class.getName(), new MapClusterServiceCustomizer());
        mapClusterServiceTracker.open();

        // Try to register with Android Auto listener distributor
        tryRegisterWithDistributor();
    }

    public void stop(BundleContext context) throws Exception {
        // Unregister listener if registered
        if (listenerRegistered) {
            tryUnregisterFromDistributor();
        }

        // Dispose map controller (restores navigation map)
        if (mapController != null) {
            mapController.dispose();
            mapController = null;
        }

        // Close service trackers
        if (loggerTracker != null) {
            loggerTracker.close();
            loggerTracker = null;
        }

        if (clusterServiceTracker != null) {
            clusterServiceTracker.close();
            clusterServiceTracker = null;
        }

        if (sysServicesTracker != null) {
            sysServicesTracker.close();
            sysServicesTracker = null;
        }

        if (carCoreServicesTracker != null) {
            carCoreServicesTracker.close();
            carCoreServicesTracker = null;
        }

        if (mapClusterServiceTracker != null) {
            mapClusterServiceTracker.close();
            mapClusterServiceTracker = null;
        }

        this.clusterIntegration = null;
        this.context = null;
    }

    /**
     * Attempts to register with Android Auto listener distributor using reflection.
     * This avoids compile-time dependency on AndroidAuto2ListenerDistributor.
     */
    private synchronized void tryRegisterWithDistributor() {
        if (clusterIntegration == null || listenerRegistered) {
            return;
        }

        try {
            // Look for AndroidAuto2ListenerDistributor service using string name
            String distributorClassName = "de.audi.app.terminalmode.smartphone.androidauto2.AndroidAuto2ListenerDistributor";
            ServiceReference[] refs = context.getServiceReferences(distributorClassName, null);

            if (refs != null && refs.length > 0) {
                Object distributorService = context.getService(refs[0]);

                if (distributorService != null) {
                    // Use reflection to call addListener method
                    Class distributorClass = distributorService.getClass();
                    Method addListenerMethod = distributorClass.getMethod("addListener", new Class[]{Object.class});
                    addListenerMethod.invoke(distributorService, new Object[]{clusterIntegration});

                    listenerRegistered = true;

                    if (logger != null) {
                        logger.log(1000000, "[AndroidAutoClusterActivator] Successfully registered with listener distributor via reflection");
                    }

                    context.ungetService(refs[0]);
                }
            } else if (logger != null) {
                logger.log(1000000, "[AndroidAutoClusterActivator] AndroidAuto2ListenerDistributor service not found - will retry");
            }
        } catch (Exception e) {
            if (logger != null) {
                logger.log(100000, "[AndroidAutoClusterActivator] Failed to register via reflection: " + e.toString());
            }
        }
    }

    /**
     * Attempts to unregister from Android Auto listener distributor using reflection.
     */
    private synchronized void tryUnregisterFromDistributor() {
        if (clusterIntegration == null || !listenerRegistered) {
            return;
        }

        try {
            String distributorClassName = "de.audi.app.terminalmode.smartphone.androidauto2.AndroidAuto2ListenerDistributor";
            ServiceReference[] refs = context.getServiceReferences(distributorClassName, null);

            if (refs != null && refs.length > 0) {
                Object distributorService = context.getService(refs[0]);

                if (distributorService != null) {
                    Class distributorClass = distributorService.getClass();
                    Method removeListenerMethod = distributorClass.getMethod("removeListener", new Class[]{Object.class});
                    removeListenerMethod.invoke(distributorService, new Object[]{clusterIntegration});

                    listenerRegistered = false;

                    if (logger != null) {
                        logger.log(1000000, "[AndroidAutoClusterActivator] Unregistered from listener distributor");
                    }

                    context.ungetService(refs[0]);
                }
            }
        } catch (Exception e) {
            if (logger != null) {
                logger.log(100000, "[AndroidAutoClusterActivator] Failed to unregister: " + e.toString());
            }
        }
    }

    /**
     * ServiceTracker customizer for LogChannel service.
     */
    private class LoggerCustomizer implements ServiceTrackerCustomizer {
        public Object addingService(ServiceReference reference) {
            LogChannel service = (LogChannel) context.getService(reference);
            logger = service;

            if (clusterIntegration != null) {
                clusterIntegration.setLogger();
            }

            if (logger != null) {
                logger.log(1000000, "[AndroidAutoClusterActivator] Logger service acquired");
            }

            // Retry registration now that we have logger
            tryRegisterWithDistributor();

            return service;
        }

        public void modifiedService(ServiceReference reference, Object service) {
            // Not used
        }

        public void removedService(ServiceReference reference, Object service) {
            logger = null;
            if (clusterIntegration != null) {
                clusterIntegration.setLogger();
            }
            context.ungetService(reference);
        }
    }

    /**
     * ServiceTracker customizer for CombiBAPServiceNavi service.
     */
    private class ClusterServiceCustomizer implements ServiceTrackerCustomizer {
        public Object addingService(ServiceReference reference) {
            CombiBAPServiceNavi service = (CombiBAPServiceNavi) context.getService(reference);
            clusterService = service;

            if (clusterIntegration != null) {
                clusterIntegration.setClusterService(service);
            }

            if (logger != null) {
                logger.log(1000000, "[AndroidAutoClusterActivator] Cluster service acquired");
            }

            return service;
        }

        public void modifiedService(ServiceReference reference, Object service) {
            // Not used
        }

        public void removedService(ServiceReference reference, Object service) {
            if (clusterIntegration != null) {
                clusterIntegration.unsetClusterService((CombiBAPServiceNavi) service);
            }

            clusterService = null;

            if (logger != null) {
                logger.log(1000000, "[AndroidAutoClusterActivator] Cluster service removed");
            }

            context.ungetService(reference);
        }
    }

    /**
     * ServiceTracker customizer for ISysServices.
     */
    private class SysServicesCustomizer implements ServiceTrackerCustomizer {
        public Object addingService(ServiceReference reference) {
            ISysServices service = (ISysServices) context.getService(reference);

            if (clusterIntegration != null) {
                clusterIntegration.setSysServices(service);
            }

            if (logger != null) {
                logger.log(1000000, "[AndroidAutoClusterActivator] ISysServices acquired");
            }

            return service;
        }

        public void modifiedService(ServiceReference reference, Object service) {
            // Not used
        }

        public void removedService(ServiceReference reference, Object service) {
            if (clusterIntegration != null) {
                clusterIntegration.setSysServices(null);
            }

            if (logger != null) {
                logger.log(1000000, "[AndroidAutoClusterActivator] ISysServices removed");
            }

            context.ungetService(reference);
        }
    }

    /**
     * ServiceTracker customizer for ICarCoreServices.
     */
    private class CarCoreServicesCustomizer implements ServiceTrackerCustomizer {
        public Object addingService(ServiceReference reference) {
            ICarCoreServices service = (ICarCoreServices) context.getService(reference);

            if (clusterIntegration != null) {
                clusterIntegration.setCarCoreServices(service);
            }

            if (logger != null) {
                logger.log(1000000, "[AndroidAutoClusterActivator] ICarCoreServices acquired");
            }

            return service;
        }

        public void modifiedService(ServiceReference reference, Object service) {
            // Not used
        }

        public void removedService(ServiceReference reference, Object service) {
            if (clusterIntegration != null) {
                clusterIntegration.setCarCoreServices(null);
            }

            if (logger != null) {
                logger.log(1000000, "[AndroidAutoClusterActivator] ICarCoreServices removed");
            }

            context.ungetService(reference);
        }
    }

    /**
     * ServiceTracker customizer for IMapClusterService.
     */
    private class MapClusterServiceCustomizer implements ServiceTrackerCustomizer {
        public Object addingService(ServiceReference reference) {
            IMapClusterService service = (IMapClusterService) context.getService(reference);

            if (mapController != null) {
                mapController.setMapClusterService(service);
            }

            if (clusterIntegration != null) {
                clusterIntegration.setClusterMapController(mapController);
            }

            if (logger != null) {
                logger.log(1000000, "[AndroidAutoClusterActivator] IMapClusterService acquired - map control ready");
            }

            return service;
        }

        public void modifiedService(ServiceReference reference, Object service) {
            // Not used
        }

        public void removedService(ServiceReference reference, Object service) {
            if (mapController != null) {
                mapController.setMapClusterService(null);
            }

            if (logger != null) {
                logger.log(1000000, "[AndroidAutoClusterActivator] IMapClusterService removed");
            }

            context.ungetService(reference);
        }
    }
}
