#pragma once

#include <JuceHeader.h>
#include <memory>
#include <cstdint>
#include <array>

#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace vlcplayer
{
struct Api
{
    using Instance = void;
    using Media = void;
    using Player = void;

    HMODULE module = nullptr;
    Instance* instance = nullptr;

    Instance* (__cdecl *newInstance)(int, const char* const*) = nullptr;
    void (__cdecl *releaseInstance)(Instance*) = nullptr;
    Media* (__cdecl *newPath)(Instance*, const char*) = nullptr;
    void (__cdecl *releaseMedia)(Media*) = nullptr;
    Player* (__cdecl *newPlayer)(Instance*) = nullptr;
    void (__cdecl *releasePlayer)(Player*) = nullptr;
    void (__cdecl *setMedia)(Player*, Media*) = nullptr;
    void (__cdecl *setHwnd)(Player*, void*) = nullptr;
    int (__cdecl *play)(Player*) = nullptr;
    void (__cdecl *stop)(Player*) = nullptr;
    void (__cdecl *setPause)(Player*, int) = nullptr;
    int (__cdecl *isPlaying)(Player*) = nullptr;
    std::int64_t (__cdecl *getTime)(Player*) = nullptr;
    std::int64_t (__cdecl *getLength)(Player*) = nullptr;
    void (__cdecl *setTime)(Player*, std::int64_t) = nullptr;
    void (__cdecl *setVolume)(Player*, int) = nullptr;
    void (__cdecl *setMute)(Player*, int) = nullptr;

    Api()
    {
        auto base = juce::File::getSpecialLocation(juce::File::currentApplicationFile)
                        .getParentDirectory().getChildFile("VLC");
        if (!base.isDirectory())
            base = juce::File::getSpecialLocation(juce::File::currentApplicationFile).getParentDirectory();

        auto plugins = base.getChildFile("plugins");
        SetDllDirectoryW(base.getFullPathName().toWideCharPointer());
        if (plugins.isDirectory())
            SetEnvironmentVariableW(L"VLC_PLUGIN_PATH", plugins.getFullPathName().toWideCharPointer());

        module = LoadLibraryW(base.getChildFile("libvlc.dll").getFullPathName().toWideCharPointer());
        if (!module)
            return;

#define VLC_LOAD(member, symbol) member = reinterpret_cast<decltype(member)>(GetProcAddress(module, symbol))
        VLC_LOAD(newInstance, "libvlc_new");
        VLC_LOAD(releaseInstance, "libvlc_release");
        VLC_LOAD(newPath, "libvlc_media_new_path");
        VLC_LOAD(releaseMedia, "libvlc_media_release");
        VLC_LOAD(newPlayer, "libvlc_media_player_new");
        VLC_LOAD(releasePlayer, "libvlc_media_player_release");
        VLC_LOAD(setMedia, "libvlc_media_player_set_media");
        VLC_LOAD(setHwnd, "libvlc_media_player_set_hwnd");
        VLC_LOAD(play, "libvlc_media_player_play");
        VLC_LOAD(stop, "libvlc_media_player_stop");
        VLC_LOAD(setPause, "libvlc_media_player_set_pause");
        VLC_LOAD(isPlaying, "libvlc_media_player_is_playing");
        VLC_LOAD(getTime, "libvlc_media_player_get_time");
        VLC_LOAD(getLength, "libvlc_media_player_get_length");
        VLC_LOAD(setTime, "libvlc_media_player_set_time");
        VLC_LOAD(setVolume, "libvlc_audio_set_volume");
        VLC_LOAD(setMute, "libvlc_audio_set_mute");
#undef VLC_LOAD

        if (!newInstance || !releaseInstance || !newPath || !releaseMedia ||
            !newPlayer || !releasePlayer || !setMedia || !setHwnd || !play ||
            !stop || !setPause || !isPlaying || !getTime || !getLength ||
            !setTime || !setVolume || !setMute)
            return;

        const char* args[] = { "--no-video-title-show", "--quiet", "--no-snapshot-preview" };
        instance = newInstance(3, args);
    }

    ~Api()
    {
        if (instance && releaseInstance)
            releaseInstance(instance);
        if (module)
            FreeLibrary(module);
    }

    bool ready() const { return module && instance; }

    static Api& get()
    {
        static Api api;
        return api;
    }
};

class VideoView final : public juce::Component, private juce::Timer
{
public:
    VideoView()
    {
        setOpaque(true);
        player = Api::get().ready() ? Api::get().newPlayer(Api::get().instance) : nullptr;
        startTimerHz(60);
    }

    ~VideoView() override
    {
        stopTimer();
        stop();
        if (media && Api::get().releaseMedia)
            Api::get().releaseMedia(media);
        media = nullptr;
        if (player && Api::get().releasePlayer)
            Api::get().releasePlayer(player);
        player = nullptr;
        destroyVideoWindow();
    }

    bool isAvailable() const
    {
        return player != nullptr;
    }

    juce::String errorMessage() const
    {
        if (!Api::get().module)
            return "LibVLC introuvable. Le dossier VLC doit être à côté du programme.";
        if (!Api::get().instance)
            return "Impossible d'initialiser LibVLC. Vérifie le dossier VLC et ses greffons.";
        if (!player)
            return "Impossible de créer le lecteur d'aperçu LibVLC.";
        return {};
    }

    bool load(const juce::File& file)
    {
        if (!file.existsAsFile() || !player)
            return false;

        stop();
        if (media)
        {
            Api::get().releaseMedia(media);
            media = nullptr;
        }

        auto utf8 = file.getFullPathName().toUTF8();
        media = Api::get().newPath(Api::get().instance, utf8.getAddress());
        if (!media)
            return false;

        Api::get().setMedia(player, media);
        loadedFile = file;
        pendingSeek = false;
        pendingSeekMs = 0;
        pendingPlay = false;
        ensureWindows();
        return true;
    }

    void unload()
    {
        stop();
        if (media)
        {
            Api::get().releaseMedia(media);
            media = nullptr;
        }
        loadedFile = {};
        pendingSeek = false;
        pendingPlay = false;
    }

    void play()
    {
        if (!player || !media)
            return;
        if (!ensureWindows())
        {
            pendingPlay = true;
            return;
        }
        if (videoHwnd && IsWindow(videoHwnd))
            Api::get().setHwnd(player, videoHwnd);
        lastPlayResult = Api::get().play(player);
        pendingPlay = false;
        if (pendingSeek)
            seekWhenPlaying = true;
    }

    void pause()
    {
        if (player && media)
            Api::get().setPause(player, 1);
    }

    void stop()
    {
        if (player && media)
            Api::get().stop(player);
        seekWhenPlaying = false;
        pendingSeek = false;
        pendingPlay = false;
    }

    bool isPlaying() const
    {
        return player && media && Api::get().isPlaying(player) != 0;
    }

    juce::String playbackDiagnostic() const
    {
        if (!player)
            return errorMessage();
        if (!media || !loadedFile.existsAsFile())
            return "Aucun média chargé dans l'aperçu.";
        if (!videoHwnd || !IsWindow(videoHwnd))
            return "Fenêtre vidéo Windows non créée.";
        if (lastPlayResult < 0)
            return "LibVLC a refusé le démarrage (code -1).";
        return isPlaying() ? "Aperçu VLC : lecture active."
                           : "Aperçu VLC chargé, en attente de lecture.";
    }

    double getPlayPosition() const
    {
        return player && media ? (double) Api::get().getTime(player) / 1000.0 : 0.0;
    }

    double getVideoDuration() const
    {
        return player && media ? (double) Api::get().getLength(player) / 1000.0 : 0.0;
    }

    void setPlayPosition(double seconds)
    {
        pendingSeekMs = (std::int64_t) (juce::jmax(0.0, seconds) * 1000.0);
        if (player && media && isPlaying())
        {
            Api::get().setTime(player, pendingSeekMs);
            pendingSeek = false;
            seekWhenPlaying = false;
        }
        else
        {
            pendingSeek = true;
        }
    }

    void setMuted(bool muted)
    {
        if (player)
            Api::get().setMute(player, muted ? 1 : 0);
    }

    void setVolume(int volume)
    {
        if (player)
            Api::get().setVolume(player, juce::jlimit(0, 100, volume));
    }

    void setBlackoutAlpha(int) {}

    void setOutputVisible(bool visible)
    {
        outputVisible = visible;
        if (videoHwnd && IsWindow(videoHwnd))
        {
            positionWindows();
            ShowWindow(videoHwnd, visible ? SW_SHOWNA : SW_HIDE);
            SetWindowPos(videoHwnd, visible ? HWND_TOP : HWND_BOTTOM, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }

    void resized() override
    {
        if (ensureWindows())
            positionWindows();
    }

    void parentHierarchyChanged() override
    {
        if (ensureWindows())
            positionWindows();
    }

private:
    void positionWindows()
    {
        if (!parentHwnd || !videoHwnd || !IsWindow(parentHwnd) || !IsWindow(videoHwnd))
            return;
        auto screen = getScreenBounds();
        POINT p{ screen.getX(), screen.getY() };
        ScreenToClient(parentHwnd, &p);
        SetWindowPos(videoHwnd, HWND_TOP, p.x, p.y,
                     juce::jmax(1, screen.getWidth()),
                     juce::jmax(1, screen.getHeight()),
                     SWP_NOACTIVATE);
    }

    void destroyVideoWindow()
    {
        if (videoHwnd && IsWindow(videoHwnd))
            DestroyWindow(videoHwnd);
        videoHwnd = nullptr;
        parentHwnd = nullptr;
    }
    void timerCallback() override
    {
        if ((!videoHwnd || !IsWindow(videoHwnd)) && isShowing())
            ensureWindows();

        if (pendingPlay && ensureWindows())
            play();

        if (seekWhenPlaying && player && isPlaying())
        {
            Api::get().setTime(player, pendingSeekMs);
            pendingSeek = false;
            seekWhenPlaying = false;
        }
    }

    bool ensureWindows()
    {
        auto* topLevel = getTopLevelComponent();
        auto* peer = topLevel != nullptr ? topLevel->getPeer() : nullptr;
        if (!peer)
            return false;

        auto parent = (HWND) peer->getNativeHandle();
        if (!parent || !IsWindow(parent))
            return false;

        if (parentHwnd != parent || !videoHwnd || !IsWindow(videoHwnd))
        {
            destroyVideoWindow();
            parentHwnd = parent;
            videoHwnd = CreateWindowExW(0, L"STATIC", L"",
                                        WS_CHILD | WS_CLIPSIBLINGS,
                                        0, 0, 1, 1, parentHwnd, nullptr,
                                        GetModuleHandleW(nullptr), nullptr);
            if (!videoHwnd)
                return false;
            if (player && media)
                Api::get().setHwnd(player, videoHwnd);
            positionWindows();
            ShowWindow(videoHwnd, outputVisible ? SW_SHOWNA : SW_HIDE);
        }

        if (player && media)
            Api::get().setHwnd(player, videoHwnd);
        return videoHwnd != nullptr && IsWindow(videoHwnd);
    }

    Api::Player* player = nullptr;
    Api::Media* media = nullptr;
    HWND parentHwnd = nullptr;
    HWND videoHwnd = nullptr;
    juce::File loadedFile;
    std::int64_t pendingSeekMs = 0;
    int lastPlayResult = 0;
    bool pendingSeek = false;
    bool seekWhenPlaying = false;
    bool pendingPlay = false;
    bool outputVisible = true;
};
}
#else
namespace vlcplayer
{
class VideoView final : public juce::Component
{
public:
    bool isAvailable() const { return false; }
    juce::String errorMessage() const { return "Ce lecteur vidéo est actuellement prévu pour Windows."; }
    juce::String playbackDiagnostic() const { return errorMessage(); }
    bool load(const juce::File&) { return false; }
    void unload() {}
    void play() {}
    void trigger(double) {}
    void pause() {}
    void stop() {}
    bool isPlaying() const { return false; }
    double getPlayPosition() const { return 0.0; }
    double getVideoDuration() const { return 0.0; }
    void setPlayPosition(double) {}
    void setMuted(bool) {}
    void setVolume(int) {}
    void setBlackoutAlpha(int) {}
    void setOutputVisible(bool) {}
};
}
#endif
