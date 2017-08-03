#include "sound.hpp"
#include "logger.hpp"
#include "resourcemapper.hpp"

Sound::Sound(IAudioController& audioController, ResourceLocator& locator, OSBaseFileSystem& fs)
    : mAudioController(audioController), mLocator(locator), mCache(fs)
{
    mAudioController.AddPlayer(this);

    Debugging().AddSection([&]()
    {
        SoundBrowserUi();
    });
}

Sound::~Sound()
{
    // Ensure the audio call back stops at this point else will be
    // calling back into freed objects
    SDL_AudioQuit();

    mAudioController.RemovePlayer(this);
}

void Sound::SetMusicTheme(const char* themeName, const char* eventOnLoad)
{
    //CacheMemoryResidentSounds();


    mAmbiance = nullptr;
    mMusicTrack = nullptr;

    // This is just an in-memory non blocking look up
    mThemeToLoad = mLocator.LocateSoundTheme(themeName);
    mEventToSetAfterLoad = eventOnLoad ? eventOnLoad : "";

    if (mThemeToLoad)
    {
        if (mState != eSoundStates::eIdle)
        {
            SetState(eSoundStates::eCancel);
        }
        else
        {
            SetState(eSoundStates::eUnloadingActiveSoundTheme);
        }
    }
    else
    {
        LOG_ERROR("Music theme " << themeName << " was not found");
    }
}

bool Sound::IsLoading() const
{
    return mState != eSoundStates::eIdle;
}

std::unique_ptr<ISound> Sound::PlaySound(const char* soundName, const char* explicitSoundBankName, bool useMusicRec, bool useSfxRec, bool useCache)
{
    if (useCache)
    {
        std::unique_ptr<ISound> pSound = mCache.GetCached(soundName);
        if (pSound)
        {
            LOG_INFO("Play sound cached: " << soundName);
        }
        else
        {
            LOG_ERROR("Cached sound not found: " << soundName);
        }
        return pSound;
    }
    else
    {
        std::unique_ptr<ISound> pSound = mLocator.LocateSound(soundName, explicitSoundBankName, useMusicRec, useSfxRec);
        if (pSound)
        {
            LOG_INFO("Play sound: " << soundName);
            pSound->Load();
        }
        else
        {
            LOG_ERROR("Sound: " << soundName << " not found");
        }
        return pSound;
    }
}

void Sound::HandleMusicEvent(const char* eventName)
{
    // TODO: Need quarter beat transition in some cases
    EnsureAmbiance();

    if (strcmp(eventName, "AMBIANCE") == 0)
    {
        mMusicTrack = nullptr;
        return;
    }

    auto ret = PlayThemeEntry(eventName);
    if (ret)
    {
        mMusicTrack = std::move(ret);
    }
}

void Sound::PlaySoundEffect(const char* soundName)
{
    auto ret = PlaySound(soundName, nullptr, true, true, true);
    if (ret)
    {
        std::lock_guard<std::mutex> lock(mSoundPlayersMutex);
        mSoundPlayers.push_back(std::move(ret));
    }
}


up_future_void Sound::CacheMemoryResidentSounds()
{
    return std::make_unique<future_void>(std::async(std::launch::async, [&]() 
    {
        TRACE_ENTRYEXIT;

        SetState(eSoundStates::eLoadingSoundEffects);

        // initial one time sync
        mCache.Sync();

        const std::vector<SoundResource>& resources = mLocator.GetSoundResources();
        for (const SoundResource& resource : resources)
        {
            if (resource.mIsCacheResident)
            {
                mCache.CacheSound(mLocator, resource.mResourceName);
            }
        }
    }));
}

void Sound::CacheActiveTheme(bool add)
{
    for (auto& entry : mActiveTheme->mEntries)
    {
        for (auto& e : entry.second)
        {
            if (add)
            {
                mCache.CacheSound(mLocator, e.mMusicName);
            }
            else
            {
                mCache.RemoveFromMemoryCache(e.mMusicName);
            }
        }
    }
}


