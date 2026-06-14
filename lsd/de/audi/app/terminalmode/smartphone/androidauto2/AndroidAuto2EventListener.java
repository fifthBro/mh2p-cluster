/*
 * Copyright (c) 2026 fifthBro
 * https://fifthbro.github.io
 *
 * Licensed under CC BY-NC-SA 4.0
 * https://creativecommons.org/licenses/by-nc-sa/4.0/
 * NOT FOR COMMERCIAL USE
 */

/**
 * Event listener implementation for Android Auto v2 DSI callbacks.
 * 
 * Handles real-time Android Auto events and delegates navigation-related
 * events to the cluster integration for processing and display. Provides
 * the bridge between Android Auto's event system and Audi's cluster
 * display infrastructure.
 * 
 * Events processed include navigation updates, route changes, and other
 * Android Auto state changes that require cluster display updates.
 */
package de.audi.app.terminalmode.smartphone.androidauto2;

import de.audi.app.terminalmode.dsi.androidauto2.DSIAndroidAuto2DefaultListenerSafe;
import de.audi.app.terminalmode.dsi.androidauto2.DSIAndroidAuto2Safe;
import de.audi.app.terminalmode.events.IEventBus;
import de.audi.app.terminalmode.events.PlayPositionEvent;
import de.audi.app.terminalmode.events.PlayPositionLogEvent;
import de.audi.app.terminalmode.events.PlayPositionLogger;
import de.audi.app.terminalmode.events.PlayPositionLoggerConfiguration;
import de.audi.app.terminalmode.events.PlaybackInfoChangedEvent;
import de.audi.app.terminalmode.events.TrackDataChangedEvent;
import de.audi.app.terminalmode.events.TrackPlayPositionEvent;
import de.audi.app.terminalmode.smartphone.ISmartphoneProperties;
import de.audi.atip.log.LogChannel;
import org.dsi.ifc.androidauto2.PlaybackInfo;
import org.dsi.ifc.androidauto2.TrackData;
import org.dsi.ifc.global.ResourceLocator;

