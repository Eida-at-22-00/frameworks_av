/*
 * Copyright 2012, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "MediaCodecList"
#include <utils/Log.h>

#include <binder/IServiceManager.h>

#include <android_media_codec.h>

#include <android-base/properties.h>
#include <android-base/no_destructor.h>

#include <media/IMediaCodecList.h>
#include <media/IMediaPlayerService.h>
#include <media/MediaCodecInfo.h>

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/MediaDefs.h>
#include <media/stagefright/omx/OMXUtils.h>
#include <media/stagefright/xmlparser/MediaCodecsXmlParser.h>
#include <media/stagefright/CCodec.h>
#include <media/stagefright/Codec2InfoBuilder.h>
#include <media/stagefright/MediaCodecConstants.h>
#include <media/stagefright/MediaCodecList.h>
#include <media/stagefright/MediaCodecListOverrides.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/OmxInfoBuilder.h>
#include <media/stagefright/PersistentSurface.h>

#include <sys/stat.h>
#include <utils/threads.h>

#include <cutils/properties.h>

#include <algorithm>
#include <regex>

namespace android {

namespace {

constexpr const char* kProfilingResults =
        MediaCodecsXmlParser::defaultProfilingResultsXmlPath;

bool isProfilingNeeded() {
    int8_t value = property_get_bool("debug.stagefright.profilecodec", 0);
    if (value == 0) {
        return false;
    }

    bool profilingNeeded = true;
    FILE *resultsFile = fopen(kProfilingResults, "r");
    if (resultsFile) {
        AString currentVersion = getProfilingVersionString();
        size_t currentVersionSize = currentVersion.size();
        char *versionString = new char[currentVersionSize + 1];
        fgets(versionString, currentVersionSize + 1, resultsFile);
        if (strcmp(versionString, currentVersion.c_str()) == 0) {
            // profiling result up to date
            profilingNeeded = false;
        }
        fclose(resultsFile);
        delete[] versionString;
    }
    return profilingNeeded;
}

OmxInfoBuilder sOmxInfoBuilder{true /* allowSurfaceEncoders */};
OmxInfoBuilder sOmxNoSurfaceEncoderInfoBuilder{false /* allowSurfaceEncoders */};

Mutex sCodec2InfoBuilderMutex;
std::unique_ptr<MediaCodecListBuilderBase> sCodec2InfoBuilder;

MediaCodecListBuilderBase *GetCodec2InfoBuilder() {
    Mutex::Autolock _l(sCodec2InfoBuilderMutex);
    if (!sCodec2InfoBuilder) {
        sCodec2InfoBuilder.reset(new Codec2InfoBuilder);
    }
    return sCodec2InfoBuilder.get();
}

std::vector<MediaCodecListBuilderBase *> GetBuilders() {
    std::vector<MediaCodecListBuilderBase *> builders;
    // if plugin provides the input surface, we cannot use OMX video encoders.
    // In this case, rely on plugin to provide list of OMX codecs that are usable.
    sp<PersistentSurface> surfaceTest = CCodec::CreateInputSurface();
    if (surfaceTest == nullptr) {
        ALOGD("Allowing all OMX codecs");
        builders.push_back(&sOmxInfoBuilder);
    } else {
        ALOGD("Allowing only non-surface-encoder OMX codecs");
        builders.push_back(&sOmxNoSurfaceEncoderInfoBuilder);
    }
    builders.push_back(GetCodec2InfoBuilder());
    return builders;
}

}  // unnamed namespace

class MediaCodecList::InstanceCache {
public:
    static InstanceCache &Get() {
        static base::NoDestructor<InstanceCache> sCache;
        return *sCache;
    }

    InstanceCache() : mBootCompleted(false), mBootCompletedRemote(false) {}