std::unique_ptr<ISound> Sound::PlayThemeEntry(const char* entryName)
{
    if (mActiveTheme)
    {
        mActiveThemeEntry.SetMusicThemeEntry(mActiveTheme->FindEntry(entryName));

        const MusicThemeEntry* entry = mActiveThemeEntry.Entry();
        if (entry)
        {
            return PlaySound(entry->mMusicName.c_str(), nullptr, true, true, true);
        }
    }
    return nullptr;
}

void Sound::EnsureAmbiance()
{
    std::lock_guard<std::mutex> lock(mSoundPlayersMutex);
    if (!mAmbiance)
    {
        mAmbiance = PlayThemeEntry("AMBIANCE");
    }
}

// Audio thread context
bool Sound::Play(f32* stream, u32 len)
{
    std::lock_guard<std::mutex> lock(mSoundPlayersMutex);

    if (mAmbiance)
    {
        mAmbiance->Play(stream, len);
    }

    if (mMusicTrack)
    {
        mMusicTrack->Play(stream, len);
    }

    for (auto& player : mSoundPlayers)
    {
        player->Play(stream, len);
    }
    return false;
}

void Sound::SetState(Sound::eSoundStates state)
{
    if (mState != state)
    {
        mState = state;
    }
}

void Sound::Update()
{
    switch (mState)
    {
    case Sound::eSoundStates::eIdle:
        break;

    case eSoundStates::eLoadingSoundEffects:
        break;

    case eSoundStates::eCancel:
        mCache.Cancel();
        SetState(eSoundStates::eCancelling);
        break;

    case eSoundStates::eCancelling:
        if (!mCache.IsBusy())
        {
            SetState(eSoundStates::eUnloadingActiveSoundTheme);
        }
        break;

    case Sound::eSoundStates::eUnloadingActiveSoundTheme:
        if (mActiveTheme)
        {
            CacheActiveTheme(false);
        }
        mActiveTheme = mThemeToLoad;
        mThemeToLoad = nullptr;
        SetState(eSoundStates::eLoadActiveSoundTheme);
        break;

    case eSoundStates::eLoadActiveSoundTheme:
        if (mActiveTheme)
        {
            CacheActiveTheme(true);
        }
        SetState(eSoundStates::eLoadingActiveSoundTheme);
        break;

    case eSoundStates::eLoadingActiveSoundTheme:
        if (!mCache.IsBusy())
        {
            SetState(eSoundStates::eIdle);
            if (!mEventToSetAfterLoad.empty())
            {
                HandleMusicEvent(mEventToSetAfterLoad.c_str());
                mEventToSetAfterLoad.clear();
            }
        }
        break;
    }

    std::lock_guard<std::mutex> lock(mSoundPlayersMutex);
    for (auto it = mSoundPlayers.begin(); it != mSoundPlayers.end();)
    {
        if ((*it)->AtEnd())
        {
            it = mSoundPlayers.erase(it);
        }
        else
        {
            it++;
        }
    }

    if (mAmbiance)
    {
        mAmbiance->Update();
        if (mAmbiance->AtEnd())
        {
            mAmbiance->Restart();
        }
    }

    if (mMusicTrack)
    {
        mMusicTrack->Update();
        if (mMusicTrack->AtEnd())
        {
            if (mActiveThemeEntry.ToNextEntry())
            {
                mMusicTrack = PlaySound(mActiveThemeEntry.Entry()->mMusicName.c_str(), nullptr, true, true, true);
            }
            else
            {
                mMusicTrack = nullptr;
            }
        }
    }

    for (auto& player : mSoundPlayers)
    {
        player->Update();
    }
}

static const char* kMusicEvents[] =
{
    "AMBIANCE",
    "BASE_LINE",
    "CRITTER_ATTACK",
    "CRITTER_PATROL",
    "SLIG_ATTACK",
    "SLIG_PATROL",
    "SLIG_POSSESSED",
};