public final class AndroidAuto2EventListener
extends DSIAndroidAuto2DefaultListenerSafe {
    private static final String LOGCLASS = "AndroidAuto2EventListener";
    private final IEventBus eventBus;
    private final LogChannel lc;
    private volatile TrackPlayPositionEvent notifiedPlayPosition;
    private final PlayPositionLogger playPositionLogger;
    private final ISmartphoneProperties smartphoneProperties;

    public AndroidAuto2EventListener(IEventBus iEventBus, LogChannel logChannel, DSIAndroidAuto2Safe dSIAndroidAuto2Safe, ISmartphoneProperties iSmartphoneProperties) {
        this.eventBus = iEventBus;
        this.smartphoneProperties = iSmartphoneProperties;
        this.lc = logChannel;
        dSIAndroidAuto2Safe.setNotification(new int[]{3, 5, 6, 7, 8}, null);
        this.playPositionLogger = new PlayPositionLogger(new PlayPositionLoggerConfiguration().setPlayPositionLogEvent(PlayPositionEvent.STARTUP, new PlayPositionLogEvent(logChannel, 1000000, 3)).setPlayPositionLogEvent(PlayPositionEvent.PLAYBACK_STATE_CHANGED, new PlayPositionLogEvent(logChannel, 1000000, 2)).setPlayPositionLogEvent(PlayPositionEvent.TRACK_CHANGED, new PlayPositionLogEvent(logChannel, 1000000, 3)).setPlayPositionLogEvent(PlayPositionEvent.ILLEGAL_PLAYPOSITION, new PlayPositionLogEvent(logChannel, 10000, 3)).setPlayPositionLogEvent(PlayPositionEvent.TO_LATE_PLAYPOSITION, new PlayPositionLogEvent(logChannel, 10000, 3)));
    }

    public void asyncException(int n, String string, int n2) {
        this.lc.log(10000, "<- [%1.asyncException] errCode='%2' msg='%3' requestType='%4'", (Object)LOGCLASS, (Object)String.valueOf(n), (Object)String.valueOf(string), (Object)String.valueOf(n2));
    }

    public void updateNowPlayingData(TrackData trackData, int n) {
        if (this.isValid(n)) {
            TrackDataChangedEvent trackDataChangedEvent = new TrackDataChangedEvent.Builder().setAlbum(trackData.getAlbum()).setArtist(trackData.getArtist()).setComposer(trackData.getComposer()).setDuration(trackData.getDuration()).setGenre(trackData.getGenre()).setTitle(trackData.getTitle()).build();
            this.lc.log(1000000, "<- [%1.updateNowPlayingData] %2", (Object)LOGCLASS, (Object)trackDataChangedEvent);
            this.playPositionLogger.trackChanged();
            this.smartphoneProperties.getPropertyTrackData().accept((Object)trackDataChangedEvent);
            this.notifyPlayPosition(new TrackPlayPositionEvent(0, trackData.getDuration() / 1000));
        }
    }

    public void updatePlaybackState(PlaybackInfo playbackInfo, int n) {
        if (this.isValid(n)) {
            PlaybackInfoChangedEvent playbackInfoChangedEvent = new PlaybackInfoChangedEvent.Builder().setPlaybackState(this.convert(playbackInfo.getStatus())).build();
            this.lc.log(1000000, "<- [%1.updateNowPlayingData] %2", (Object)LOGCLASS, (Object)playbackInfoChangedEvent);
            this.playPositionLogger.playbackStateChanged();
        }
    }

    private PlaybackInfoChangedEvent.PlaybackState convert(int n) {
        switch (n) {
            case 2: {
                return PlaybackInfoChangedEvent.PlaybackState.PAUSED;
            }
            case 1: {
                return PlaybackInfoChangedEvent.PlaybackState.PLAYING;
            }
            case 4: {
                return PlaybackInfoChangedEvent.PlaybackState.SEEKBACKWARD;
            }
            case 3: {
                return PlaybackInfoChangedEvent.PlaybackState.SEEKBACKWARD;
            }
        }
        return PlaybackInfoChangedEvent.PlaybackState.STOPPED;
    }

    private void notifyPlayPosition(TrackPlayPositionEvent trackPlayPositionEvent) {
        if (trackPlayPositionEvent.equals((Object)this.notifiedPlayPosition)) {
            return;
        }
        this.notifiedPlayPosition = trackPlayPositionEvent;
        this.smartphoneProperties.getPropertyPlayPosition().accept((Object)trackPlayPositionEvent);
    }

    public void updatePlayposition(int n, int n2) {
        if (this.isValid(n2)) {
            if (this.notifiedPlayPosition == null) {
                this.lc.log(10000, "<- [%1.updatePlayposition] value:%2 - no track / totaltime! updateNowPlayingData has to be called before with valid total time!", (Object)LOGCLASS, (long)n);
                return;
            }
            TrackPlayPositionEvent trackPlayPositionEvent = new TrackPlayPositionEvent(n / 1000, this.notifiedPlayPosition.getTotalTimeOfTrack());
            this.playPositionLogger.updatePlayPosition(trackPlayPositionEvent);
            this.notifyPlayPosition(trackPlayPositionEvent);
        }
    }

    public void updateCoverArtUrl(ResourceLocator resourceLocator, int n) {
        if (this.isValid(n)) {
            this.lc.log(1000000, "<- [%1.updateCoverArtUrl] %2 ", (Object)LOGCLASS, (Object)resourceLocator);
            resourceLocator = resourceLocator == null ? new ResourceLocator("") : resourceLocator;
            this.smartphoneProperties.getCoverArt().accept((Object)resourceLocator);
        }
    }

    private boolean isValid(int n) {
        return 1 == n;
    }
}