    sp<IMediaCodecList> getLocalInstance() {
        std::unique_lock l(mLocalMutex);

        if (android::media::codec::provider_->in_process_sw_audio_codec_support()
                && !mBootCompleted) {
            mBootCompleted = base::GetBoolProperty("sys.boot_completed", false);
            if (mLocalInstance != nullptr && mBootCompleted) {
                ALOGI("Boot completed, will reset local instance.");
                mLocalInstance = nullptr;
            }
        }
        if (mLocalInstance == nullptr) {
            MediaCodecList *codecList = new MediaCodecList(GetBuilders());
            if (codecList->initCheck() == OK) {
                mLocalInstance = codecList;

                if (isProfilingNeeded()) {
                    ALOGV("Codec profiling needed, will be run in separated thread.");
                    pthread_t profiler;
                    if (pthread_create(&profiler, nullptr, profilerThreadWrapper, nullptr) != 0) {
                        ALOGW("Failed to create thread for codec profiling.");
                    }
                }
            } else {
                // failure to initialize may be temporary. retry on next call.
                delete codecList;
            }
        }

        return mLocalInstance;
    }

    void setLocalInstance(const sp<IMediaCodecList> &instance) {
        std::unique_lock l(mLocalMutex);
        mLocalInstance = instance;
    }

    sp<IMediaCodecList> getRemoteInstance() {
        std::unique_lock l(mRemoteMutex);
        if (android::media::codec::provider_->in_process_sw_audio_codec_support()
                && !mBootCompletedRemote) {
            mBootCompletedRemote = base::GetBoolProperty("sys.boot_completed", false);
            if (mRemoteInstance != nullptr && mBootCompletedRemote) {
                ALOGI("Boot completed, will reset remote instance.");
                mRemoteInstance = nullptr;
            }
        }
        if (mRemoteInstance == nullptr) {
            mMediaPlayer = defaultServiceManager()->getService(String16("media.player"));
            sp<IMediaPlayerService> service =
                interface_cast<IMediaPlayerService>(mMediaPlayer);
            if (service.get() != nullptr) {
                mRemoteInstance = service->getCodecList();
                if (mRemoteInstance != nullptr) {
                    mBinderDeathObserver = new BinderDeathObserver();
                    mMediaPlayer->linkToDeath(mBinderDeathObserver.get());
                }
            }
            if (mRemoteInstance == nullptr) {
                // if failed to get remote list, create local list
                mRemoteInstance = getLocalInstance();
            }
        }
        return mRemoteInstance;
    }

    void binderDied() {
        std::unique_lock l(mRemoteMutex);
        mRemoteInstance.clear();
        mBinderDeathObserver.clear();
    }

private:
    std::mutex mLocalMutex;
    bool mBootCompleted                 GUARDED_BY(mLocalMutex);
    sp<IMediaCodecList> mLocalInstance  GUARDED_BY(mLocalMutex);

    std::mutex mRemoteMutex;
    bool mBootCompletedRemote                       GUARDED_BY(mRemoteMutex);
    sp<IMediaCodecList> mRemoteInstance             GUARDED_BY(mRemoteMutex);
    sp<BinderDeathObserver> mBinderDeathObserver    GUARDED_BY(mRemoteMutex);
    sp<IBinder> mMediaPlayer                        GUARDED_BY(mRemoteMutex);
};

// static
void *MediaCodecList::profilerThreadWrapper(void * /*arg*/) {
    ALOGV("Enter profilerThreadWrapper.");
    remove(kProfilingResults);  // remove previous result so that it won't be loaded to
                                // the new MediaCodecList
    sp<MediaCodecList> codecList(new MediaCodecList(GetBuilders()));
    if (codecList->initCheck() != OK) {
        ALOGW("Failed to create a new MediaCodecList, skipping codec profiling.");
        return nullptr;
    }

    const auto& infos = codecList->mCodecInfos;
    ALOGV("Codec profiling started.");
    profileCodecs(infos, kProfilingResults);
    ALOGV("Codec profiling completed.");
    codecList = new MediaCodecList(GetBuilders());
    if (codecList->initCheck() != OK) {
        ALOGW("Failed to parse profiling results.");
        return nullptr;
    }

    InstanceCache::Get().setLocalInstance(codecList);
    return nullptr;
}

// static
sp<IMediaCodecList> MediaCodecList::getLocalInstance() {
    return InstanceCache::Get().getLocalInstance();
}

