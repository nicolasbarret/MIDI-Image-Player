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
    static constexpr int PlayerPoolSize = 3;

    VideoView()
    {
        setOpaque(true);
        for (auto& slot : slots)
            slot.player = Api::get().ready() ? Api::get().newPlayer(Api::get().instance) : nullptr;
        startTimerHz(120);
    }

    ~VideoView() override
    {
        stop();
        for (auto& slot : slots)
        {
            if (slot.media && Api::get().releaseMedia)
                Api::get().releaseMedia(slot.media);
            slot.media = nullptr;
            if (slot.player && Api::get().releasePlayer)
                Api::get().releasePlayer(slot.player);
            slot.player = nullptr;
        }
        destroyWindows();
    }

    bool isAvailable() const
    {
        // Availability means LibVLC player objects were created. Media objects
        // only exist after load(), so do not require them at this stage.
        return createdPlayerCount() > 0;
    }

    juce::String errorMessage() const
    {
        if (!Api::get().module)
            return "LibVLC introuvable. Le dossier VLC doit être à côté du programme.";
        if (!Api::get().instance)
            return "Impossible d'initialiser LibVLC. Vérifie le dossier VLC et ses greffons.";
        if (createdPlayerCount() == 0)
            return "Impossible de créer les lecteurs LibVLC.";
        return {};
    }

    bool load(const juce::File& file)
    {
        if (!file.existsAsFile() || createdPlayerCount() == 0)
            return false;

        stop();
        for (auto& slot : slots)
        {
            if (slot.media)
            {
                Api::get().releaseMedia(slot.media);
                slot.media = nullptr;
            }
            slot.warmed = false;
        }

        auto utf8 = file.getFullPathName().toUTF8();
        int loaded = 0;
        for (auto& slot : slots)
        {
            if (!slot.player)
                continue;
            slot.media = Api::get().newPath(Api::get().instance, utf8.getAddress());
            if (!slot.media)
                continue;
            Api::get().setMedia(slot.player, slot.media);
            ++loaded;
        }

        if (loaded == 0)
            return false;

        loadedFile = file;
        activeSlot = -1;
        nextSlot = 0;
        pendingPlay = false;
        pendingPlaySlot = -1;
        pendingSeek = false;
        warmupActive = true;
        warmupIndex = 0;
        warmupPhase = 0;
        warmupDeadline = 0;
        ensureWindows();
        setBlackoutAlpha(255);
        return true;
    }

    void unload()
    {
        stop();
        for (auto& slot : slots)
        {
            if (slot.media)
            {
                Api::get().releaseMedia(slot.media);
                slot.media = nullptr;
            }
            slot.warmed = false;
        }
        loadedFile = {};
        pendingPlay = false;
        pendingPlaySlot = -1;
        pendingSeek = false;
        pendingSeekSlot = -1;
        warmupActive = false;
        setBlackoutAlpha(255);
    }

    // Used by MIDI Note On. A prepared player is selected for every retrigger,
    // while all players render into the same OBS output HWND.
    void trigger(double seconds)
    {
        pendingSeekMs = (std::int64_t)(juce::jmax(0.0, seconds) * 1000.0);
        pendingSeek = true;
        pendingSeekSlot = -1;
        play();
    }

    // Manual play resumes the current player unless a new seek is pending.
    void play()
    {
        if (usablePlayerCount() == 0)
            return;

        cancelWarmup();
        int selected = pendingSeek ? chooseNextPlayer() : activeSlot;
        if (selected < 0)
            selected = chooseNextPlayer();
        if (selected < 0)
            return;

        if (!ensureWindows())
        {
            pendingPlay = true;
            pendingPlaySlot = selected;
            return;
        }

        startPlayer(selected);
    }

    void pause()
    {
        if (auto* p = activePlayer())
            Api::get().setPause(p, 1);
    }

    void stop()
    {
        for (auto& slot : slots)
            if (slot.player && slot.media)
                Api::get().stop(slot.player);
        activeSlot = -1;
        nextSlot = 0;
        pendingPlay = false;
        pendingPlaySlot = -1;
        pendingSeek = false;
        pendingSeekSlot = -1;
        warmupActive = false;
    }

    bool isPlaying() const
    {
        auto* p = activePlayer();
        return p && Api::get().isPlaying(p) != 0;
    }

    juce::String playbackDiagnostic() const
    {
        if (usablePlayerCount() == 0) return errorMessage();
        if (!loadedFile.existsAsFile()) return "Aucun média associé aux lecteurs VLC.";
        if (!videoHwnd || !IsWindow(videoHwnd))
            return windowDiagnostic.isNotEmpty() ? windowDiagnostic : "Fenêtre vidéo Windows non créée.";
        const auto* slot = diagnosticSlot();
        if (slot == nullptr) return "Aucun lecteur VLC prêt.";
        if (slot->lastPlayResult < 0) return "LibVLC a refusé le démarrage (code -1).";
        if (isPlaying()) return "VLC : lecture active (3 lecteurs préchargés).";
        if (warmupActive) return "VLC : préchargement des lecteurs...";
        return "VLC chargé, lecteurs préchargés.";
    }

    double getPlayPosition() const
    {
        auto* p = activePlayer();
        if (!p && slots[0].player && slots[0].media)
            p = slots[0].player;
        return p ? (double) Api::get().getTime(p) / 1000.0 : 0.0;
    }

    double getVideoDuration() const
    {
        auto* p = activePlayer();
        if (!p && slots[0].player && slots[0].media)
            p = slots[0].player;
        return p ? (double) Api::get().getLength(p) / 1000.0 : 0.0;
    }

    void setPlayPosition(double seconds)
    {
        pendingSeekMs = (std::int64_t)(juce::jmax(0.0, seconds) * 1000.0);
        auto* p = activePlayer();
        if (p && Api::get().isPlaying(p))
        {
            Api::get().setTime(p, pendingSeekMs);
            pendingSeek = false;
            pendingSeekSlot = -1;
        }
        else
        {
            pendingSeek = true;
            pendingSeekSlot = -1;
        }
    }

    void setMuted(bool muted)
    {
        for (auto& slot : slots)
            if (slot.player)
                Api::get().setMute(slot.player, muted ? 1 : 0);
    }

    void setVolume(int volume)
    {
        for (auto& slot : slots)
            if (slot.player)
                Api::get().setVolume(slot.player, juce::jlimit(0, 100, volume));
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
    struct PlayerSlot
    {
        Api::Player* player = nullptr;
        Api::Media* media = nullptr;
        int lastPlayResult = 0;
        bool warmed = false;
    };

    int createdPlayerCount() const
    {
        int count = 0;
        for (const auto& slot : slots)
            if (slot.player)
                ++count;
        return count;
    }

    int usablePlayerCount() const
    {
        int count = 0;
        for (const auto& slot : slots)
            if (slot.player && slot.media)
                ++count;
        return count;
    }

    Api::Player* activePlayer() const
    {
        if (activeSlot >= 0 && activeSlot < PlayerPoolSize)
            return slots[(size_t) activeSlot].player;
        return nullptr;
    }

    const PlayerSlot* diagnosticSlot() const
    {
        if (activeSlot >= 0 && activeSlot < PlayerPoolSize)
            return &slots[(size_t) activeSlot];
        for (const auto& slot : slots)
            if (slot.player && slot.media)
                return &slot;
        return nullptr;
    }

    int chooseNextPlayer() const
    {
        int count = usablePlayerCount();
        if (count == 0)
            return -1;
        for (int offset = 0; offset < PlayerPoolSize; ++offset)
        {
            int candidate = (nextSlot + offset) % PlayerPoolSize;
            const auto& slot = slots[(size_t) candidate];
            if (slot.player && slot.media &&
                (candidate != activeSlot || count == 1))
                return candidate;
        }
        return activeSlot;
    }

    void startPlayer(int selected)
    {
        if (selected < 0 || selected >= PlayerPoolSize)
            return;
        auto& slot = slots[(size_t) selected];
        if (!slot.player || !slot.media)
            return;

        if (activeSlot >= 0 && activeSlot != selected)
        {
            auto* old = slots[(size_t) activeSlot].player;
            if (old)
                Api::get().setPause(old, 1);
        }

        Api::get().setHwnd(slot.player, videoHwnd);
        slot.lastPlayResult = Api::get().play(slot.player);

        activeSlot = selected;
        if (pendingSeek)
        {
            // VLC ignores set_time until its input is genuinely playing.
            // Apply it from the 120 Hz timer as soon as is_playing becomes true.
            pendingSeekSlot = selected;
        }
        else
        {
            pendingSeekSlot = -1;
        }
        nextSlot = (selected + 1) % PlayerPoolSize;
        pendingPlay = false;
        pendingPlaySlot = -1;
        setBlackoutAlpha(0);
    }

    void cancelWarmup()
    {
        if (!warmupActive)
            return;
        if (warmupPhase > 0 && warmupIndex < PlayerPoolSize)
        {
            auto* p = slots[(size_t) warmupIndex].player;
            if (p)
                Api::get().setPause(p, 1);
        }
        warmupActive = false;
        warmupPhase = 0;
    }

    void serviceWarmup()
    {
        if (!warmupActive || activeSlot >= 0 || pendingPlay)
            return;

        while (warmupIndex < PlayerPoolSize &&
               (!slots[(size_t) warmupIndex].player || !slots[(size_t) warmupIndex].media))
            ++warmupIndex;

        if (warmupIndex >= PlayerPoolSize)
        {
            warmupActive = false;
            setBlackoutAlpha(0);
            return;
        }

        auto& slot = slots[(size_t) warmupIndex];
        const auto now = juce::Time::getMillisecondCounter();

        if (warmupPhase == 0)
        {
            Api::get().setHwnd(slot.player, videoHwnd);
            slot.lastPlayResult = Api::get().play(slot.player);
            warmupDeadline = now + 2500;
            warmupPhase = 1;
        }
        else if (warmupPhase == 1)
        {
            if (Api::get().isPlaying(slot.player))
            {
                Api::get().setPause(slot.player, 1);
                warmupDeadline = now + 750;
                warmupPhase = 2;
            }
            else if (now >= warmupDeadline)
            {
                Api::get().stop(slot.player);
                ++warmupIndex;
                warmupPhase = 0;
            }
        }
        else if (warmupPhase == 2)
        {
            if (!Api::get().isPlaying(slot.player) || now >= warmupDeadline)
            {
                Api::get().setPause(slot.player, 1);
                Api::get().setTime(slot.player, 0);
                slot.warmed = true;
                ++warmupIndex;
                warmupPhase = 0;
            }
        }
    }

    void timerCallback() override
    {
        // Rebind all preloaded players if JUCE recreates its native window.
        if ((!videoHwnd || !IsWindow(videoHwnd)) && isShowing())
            ensureWindows();

        if (pendingPlay && ensureWindows())
            startPlayer(pendingPlaySlot);

        if (pendingSeek && pendingSeekSlot >= 0 && pendingSeekSlot < PlayerPoolSize)
        {
            auto* p = slots[(size_t) pendingSeekSlot].player;
            if (p && Api::get().isPlaying(p))
            {
                Api::get().setTime(p, pendingSeekMs);
                pendingSeek = false;
                pendingSeekSlot = -1;
            }
        }

        if (warmupActive && ensureWindows())
            serviceWarmup();
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

            // One native output HWND is shared by the three VLC players.
            auto style = GetWindowLongPtrW(parentHwnd, GWL_STYLE);
            SetWindowLongPtrW(parentHwnd, GWL_STYLE, style | WS_CLIPCHILDREN);
            videoHwnd = parentHwnd;

            blackoutHwnd = CreateWindowExW(WS_EX_LAYERED, L"STATIC", L"",
                                           WS_CHILD | WS_CLIPSIBLINGS,
                                           0, 0, 1, 1, parentHwnd, nullptr,
                                           GetModuleHandleW(nullptr), nullptr);
            if (!blackoutHwnd)
                windowDiagnostic = "VLC utilise la sortie unique. Le cache noir de préchargement est indisponible.";
            else
                windowDiagnostic.clear();

            for (auto& slot : slots)
                if (slot.player)
                    Api::get().setHwnd(slot.player, videoHwnd);

            setBlackoutAlpha(blackoutAlpha);
            positionWindows();
        }
        const bool valid = videoHwnd != nullptr && IsWindow(videoHwnd);
        if (!valid && windowDiagnostic.isEmpty())
            windowDiagnostic = "Fenêtre vidéo : handle Windows invalide.";
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

    std::array<PlayerSlot, PlayerPoolSize> slots{};
    int activeSlot = -1;
    int nextSlot = 0;
    HWND parentHwnd = nullptr;
    HWND videoHwnd = nullptr;
    HWND blackoutHwnd = nullptr;
    juce::String windowDiagnostic;
    BYTE blackoutAlpha = 255;
    std::int64_t pendingSeekMs = 0;
    bool pendingSeek = false;
    int pendingSeekSlot = -1;
    bool pendingPlay = false;
    int pendingPlaySlot = -1;
    bool warmupActive = false;
    int warmupIndex = 0;
    int warmupPhase = 0;
    juce::uint32 warmupDeadline = 0;
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
};
}
#endif
