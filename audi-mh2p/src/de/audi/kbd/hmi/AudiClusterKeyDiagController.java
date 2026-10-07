/*
 * Audi MH2P/FPK steering-wheel map selector contributed by tearfalls1987.
 * Licensed under the repository's CC BY-NC-SA 4.0 terms.
 * OEM API names are compile-time references; no OEM implementation is included.
 */
package de.audi.kbd.hmi;

import de.audi.atip.base.IFrameworkAccess;
import de.audi.atip.hmi.event.ATIPEvent;
import de.audi.atip.hmi.event.KeyEvent;
import de.audi.atip.hmi.event.WheelButtonEvent;
import de.audi.atip.keyhandling.IKeyEventDistributor;
import java.io.FileWriter;
import java.io.IOException;
import java.lang.reflect.Method;

/** Audi C8: hold the gated OK/menu key to select AA or native VC map. */
public final class AudiClusterKeyDiagController {
    private static final String LOG = "/tmp/cluster_keydiag.log";
    private static final int VC_NAV_SCREEN = 4000010;
    private static final long HOLD_MS = 1100L;
    private static boolean aaActive;
    private static boolean consumeMenuRelease;
    private static boolean nativeSelected;
    private static long menuPressAtMs;
    private static KeyEvent pendingMenuPress;

    private AudiClusterKeyDiagController() {
    }

    public static synchronized void onMirrorCommand(String command) {
        if (command == null) return;
        if (command.startsWith("prepare") || command.startsWith("start")
                || command.startsWith("resume")) aaActive = true;
        else if (command.startsWith("stop")) {
            aaActive = false;
            consumeMenuRelease = false;
            menuPressAtMs = 0L;
            pendingMenuPress = null;
            nativeSelected = false;
        }
    }

    private static int currentScreen(IFrameworkAccess framework) {
        try {
            return framework.getHMIService().getScreenManager(1).getCurrentConnectedScreenId();
        } catch (Throwable error) {
            log("screen lookup failed " + error.toString());
            return -1;
        }
    }

    private static boolean switchMap(IFrameworkAccess framework, boolean toNative) {
        try {
            Object service = framework.getHMIService().getDisplayManager();
            if (service == null) {
                log("toggle failed: DisplayManager unavailable");
                return false;
            }
            String name = toNative ? "restoreClusterMapFromMirror62" : "activateClusterMirror62";
            Method method = service.getClass().getMethod(name, new Class[0]);
            Object result = method.invoke(service, new Object[0]);
            boolean ok = result instanceof Boolean && ((Boolean)result).booleanValue();
            log("toggle " + (toNative ? "native" : "AA") + " result=" + ok);
            return ok;
        } catch (Throwable error) {
            log("toggle failed: " + error.toString());
            return false;
        }
    }

    private static void replayShortNativePress(IFrameworkAccess framework,
            int listenerTerminal, KeyEvent release) {
        KeyEvent press = pendingMenuPress;
        pendingMenuPress = null;
        if (press == null) {
            log("native short press missing buffered press; no replay");
            return;
        }
        try {
            IKeyEventDistributor distributor = framework.getHMITerminalRegistry()
                    .getKeyEventDistributor(listenerTerminal);
            if (distributor == null) {
                log("native short press distributor unavailable");
                return;
            }
            distributor.keyPressed(press);
            distributor.keyReleased(release);
            log("replayed native OK/menu short press on terminal=" + listenerTerminal);
        } catch (Throwable error) {
            log("native OK/menu short replay failed: " + error.toString());
        }
    }

    public static synchronized boolean handle(ATIPEvent event,
            IFrameworkAccess framework, int listenerTerminal) {
        if (!aaActive || !(event instanceof KeyEvent)) {
            if (!aaActive) {
                consumeMenuRelease = false;
                menuPressAtMs = 0L;
                pendingMenuPress = null;
            }
            return false;
        }
        KeyEvent key = (KeyEvent)event;
        if (key.getID() == 10403 && key.getKeyCode() == 20
                && event instanceof WheelButtonEvent && !nativeSelected
                && currentScreen(framework) == VC_NAV_SCREEN) {
            WheelButtonEvent wheel = (WheelButtonEvent)event;
            if (wheel.isVerticalDirection()) {
                log("suppressed native map-scale wheel direction=" + wheel.getDirection());
                return true;
            }
        }
        if (key.getKeyCode() != 30) return false;

        if (key.getID() == 10401) {
            if (consumeMenuRelease) return true; // key repeat must not reset hold time
            int screen = currentScreen(framework);
            consumeMenuRelease = (screen == VC_NAV_SCREEN);
            if (consumeMenuRelease) {
                menuPressAtMs = System.currentTimeMillis();
                pendingMenuPress = nativeSelected ? key : null;
                log("consumed OK/menu press on AA VC nav screen=" + screen
                        + " listenerTerminal=" + listenerTerminal);
                return true;
            }
            return false;
        }
        if (key.getID() == 10402 && consumeMenuRelease) {
            long elapsed = System.currentTimeMillis() - menuPressAtMs;
            consumeMenuRelease = false;
            menuPressAtMs = 0L;
            boolean stillNav = aaActive && currentScreen(framework) == VC_NAV_SCREEN;
            if (elapsed >= HOLD_MS && stillNav) {
                pendingMenuPress = null;
                boolean toNative = !nativeSelected;
                if (switchMap(framework, toNative)) nativeSelected = toNative;
                log("consumed OK/menu hold elapsedMs=" + elapsed
                        + " selected=" + (nativeSelected ? "native" : "AA"));
            } else if (nativeSelected && stillNav && elapsed >= 0L
                    && elapsed < HOLD_MS) {
                replayShortNativePress(framework, listenerTerminal, key);
            } else {
                pendingMenuPress = null;
                log("consumed OK/menu release elapsedMs=" + elapsed);
            }
            return true;
        }
        return false;
    }

    private static void log(String message) {
        FileWriter writer = null;
        try {
            writer = new FileWriter(LOG, true);
            writer.write(Long.toString(System.currentTimeMillis()));
            writer.write(" ");
            writer.write(message);
            writer.write('\n');
        } catch (IOException ignored) {
        } finally {
            if (writer != null) try { writer.close(); } catch (IOException ignored) { }
        }
    }
}