void MediaCodecList::BinderDeathObserver::binderDied(const wp<IBinder> &who __unused) {
    InstanceCache::Get().binderDied();
}

// static
sp<IMediaCodecList> MediaCodecList::getInstance() {
    return InstanceCache::Get().getRemoteInstance();
}

MediaCodecList::MediaCodecList(std::vector<MediaCodecListBuilderBase*> builders) {
    mGlobalSettings = new AMessage();
    mCodecInfos.clear();
    MediaCodecListWriter writer;
    for (MediaCodecListBuilderBase *builder : builders) {
        if (builder == nullptr) {
            ALOGD("ignored a null builder");
            continue;
        }
        auto currentCheck = builder->buildMediaCodecList(&writer);
        if (currentCheck != OK) {
            ALOGD("ignored failed builder");
            continue;
        } else {
            mInitCheck = currentCheck;
        }
    }
    writer.writeGlobalSettings(mGlobalSettings);
    writer.writeCodecInfos(&mCodecInfos);
    std::stable_sort(
            mCodecInfos.begin(),
            mCodecInfos.end(),
            [](const sp<MediaCodecInfo> &info1, const sp<MediaCodecInfo> &info2) {
                // null is lowest
                return info1 == nullptr
                        || (info2 != nullptr && info1->getRank() < info2->getRank());
            });

    // remove duplicate entries
    bool dedupe = property_get_bool("debug.stagefright.dedupe-codecs", true);
    if (dedupe) {
        std::set<std::string> codecsSeen;
        for (auto it = mCodecInfos.begin(); it != mCodecInfos.end(); ) {
            std::string codecName = (*it)->getCodecName();
            if (codecsSeen.count(codecName) == 0) {
                codecsSeen.emplace(codecName);
                it++;
            } else {
                it = mCodecInfos.erase(it);
            }
        }
    }
}

MediaCodecList::~MediaCodecList() {
}

status_t MediaCodecList::initCheck() const {
    return mInitCheck;
}

// legacy method for non-advanced codecs
ssize_t MediaCodecList::findCodecByType(
        const char *type, bool encoder, size_t startIndex) const {
    static const char *advancedFeatures[] = {
        "feature-secure-playback",
        "feature-tunneled-playback",
    };

    size_t numCodecInfos = mCodecInfos.size();
    for (; startIndex < numCodecInfos; ++startIndex) {
        const MediaCodecInfo &info = *mCodecInfos[startIndex];

        if (info.isEncoder() != encoder) {
            continue;
        }
        sp<MediaCodecInfo::Capabilities> capabilities = info.getCapabilitiesFor(type);
        if (capabilities == nullptr) {
            continue;
        }
        const sp<AMessage> &details = capabilities->getDetails();

        int32_t required;
        bool isAdvanced = false;
        for (size_t ix = 0; ix < ARRAY_SIZE(advancedFeatures); ix++) {
            if (details->findInt32(advancedFeatures[ix], &required) &&
                    required != 0) {
                isAdvanced = true;
                break;
            }
        }

        if (!isAdvanced) {
            return startIndex;
        }
    }

    return -ENOENT;
}

ssize_t MediaCodecList::findCodecByName(const char *name) const {
    Vector<AString> aliases;
    for (size_t i = 0; i < mCodecInfos.size(); ++i) {
        if (strcmp(mCodecInfos[i]->getCodecName(), name) == 0) {
            return i;
        }
        mCodecInfos[i]->getAliases(&aliases);
        for (const AString &alias : aliases) {
            if (alias == name) {
                return i;
            }
        }
    }

    return -ENOENT;
}

size_t MediaCodecList::countCodecs() const {
    return mCodecInfos.size();
}

const sp<AMessage> MediaCodecList::getGlobalSettings() const {
    return mGlobalSettings;
}

//static
bool MediaCodecList::isSoftwareCodec(const AString &componentName) {
    return componentName.startsWithIgnoreCase("OMX.google.")
            || componentName.startsWithIgnoreCase("c2.android.")
            || (!componentName.startsWithIgnoreCase("OMX.")
                    && !componentName.startsWithIgnoreCase("c2."));
}

