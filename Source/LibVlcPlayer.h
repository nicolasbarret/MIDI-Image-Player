#pragma once

#include <JuceHeader.h>
#include <memory>
#include <cstdint>

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
        startTimerHz(120);
    }

    ~VideoView() override
    {
        stop();
        if (player && Api::get().releasePlayer)
            Api::get().releasePlayer(player);
        player = nullptr;
        destroyWindows();
    }

    bool isAvailable() const { return player != nullptr; }

    juce::String errorMessage() const
    {
        if (!Api::get().module)
            return "LibVLC introuvable. Le dossier VLC doit être à côté du programme.";
        if (!Api::get().instance)
            return "Impossible d'initialiser LibVLC. Vérifie le dossier VLC et ses greffons.";
        if (!player)
            return "Impossible de créer le lecteur LibVLC.";
        return {};
    }

    bool load(const juce::File& file)
    {
        if (!player || !file.existsAsFile())
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
        if (ensureWindows()) Api::get().setHwnd(player, videoHwnd);
        loadedFile = file;
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
        pendingPlay = false;
        pendingSeek = false;
        setBlackoutAlpha(255);
    }

    void play()
    {
        if (player && media)
        {
            if (!ensureWindows())
            {
                pendingPlay = true;
                return;
            }
            Api::get().setHwnd(player, videoHwnd);
            lastPlayResult = Api::get().play(player);
            pendingPlay = false;
            if (pendingSeek) applyPendingSeek();
        }
    }

    void pause()
    {
        if (player)
            Api::get().setPause(player, 1);
    }

    void stop()
    {
        if (player)
            Api::get().stop(player);
    }

    bool isPlaying() const
    {
        return player && Api::get().isPlaying(player) != 0;
    }

    juce::String playbackDiagnostic() const
    {
        if (!player) return errorMessage();
        if (!media) return "Aucun média associé au lecteur VLC.";
        if (!videoHwnd || !IsWindow(videoHwnd)) return windowDiagnostic.isNotEmpty() ? windowDiagnostic : "Fenêtre vidéo Windows non créée.";
        if (lastPlayResult < 0) return "LibVLC a refusé le démarrage (code -1).";
        if (isPlaying()) return "VLC : lecture active.";
        const auto ms = Api::get().getTime(player);
        const auto len = Api::get().getLength(player);
        if (len > 0 && ms >= len) return "VLC : vidéo arrivée à la fin.";
        return "VLC chargé, mais le moteur ne signale pas de lecture active.";
    }

    double getPlayPosition() const
    {
        return player ? (double) Api::get().getTime(player) / 1000.0 : 0.0;
    }

    double getVideoDuration() const
    {
        return player ? (double) Api::get().getLength(player) / 1000.0 : 0.0;
    }

    void setPlayPosition(double seconds)
    {
        if (!player || !media) return;
        pendingSeekMs = (std::int64_t)(juce::jmax(0.0, seconds) * 1000.0);
        pendingSeek = true;
        // A seek requested while paused is applied immediately after play() restarts
        // the decoder. Seeking a stopped/paused LibVLC player can delay the next frame.
        if (isPlaying())
            applyPendingSeek();
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

    void setBlackoutAlpha(int alpha)
    {
        blackoutAlpha = (BYTE) juce::jlimit(0, 255, alpha);
        if (blackoutHwnd && IsWindow(blackoutHwnd))
        {
            SetLayeredWindowAttributes(blackoutHwnd, RGB(0, 0, 0), blackoutAlpha, LWA_ALPHA);
            ShowWindow(blackoutHwnd, blackoutAlpha == 0 ? SW_HIDE : SW_SHOWNA);
            SetWindowPos(blackoutHwnd, HWND_TOP, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }

    void resized() override
    {
        ensureWindows();
        positionWindows();
    }

    void parentHierarchyChanged() override
    {
        ensureWindows();
        positionWindows();
    }

private:
    void timerCallback() override
    {
        // Retry after JUCE has created the native top-level window.
        if ((!videoHwnd || !IsWindow(videoHwnd)) && isShowing())
            ensureWindows();

        if (pendingPlay && ensureWindows())
        {
            Api::get().setHwnd(player, videoHwnd);
            lastPlayResult = Api::get().play(player);
            pendingPlay = false;
            if (pendingSeek)
                applyPendingSeek();
        }
    }

    void applyPendingSeek()
    {
        if (player && media && pendingSeek)
        {
            // Start/restart the player first, then apply the precise seek.
            // Avoid VLC's fast-keyframe mode: it was less consistent on rapid triggers.
            Api::get().setTime(player, pendingSeekMs);
            pendingSeek = false;
        }
    }

    bool ensureWindows()
    {
        auto* topLevel = getTopLevelComponent();
        auto* peer = topLevel != nullptr ? topLevel->getPeer() : nullptr;
        if (!peer)
        {
            windowDiagnostic = "Fenêtre vidéo : peer Windows absent.";
            return false;
        }
        auto parent = (HWND) peer->getNativeHandle();
        if (!parent || !IsWindow(parent))
        {
            windowDiagnostic = "Fenêtre vidéo : handle parent Windows invalide.";
            return false;
        }

        if (parentHwnd != parent || !videoHwnd || !IsWindow(videoHwnd))
        {
            destroyWindows();
            parentHwnd = parent;

            // La fenêtre de sortie JUCE est déjà une vraie fenêtre Windows.
            // On la donne directement à VLC : cela évite de créer un HWND enfant
            // supplémentaire, opération qui échouait sur certaines configurations.
            auto style = GetWindowLongPtrW(parentHwnd, GWL_STYLE);
            SetWindowLongPtrW(parentHwnd, GWL_STYLE, style | WS_CLIPCHILDREN);
            videoHwnd = parentHwnd;

            // Le calque noir est facultatif : son échec ne doit jamais bloquer la vidéo.
            blackoutHwnd = CreateWindowExW(WS_EX_LAYERED, L"STATIC", L"",
                                           WS_CHILD | WS_CLIPSIBLINGS,
                                           0, 0, 1, 1, parentHwnd, nullptr,
                                           GetModuleHandleW(nullptr), nullptr);
            if (!blackoutHwnd)
                windowDiagnostic = "VLC utilise la fenêtre de sortie. Le calque de fondu noir n'a pas pu être créé.";
            else
                windowDiagnostic.clear();

            if (player)
                Api::get().setHwnd(player, videoHwnd);
            setBlackoutAlpha(blackoutAlpha);
            positionWindows();
        }
        const bool valid = videoHwnd != nullptr && IsWindow(videoHwnd);
        if (!valid && windowDiagnostic.isEmpty())
            windowDiagnostic = "Fenêtre vidéo : handle enfant invalide.";
        return valid;
    }

    void positionWindows()
    {
        if (!parentHwnd || !blackoutHwnd || !IsWindow(blackoutHwnd))
            return;
        auto screen = getScreenBounds();
        POINT p{ screen.getX(), screen.getY() };
        ScreenToClient(parentHwnd, &p);
        int w = juce::jmax(1, screen.getWidth());
        int h = juce::jmax(1, screen.getHeight());
        SetWindowPos(blackoutHwnd, HWND_TOP, p.x, p.y, w, h, SWP_NOACTIVATE);
        setBlackoutAlpha(blackoutAlpha);
    }

    void destroyWindows()
    {
        if (blackoutHwnd && IsWindow(blackoutHwnd))
            DestroyWindow(blackoutHwnd);
        if (videoHwnd && videoHwnd != parentHwnd && IsWindow(videoHwnd))
            DestroyWindow(videoHwnd);
        blackoutHwnd = nullptr;
        videoHwnd = nullptr;
        parentHwnd = nullptr;
    }

    Api::Player* player = nullptr;
    Api::Media* media = nullptr;
    HWND parentHwnd = nullptr;
    HWND videoHwnd = nullptr;
    HWND blackoutHwnd = nullptr;
    juce::String windowDiagnostic;
    BYTE blackoutAlpha = 0;
    std::int64_t pendingSeekMs = 0;
    bool pendingSeek = false;
    bool pendingPlay = false;
    int lastPlayResult = 0;
    juce::File loadedFile;
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
    bool load(const juce::File&) { return false; }
    void unload() {}
    void play() {}
    void pause() {}
    void stop() {}
    bool isPlaying() const { return false; }
    double getPlayPosition() const { return 0.0; }
    double getVideoDuration() const { return 0.0; }
    void setPlayPosition(double) {}
    void setMuted(bool) {}
    void setVolume(int) {}
    void setBlackoutAlpha(int) {}
};
}
#endif