void Sound::SoundBrowserUi()
{
    {
        std::lock_guard<std::mutex> lock(mSoundPlayersMutex);
        if (!mSoundPlayers.empty())
        {
            mSoundPlayers[0]->DebugUi();
        }

        if (ImGui::CollapsingHeader("Active SEQs"))
        {
            if (mAmbiance)
            {
                ImGui::Text("Ambiance: %s", mAmbiance->Name().c_str());
            }
            else
            {
                ImGui::TextUnformatted("Ambiance: (none)");
            }

            if (mMusicTrack)
            {
                ImGui::Text("Music: %s", mMusicTrack->Name().c_str());
            }
            else
            {
                ImGui::TextUnformatted("Music: (none)");
            }

            int i = 0;
            for (auto& player : mSoundPlayers)
            {
                i++;
                if (ImGui::Button((std::to_string(i) + player->Name()).c_str()))
                {
                    if (player.get() == mSoundPlayers[i].get())
                    {
                        player->Stop();
                    }
                }
            }
        }
    }

    if (ImGui::CollapsingHeader("Sound list"))
    {
        static const SoundResource* selected = nullptr;

        // left
        ImGui::BeginChild("left pane", ImVec2(200, 200), true);
        {
            for (const SoundResource& soundInfo : mLocator.GetSoundResources())
            {
                if (ImGui::Selectable(soundInfo.mResourceName.c_str()))
                {
                    selected = &soundInfo;
                }
            }
            ImGui::EndChild();
        }
        ImGui::SameLine();

        // right
        ImGui::BeginGroup();
        {
            ImGui::BeginChild("item view", ImVec2(0, 200));
            {
                if (selected)
                {
                    ImGui::TextWrapped("Resource name: %s", selected->mResourceName.c_str());
                    ImGui::Separator();
                    ImGui::TextWrapped("Comment: %s", selected->mComment.empty() ? "(none)" : selected->mComment.c_str());
                    ImGui::Separator();
                    ImGui::TextWrapped("Is memory resident: %s", selected->mIsCacheResident ? "true" : "false");
                    ImGui::TextWrapped("Is cached: %s", mCache.ExistsInMemoryCache(selected->mResourceName) ? "true" : "false");

                    static bool bUseCache = false;
                    ImGui::Checkbox("Use cache", &bUseCache);

                    const bool bHasMusic = !selected->mMusic.mSoundBanks.empty();
                    const bool bHasSample = !selected->mSoundEffect.mSoundBanks.empty();
                    if (bHasMusic)
                    {
                        if (ImGui::CollapsingHeader("SEQs"))
                        {
                            for (const std::string& sb : selected->mMusic.mSoundBanks)
                            {
                                if (ImGui::Selectable(sb.c_str()))
                                {
                                    auto player = PlaySound(selected->mResourceName.c_str(), sb.c_str(), true, false, bUseCache);
                                    if (player)
                                    {
                                        std::lock_guard<std::mutex> lock(mSoundPlayersMutex);
                                        mSoundPlayers.push_back(std::move(player));
                                    }
                                }
                            }
                        }
                    }

                    if (bHasSample)
                    {
                        if (ImGui::CollapsingHeader("Samples"))
                        {
                            for (const SoundEffectResourceLocation& sbLoc : selected->mSoundEffect.mSoundBanks)
                            {
                                for (const std::string& sb : sbLoc.mSoundBanks)
                                {
                                    if (ImGui::Selectable(sb.c_str()))
                                    {
                                        auto player = PlaySound(selected->mResourceName.c_str(), sb.c_str(), false, true, bUseCache);
                                        if (player)
                                        {
                                            std::lock_guard<std::mutex> lock(mSoundPlayersMutex);
                                            mSoundPlayers.push_back(std::move(player));
                                        }
                                    }
                                }
                            }
                        }
                    }

                    if (ImGui::Button("Play (cached/scripted)"))
                    {
                        PlaySoundEffect(selected->mResourceName.c_str());
                    }
                }
                else
                {
                    ImGui::TextWrapped("Click an item to display its info");
                }
            }
            ImGui::EndChild();
        }
        ImGui::EndGroup();
    }

    if (ImGui::CollapsingHeader("Sound themes"))
    {
        for (const MusicTheme& theme : mLocator.mResMapper.mSoundResources.mThemes)
        {
            if (ImGui::RadioButton(theme.mName.c_str(), mActiveTheme && theme.mName == mActiveTheme->mName))
            {
                SetMusicTheme(theme.mName.c_str(), "BASE_LINE");
            }
        }

        // TODO: Use radio buttons - use as active event when theme is changed
        for (const char* eventName : kMusicEvents)
        {
            if (ImGui::Button(eventName))
            {
                HandleMusicEvent(eventName);
            }
        }
    }
}