static int compareSoftwareCodecsFirst(const AString *name1, const AString *name2) {
    // sort order 1: software codecs are first (lower)
    bool isSoftwareCodec1 = MediaCodecList::isSoftwareCodec(*name1);
    bool isSoftwareCodec2 = MediaCodecList::isSoftwareCodec(*name2);
    if (isSoftwareCodec1 != isSoftwareCodec2) {
        return isSoftwareCodec2 - isSoftwareCodec1;
    }

    // sort order 2: Codec 2.0 codecs are first (lower)
    bool isC2_1 = name1->startsWithIgnoreCase("c2.");
    bool isC2_2 = name2->startsWithIgnoreCase("c2.");
    if (isC2_1 != isC2_2) {
        return isC2_2 - isC2_1;
    }

    // sort order 3: OMX codecs are first (lower)
    bool isOMX1 = name1->startsWithIgnoreCase("OMX.");
    bool isOMX2 = name2->startsWithIgnoreCase("OMX.");
    return isOMX2 - isOMX1;
}

//static
void MediaCodecList::findMatchingCodecs(
        const char *mime, bool encoder, uint32_t flags,
        Vector<AString> *matches) {
    sp<AMessage> format;        // initializes as clear/null
    findMatchingCodecs(mime, encoder, flags, format, matches);
}

//static
void MediaCodecList::findMatchingCodecs(
        const char *mime, bool encoder, uint32_t flags, const sp<AMessage> &format,
        Vector<AString> *matches) {
    matches->clear();

    const sp<IMediaCodecList> list = getInstance();
    if (list == nullptr) {
        return;
    }

    size_t index = 0;
    for (;;) {
        ssize_t matchIndex =
            list->findCodecByType(mime, encoder, index);

        if (matchIndex < 0) {
            break;
        }

        index = matchIndex + 1;

        const sp<MediaCodecInfo> info = list->getCodecInfo(matchIndex);
        CHECK(info != nullptr);

        AString componentName = info->getCodecName();

        if (!codecHandlesFormat(mime, info, format)) {
            ALOGV("skipping codec '%s' which doesn't satisfy format %s",
                  componentName.c_str(), format->debugString(2).c_str());
            continue;
        }

        if ((flags & kHardwareCodecsOnly) && isSoftwareCodec(componentName)) {
            ALOGV("skipping SW codec '%s'", componentName.c_str());
            continue;
        }

        matches->push(componentName);
        ALOGV("matching '%s'", componentName.c_str());
    }

    if (flags & kPreferSoftwareCodecs ||
            property_get_bool("debug.stagefright.swcodec", false)) {
        matches->sort(compareSoftwareCodecsFirst);
    }

    // if we did NOT find anything maybe it's because of a profile mismatch.
    // let's recurse after trimming the profile from the format to see if that yields
    // a suitable codec.
    //
    int profile = -1;
    if (matches->empty() && format != nullptr && format->findInt32(KEY_PROFILE, &profile)) {
        ALOGV("no matching codec found, retrying without profile");
        sp<AMessage> formatNoProfile = format->dup();
        formatNoProfile->removeEntryByName(KEY_PROFILE);
        findMatchingCodecs(mime, encoder, flags, formatNoProfile, matches);
    }
}

// static
bool MediaCodecList::codecHandlesFormat(
        const char *mime, const sp<MediaCodecInfo> &info, const sp<AMessage> &format) {

    if (format == nullptr) {
        ALOGD("codecHandlesFormat: no format, so no extra checks");
        return true;
    }

    sp<MediaCodecInfo::Capabilities> capabilities = info->getCapabilitiesFor(mime);

    // ... no capabilities listed means 'handle it all'
    if (capabilities == nullptr) {
        ALOGD("codecHandlesFormat: no capabilities for refinement");
        return true;
    }

    const sp<AMessage> &details = capabilities->getDetails();

    // if parsing the capabilities fails, ignore this particular codec
    // currently video-centric evaluation
    //
    // TODO: like to make it handle the same set of properties from
    // MediaCodecInfo::isFormatSupported()
    // not yet done here are:
    //  profile, level, bitrate, features,

    bool isVideo = false;
    if (strncmp(mime, "video/", 6) == 0) {
        isVideo = true;
    }

    if (isVideo) {
        int width = -1;
        int height = -1;

        if (format->findInt32("height", &height) && format->findInt32("width", &width)) {

            // is it within the supported size range of the codec?
            AString sizeRange;
            AString minSize,maxSize;
            AString minWidth, minHeight;
            AString maxWidth, maxHeight;
            if (!details->findString("size-range", &sizeRange)
                || !splitString(sizeRange, "-", &minSize, &maxSize)) {
                ALOGW("Unable to parse size-range from codec info");
                return false;
            }
            if (!splitString(minSize, "x", &minWidth, &minHeight)) {
                if (!splitString(minSize, "*", &minWidth, &minHeight)) {
                    ALOGW("Unable to parse size-range/min-size from codec info");
                    return false;
                }
            }
            if (!splitString(maxSize, "x", &maxWidth, &maxHeight)) {
                if (!splitString(maxSize, "*", &maxWidth, &maxHeight)) {
                    ALOGW("Unable to fully parse size-range/max-size from codec info");
                    return false;
                }
            }

            // strtol() returns 0 if unable to parse a number, which works for our later tests
            int minW = strtol(minWidth.c_str(), NULL, 10);
            int minH = strtol(minHeight.c_str(), NULL, 10);
            int maxW = strtol(maxWidth.c_str(), NULL, 10);
            int maxH = strtol(maxHeight.c_str(), NULL, 10);

            if (minW == 0 || minH == 0 || maxW == 0 || maxH == 0) {
                ALOGW("Unable to parse values from size-range from codec info");
                return false;
            }

            // finally, comparison time
            if (width < minW || width > maxW || height < minH || height > maxH) {
                ALOGV("format %dx%d outside of allowed %dx%d-%dx%d",
                      width, height, minW, minH, maxW, maxH);
                // at this point, it's a rejection, UNLESS
                // the codec allows swapping width and height
                int32_t swappable;
                if (!details->findInt32("feature-can-swap-width-height", &swappable)
                    || swappable == 0) {
                    return false;
                }
                // NB: deliberate comparison of height vs width limits (and width vs height)
                if (height < minW || height > maxW || width < minH || width > maxH) {
                    return false;
                }
            }

            // @ 'alignment' [e.g. "2x2" which tells us that both dimensions must be even]
            // no alignment == we're ok with anything
            AString alignment, alignWidth, alignHeight;
            if (details->findString("alignment", &alignment)) {
                if (splitString(alignment, "x", &alignWidth, &alignHeight) ||
                    splitString(alignment, "*", &alignWidth, &alignHeight)) {
                    int wAlign = strtol(alignWidth.c_str(), NULL, 10);
                    int hAlign = strtol(alignHeight.c_str(), NULL, 10);
                    // strtol() returns 0 if failing to parse, treat as "no restriction"
                    if (wAlign > 0 && hAlign > 0) {
                         if ((width % wAlign) != 0 || (height % hAlign) != 0) {
                            ALOGV("format dimensions %dx%d not aligned to %dx%d",
                                 width, height, wAlign, hAlign);
                            return false;
                         }
                    }
                }
            }
        }

        int32_t profile = -1;
        if (format->findInt32(KEY_PROFILE, &profile)) {
            Vector<MediaCodecInfo::ProfileLevel> profileLevels;
            capabilities->getSupportedProfileLevels(&profileLevels);
            auto it = profileLevels.begin();
            for (; it != profileLevels.end(); ++it) {
                if (profile != it->mProfile) {
                    continue;
                }
                break;
            }

            if (it == profileLevels.end()) {
                ALOGV("Codec does not support profile %d", profile);
                return false;
            }
        }
    }

    // haven't found a reason to discard this one
    return true;
}

}  // namespace android
