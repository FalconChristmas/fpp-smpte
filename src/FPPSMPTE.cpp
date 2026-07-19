#include <fpp-pch.h>

#include <ltc.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_audio.h>
#ifndef PLATFORM_OSX
#include <sys/eventfd.h>
#endif
#include <atomic>
#include <cinttypes>
#include <cstring>
#include <mutex>
#include <vector>

#if __has_include(<pipewire/pipewire.h>)
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#define HAS_PIPEWIRE_SOURCE 1
#endif

#include "FPPSMPTE.h"
#include "Plugin.h"
#include "MultiSync.h"
#include "playlist/Playlist.h"
#include "channeloutput/channeloutputthread.h"

// AudioSourceRegistry is only present in FPP cores that support plugin
// published PipeWire sources; compile the registration in when available.
#if __has_include("mediaoutput/AudioSourceRegistry.h")
#include "mediaoutput/AudioSourceRegistry.h"
#define HAS_AUDIO_SOURCE_REGISTRY 1
#endif


class FPPSMPTEPlugin : public FPPPlugin, public MultiSyncPlugin {
    
public:
    FPPSMPTEPlugin() : FPPPlugin("fpp-smpte") {
        LogInfo(VB_PLUGIN, "Initializing SMPTE Plugin\n");
        setDefaultSettings();
        SDL_Init(SDL_INIT_AUDIO);

        memset(&outputTimeCode, 0, sizeof(outputTimeCode));
    }
    virtual ~FPPSMPTEPlugin() {
        MultiSync::INSTANCE.removeMultiSyncPlugin(this);
#ifdef HAS_AUDIO_SOURCE_REGISTRY
        AudioSourceRegistry::INSTANCE.unregisterPluginSources("fpp-smpte");
#endif
#ifdef HAS_PIPEWIRE_SOURCE
        stopPipeWireSource();
#endif
        if (audioStream) {
            SDL_DestroyAudioStream(audioStream);
            audioStream = nullptr;
        }
        if (ltcDecoder) {
            ltc_decoder_free(ltcDecoder);
        }
        if (ltcEncoder) {
            ltc_encoder_free(ltcEncoder);
        }
        if (inputEventFileRead >= 0) {
            close(inputEventFileRead);
        }
        if (inputEventFileWrite >= 0 && inputEventFileWrite != inputEventFileRead) {
            close(inputEventFileWrite);
        }
    }
    
    // ── Output abstraction: SDL device stream or PipeWire source node ──
    bool outputActive() {
#ifdef HAS_PIPEWIRE_SOURCE
        if (pwStream) {
            return true;
        }
#endif
        return audioStream != nullptr;
    }
    // Queued LTC audio not yet consumed, in samples (output is U8 mono,
    // so bytes == samples for both paths).
    int queuedOutputSamples() {
#ifdef HAS_PIPEWIRE_SOURCE
        if (pwStream) {
            std::lock_guard<std::mutex> lk(pwRingMutex);
            return (int)pwRing.size();
        }
#endif
        if (audioStream) {
            return SDL_GetAudioStreamQueued(audioStream);
        }
        return 0;
    }
    void putOutputData(const uint8_t* buf, int len) {
#ifdef HAS_PIPEWIRE_SOURCE
        if (pwStream) {
            std::lock_guard<std::mutex> lk(pwRingMutex);
            // Safety cap; backpressure in encodeTimestamp() normally keeps
            // the ring at 1-2 LTC frames.
            if (pwRing.size() + len <= 16384) {
                pwRing.insert(pwRing.end(), buf, buf + len);
            }
            return;
        }
#endif
        if (audioStream) {
            SDL_PutAudioStreamData(audioStream, buf, len);
        }
    }
    // Offset (ms) added to the encoded timecode so that what is HEARD at
    // the output lines up with the playlist position.  In PipeWire source
    // mode the graph latency is auto-detected (pw_time.delay covers the
    // whole path to the device, plus our own ring backlog); the user
    // setting is an additional manual trim on top for latency outside the
    // box (external gear, network receivers, ...).
    int64_t currentOutputOffsetMS() {
        int64_t off = outputOffsetMS;
#ifdef HAS_PIPEWIRE_SOURCE
        if (pwStream) {
            off += pwAutoLatencyMS;
        }
#endif
        return off;
    }

    void encodeTimestamp(uint64_t ms) {
        int len = queuedOutputSamples();
        if (len > 2048) {
            return;
        }
        if (ms != 0) {
            // ms == 0 is the stop/reset sentinel; don't offset it
            int64_t adj = (int64_t)ms + currentOutputOffsetMS();
            ms = adj < 1 ? 1 : (uint64_t)adj;
        }

        uint64_t frame = ms;
        frame *= framerate;
        frame /= 1000;
       
        if ((lastFrame < frame) || (frame == 0) || (lastFrame > frame + 3)) {
            if (lastFrame == (frame - 1)) {
                ltc_encoder_inc_timecode(ltcEncoder);
            } else {
                uint64_t sf = ms % 1000;
                ms /= 1000;
                sf *= framerate;
                sf /= 1000;
                if (outputTimeCode.frame == sf && ((ms % 60) == outputTimeCode.secs)) {
                    //duplicate, return
                    return;
                }
                outputTimeCode.frame = sf;
                //printf("Frame:  %d\n", outputTimeCode.frame);
                outputTimeCode.secs = ms % 60;
                ms /= 60;
                outputTimeCode.mins = ms % 60;
                ms /= 60;
                outputTimeCode.hours = ms;
                ltc_encoder_set_timecode(ltcEncoder, &outputTimeCode);
            }
            lastFrame = frame;
            ltc_encoder_encode_frame(ltcEncoder);
            ltcsnd_sample_t *buf;
            len = ltc_encoder_get_bufferptr(ltcEncoder, &buf, 1);
            putOutputData(buf, len);
        }
        int i = GetChannelOutputRefreshRate();
        ms += (1000/i);
        frame = ms;
        frame *= framerate;
        frame /= 1000;
        if ((lastFrame+1) < frame) {
            //next frame will be skipped, queue it now
            ltc_encoder_inc_timecode(ltcEncoder);
            lastFrame++;
            ltc_encoder_encode_frame(ltcEncoder);
            ltcsnd_sample_t *buf;
            len = ltc_encoder_get_bufferptr(ltcEncoder, &buf, 1);
            putOutputData(buf, len);
        }
    }
    uint64_t getTimestampFromPlaylist() {
        int pos;
        uint64_t ms;
        uint64_t posms;
        
        ms = playlist->GetCurrentPosInMS(pos, posms, timeCodePType == TimeCodeProcessingType::PLAYLIST_ITEM_DEFINED);
        if (timeCodePType == TimeCodeProcessingType::HOUR) {
            ms = posms + pos * 60 * 1000 * 60;
        } else if (timeCodePType == TimeCodeProcessingType::MIN15) {
            ms = posms + pos * 15 * 1000 * 60;
        }
        return ms == 0 ? 1 : ms;  // zero is stop so we will use 1ms as a starting point
    }
    virtual void SendSeqSyncPacket(const std::string &filename, int frames, float seconds) override {
        if (outputActive()) {
            encodeTimestamp(getTimestampFromPlaylist());
        }
    }
    virtual void SendMediaSyncPacket(const std::string &filename, float seconds) override {
        if (outputActive()) {
            encodeTimestamp(getTimestampFromPlaylist());
        }
    }

    virtual void playlistCallback(const Json::Value &plj, const std::string &action, const std::string &section, int item) override {
        if (action == "stop") {
            encodeTimestamp(0);
            stopAudio();
        } else if (action == "start") {
            startAudio();
        } else if (action == "playing") {
            if (!outputActive()) {
                startAudio();
            }
            if (outputActive()) {
                encodeTimestamp(getTimestampFromPlaylist());
            }
        }
    }

    void startAudio() {
        if (!enabled) {
            return;
        }
        if (usePipeWireSource) {
#ifdef HAS_PIPEWIRE_SOURCE
            // The PipeWire source node persists for the whole fppd session
            // (created in enableOutput) so downstream routing stays linked;
            // playlist start just resets the encoder timeline.
            if (!pwStream) {
                LogInfo(VB_PLUGIN, "SMPTE - PipeWire source not available\n");
                return;
            }
            lastFrame = 0;
            {
                std::lock_guard<std::mutex> lk(pwRingMutex);
                pwRing.clear();
            }
#else
            return;
#endif
        } else if (!audioStream) {
            lastFrame = 0;
            std::string dev = settings["SMPTEOutputDevice"];
            if (dev == "") {
                LogInfo(VB_PLUGIN, "SMPTE - No Output Audio Device selected\n");
                return;
            }

            // SDL3 streams resample from our spec to the device's native rate,
            // so we always produce SMPTE_SAMPLE_RATE audio and let SDL convert.
            // libltc produces unsigned 8 bit mono samples.
            SDL_AudioSpec want;
            SDL_memset(&want, 0, sizeof(want));
            want.freq = SMPTE_SAMPLE_RATE;
            want.format = SDL_AUDIO_U8;
            want.channels = 1;

            SDL_AudioDeviceID devId = findAudioDevice(dev, false);
            if (devId == 0) {
                LogInfo(VB_PLUGIN, "SMPTE - Could not find Output Audio Device: %s\n", dev.c_str());
                return;
            }
            SDL_ClearError();
            audioStream = SDL_OpenAudioDeviceStream(devId, &want, nullptr, nullptr);
            if (!audioStream) {
                LogInfo(VB_PLUGIN, "SMPTE - Could not open Output Audio Device: %s   Error: %s\n", dev.c_str(), SDL_GetError());
                return;
            }
            // Streams open paused; start the device pulling from the stream.
            SDL_ResumeAudioStreamDevice(audioStream);
        } else {
            return;
        }

        ltc_encoder_set_buffersize(ltcEncoder, SMPTE_SAMPLE_RATE, framerate);
        ltc_encoder_reinit(ltcEncoder, SMPTE_SAMPLE_RATE, framerate,
                framerate==25?LTC_TV_625_50:LTC_TV_525_60, 0);
        ltc_encoder_set_filter(ltcEncoder, 0);
        //ltc_encoder_set_filter(ltcEncoder, 25.0);
        //ltc_encoder_set_volume(ltcEncoder, -18.0);

        encodeTimestamp(0);
    }
    void stopAudio() {
        if (audioStream) {
            SDL_DestroyAudioStream(audioStream);
            audioStream = nullptr;
        }
#ifdef HAS_PIPEWIRE_SOURCE
        if (pwStream) {
            // Keep the node; just drop any pending LTC so the output goes
            // silent until the next playlist starts.
            std::lock_guard<std::mutex> lk(pwRingMutex);
            pwRing.clear();
        }
#endif
        lastFrame = 0;
    }

#ifdef HAS_PIPEWIRE_SOURCE
    static void onPwProcess(void *userdata) {
        FPPSMPTEPlugin *p = (FPPSMPTEPlugin*)userdata;
        struct pw_buffer *b = pw_stream_dequeue_buffer(p->pwStream);
        if (!b) {
            return;
        }
        struct spa_buffer *buf = b->buffer;
        uint8_t *dst = (uint8_t*)buf->datas[0].data;
        if (!dst) {
            pw_stream_queue_buffer(p->pwStream, b);
            return;
        }
        uint32_t want = buf->datas[0].maxsize;
        if (b->requested > 0 && b->requested < want) {
            want = (uint32_t)b->requested;   // frames == bytes for U8 mono
        }
        uint32_t filled = 0;
        uint32_t backlog = 0;
        {
            std::lock_guard<std::mutex> lk(p->pwRingMutex);
            backlog = (uint32_t)p->pwRing.size();
            filled = backlog < want ? backlog : want;
            if (filled) {
                memcpy(dst, p->pwRing.data(), filled);
                p->pwRing.erase(p->pwRing.begin(), p->pwRing.begin() + filled);
            }
        }
        if (filled < want) {
            memset(dst + filled, 0x80, want - filled);   // U8 silence
        }
        buf->datas[0].chunk->offset = 0;
        buf->datas[0].chunk->stride = 1;
        buf->datas[0].chunk->size = want;
        pw_stream_queue_buffer(p->pwStream, b);

        // Auto latency: time from a sample entering our ring to it being
        // presented downstream.  pw_time.delay covers the graph path to the
        // device (incl. loopbacks/filters); add our ring backlog and any
        // resampler-buffered frames.  Exponentially smoothed to keep the
        // encoded timecode from jittering.
        struct pw_time t;
        if (pw_stream_get_time_n(p->pwStream, &t, sizeof(t)) == 0 && t.rate.denom > 0) {
            int64_t samples = backlog + (int64_t)t.buffered +
                              t.delay * t.rate.num * (int64_t)SMPTE_SAMPLE_RATE / t.rate.denom;
            int64_t ms = samples * 1000 / SMPTE_SAMPLE_RATE;
            int64_t cur = p->pwAutoLatencyMS;
            p->pwAutoLatencyMS = cur + (ms - cur) / 8;
        }
    }
    static void onPwStateChanged(void *userdata, enum pw_stream_state old,
                                 enum pw_stream_state state, const char *error) {
        LogDebug(VB_PLUGIN, "SMPTE - PipeWire source state: %s%s%s\n",
                 pw_stream_state_as_string(state),
                 error ? " error: " : "", error ? error : "");
    }

    bool startPipeWireSource() {
        // fppd's PipeWire daemon runs with its own runtime dir; make sure we
        // connect to it (no-op if fppd already set these).
        setenv("PIPEWIRE_RUNTIME_DIR", "/run/pipewire-fpp", 0);
        setenv("XDG_RUNTIME_DIR", "/run/pipewire-fpp", 0);

        pw_init(nullptr, nullptr);
        pwLoop = pw_thread_loop_new("fpp-smpte-ltc", nullptr);
        if (!pwLoop) {
            LogWarn(VB_PLUGIN, "SMPTE - Could not create PipeWire thread loop\n");
            return false;
        }
        if (pw_thread_loop_start(pwLoop) != 0) {
            LogWarn(VB_PLUGIN, "SMPTE - Could not start PipeWire thread loop\n");
            pw_thread_loop_destroy(pwLoop);
            pwLoop = nullptr;
            return false;
        }
        pw_thread_loop_lock(pwLoop);
        struct pw_properties *props = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Playback",
            PW_KEY_MEDIA_ROLE, "Production",
            PW_KEY_MEDIA_CLASS, "Audio/Source",
            PW_KEY_NODE_NAME, SMPTE_PW_NODE_NAME,
            PW_KEY_NODE_DESCRIPTION, "FPP SMPTE LTC Timecode",
            PW_KEY_NODE_VIRTUAL, "true",
            "node.autoconnect", "false",
            "node.always-process", "true",
            nullptr);
        static const struct pw_stream_events streamEvents = {
            .version = PW_VERSION_STREAM_EVENTS,
            .state_changed = onPwStateChanged,
            .process = onPwProcess,
        };
        pwStream = pw_stream_new_simple(pw_thread_loop_get_loop(pwLoop),
                                        "fpp-smpte-ltc", props, &streamEvents, this);
        if (!pwStream) {
            pw_thread_loop_unlock(pwLoop);
            LogWarn(VB_PLUGIN, "SMPTE - Could not create PipeWire stream\n");
            stopPipeWireSource();
            return false;
        }
        uint8_t podBuffer[1024];
        struct spa_pod_builder podB = SPA_POD_BUILDER_INIT(podBuffer, sizeof(podBuffer));
        struct spa_audio_info_raw info = {};
        info.format = SPA_AUDIO_FORMAT_U8;
        info.rate = SMPTE_SAMPLE_RATE;
        info.channels = 1;
        info.position[0] = SPA_AUDIO_CHANNEL_MONO;
        const struct spa_pod *params[1];
        params[0] = spa_format_audio_raw_build(&podB, SPA_PARAM_EnumFormat, &info);
        int res = pw_stream_connect(pwStream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                                    (enum pw_stream_flags)(PW_STREAM_FLAG_MAP_BUFFERS),
                                    params, 1);
        pw_thread_loop_unlock(pwLoop);
        if (res < 0) {
            LogWarn(VB_PLUGIN, "SMPTE - Could not connect PipeWire stream: %s\n", strerror(-res));
            stopPipeWireSource();
            return false;
        }
        LogInfo(VB_PLUGIN, "SMPTE - Publishing LTC as PipeWire source node '%s'\n", SMPTE_PW_NODE_NAME);
        return true;
    }
    void stopPipeWireSource() {
        if (pwStream) {
            pw_thread_loop_lock(pwLoop);
            pw_stream_destroy(pwStream);
            pwStream = nullptr;
            pw_thread_loop_unlock(pwLoop);
        }
        if (pwLoop) {
            pw_thread_loop_stop(pwLoop);
            pw_thread_loop_destroy(pwLoop);
            pwLoop = nullptr;
        }
    }
#endif

    bool enableOutput() {
        if (getFPPmode() & PLAYER_MODE) {
            usePipeWireSource = settings["SMPTEOutputPipeWireSource"] == "1";
#ifndef HAS_PIPEWIRE_SOURCE
            if (usePipeWireSource) {
                LogWarn(VB_PLUGIN, "SMPTE - Built without PipeWire support, falling back to output device\n");
                usePipeWireSource = false;
            }
#endif
            std::string off = settings["SMPTEOutputOffsetMS"];
            if (!off.empty()) {
                outputOffsetMS = std::atoi(off.c_str());
            }
            std::string dev = settings["SMPTEOutputDevice"];
            if (!usePipeWireSource && dev == "") {
                LogInfo(VB_PLUGIN, "SMPTE - No Output Audio Device selected\n");
                return false;
            }
            enabled = true;

            LTC_TV_STANDARD tvCode;
            if (framerate == 25) {
                tvCode = LTC_TV_625_50;
            } else if (framerate == 24) {
                tvCode = LTC_TV_FILM_24;
            } else {
                tvCode = LTC_TV_525_60;
            }
            ltcEncoder = ltc_encoder_create(48000, framerate, tvCode, 0);
            ltc_encoder_set_timecode(ltcEncoder, &outputTimeCode);

#ifdef HAS_PIPEWIRE_SOURCE
            if (usePipeWireSource) {
                if (startPipeWireSource()) {
#ifdef HAS_AUDIO_SOURCE_REGISTRY
                    AudioSourceRegistry::AudioSource src;
                    src.id = "fpp-smpte:ltc";
                    src.name = "SMPTE LTC Timecode";
                    src.nodeName = SMPTE_PW_NODE_NAME;
                    src.plugin = "fpp-smpte";
                    src.channels = 1;
                    src.sampleRate = SMPTE_SAMPLE_RATE;
                    AudioSourceRegistry::INSTANCE.registerSource(src);
#endif
                } else {
                    LogWarn(VB_PLUGIN, "SMPTE - PipeWire source unavailable; timecode output disabled\n");
                }
            }
#endif

            MultiSync::INSTANCE.addMultiSyncPlugin(this);
            return true;
        }
        return false;
    }

    
    static uint32_t getUserBits(LTCFrame *f){
        uint32_t data = 0;
        data += f->user8;
        data <<= 4;
        data += f->user7;
        data <<= 4;
        data += f->user6;
        data <<= 4;
        data += f->user5;
        data <<= 4;
        data += f->user4;
        data <<= 4;
        data += f->user3;
        data <<= 4;
        data += f->user2;
        data <<= 4;
        data += f->user1;
        return data;
    }
    
    
    uint64_t lastMS = 0;
    // SDL3 recording callback: drain everything currently available from the
    // device stream and feed it to the libltc decoder.
    static void SDLCALL InputAudioCallback(void *userdata, SDL_AudioStream *stream,
                                           int additional_amount, int total_amount) {
        FPPSMPTEPlugin *p = (FPPSMPTEPlugin*)userdata;
        Uint8 buf[4096];
        int got = SDL_GetAudioStreamData(stream, buf, sizeof(buf));
        while (got > 0) {
            decodeInputAudio(p, buf, got);
            got = SDL_GetAudioStreamData(stream, buf, sizeof(buf));
        }
    }
    static void decodeInputAudio(FPPSMPTEPlugin *p, Uint8* audioData, int len) {
        ltc_decoder_write_u16(p->ltcDecoder, (uint16_t*)audioData, len / 2, p->decoderPos);
        p->decoderPos += len;
        LTCFrameExt frame;
        while (ltc_decoder_read(p->ltcDecoder, &frame)) {
            SMPTETimecode stime;
            ltc_frame_to_time(&stime, &frame.ltc, 1);
            uint64_t msTimeStamp = ((stime.hours * 3600) + (stime.mins * 60) + stime.secs) * 1000;
            float f = stime.frame;
            f /= p->framerate;
            f *= 1000;
            uint64_t oms = f;
            msTimeStamp += oms;

            uint64_t df = msTimeStamp > p->lastMS ? (msTimeStamp - p->lastMS) : (p->lastMS - msTimeStamp);
            if (df > 0 && df < 5000 && p->inputEventFileWrite >= 0) {                
                //printf("msTimeStamp: %d     frame: %d\n", (int)msTimeStamp, (int)stime.frame);
                int32_t idx = 0;
                if (p->timeCodePType == TimeCodeProcessingType::HOUR) {
                    constexpr int DIV = 1000 * 60 * 60;
                    idx = msTimeStamp / DIV;
                    msTimeStamp %= DIV;
                } else if (p->timeCodePType == TimeCodeProcessingType::MIN15) {
                    constexpr int DIV = 1000 * 60 * 15;
                    idx = msTimeStamp / DIV;
                    msTimeStamp %= DIV;
                } else if (p->timeCodePType == TimeCodeProcessingType::PLAYLIST_ITEM_DEFINED) {
                    idx = -2;
                } else {
                    idx = -1;
                }
                if (oms == 0 && stime.hours == 0 && stime.mins == 0 && stime.secs == 0) {
                    idx = -99;
                }
                p->currentPosMS = msTimeStamp;
                p->currentUserBits =  getUserBits(&frame.ltc);
                p->currentIdx = idx;
                //printf("Frame: h: %d     m: %d    s:   %d    f: %d       ts: %d\n", stime.hours, stime.mins, stime.secs, stime.frame, (int)msTimeStamp);
                write(p->inputEventFileWrite, &msTimeStamp, sizeof(msTimeStamp));
            }
            p->lastMS = msTimeStamp;
        }
    }
    bool enableInput() {
        std::string dev = settings["SMPTEInputDevice"];
        if (dev == "") {
            LogInfo(VB_PLUGIN, "SMPTE - No Input Audio Device selected\n");
            return false;
        }
        ltcDecoder = ltc_decoder_create(1920, 16);

        SDL_AudioSpec want;
        SDL_memset(&want, 0, sizeof(want));
        want.freq = SMPTE_SAMPLE_RATE;
        want.format = SDL_AUDIO_S16;
        want.channels = 1;

        SDL_AudioDeviceID devId = findAudioDevice(dev, true);
        if (devId == 0) {
            LogInfo(VB_PLUGIN, "SMPTE - Could not find Input Audio Device: %s\n", dev.c_str());
            return false;
        }
        SDL_ClearError();
        audioStream = SDL_OpenAudioDeviceStream(devId, &want, InputAudioCallback, this);
        if (!audioStream) {
            LogInfo(VB_PLUGIN, "SMPTE - Could not open Input Audio Device: %s   Error: %s\n", dev.c_str(), SDL_GetError());
            return false;
        }
        // Streams open paused; start the device feeding the stream.
        SDL_ResumeAudioStreamDevice(audioStream);
        return true;
    }

    // Resolve a stored device name (from FPP's aplay-derived AudioOutputList /
    // AudioInputList) to an SDL3 device id. SDL3 opens devices by id, so we
    // enumerate and match by name (exact, then substring since the stored name
    // may not be byte-identical to SDL3's device name). Returns 0 if not found.
    static SDL_AudioDeviceID findAudioDevice(const std::string &name, bool recording) {
        int count = 0;
        SDL_AudioDeviceID *devs = recording ? SDL_GetAudioRecordingDevices(&count)
                                            : SDL_GetAudioPlaybackDevices(&count);
        if (!devs) {
            return 0;
        }
        SDL_AudioDeviceID found = 0;
        for (int i = 0; i < count && !found; i++) {
            const char *dn = SDL_GetAudioDeviceName(devs[i]);
            if (dn && name == dn) {
                found = devs[i];
            }
        }
        for (int i = 0; i < count && !found; i++) {
            const char *dn = SDL_GetAudioDeviceName(devs[i]);
            if (dn && (name.find(dn) != std::string::npos || std::string(dn).find(name) != std::string::npos)) {
                found = devs[i];
            }
        }
        if (!found) {
            LogWarn(VB_PLUGIN, "SMPTE - Audio device '%s' not found. Available %s devices:\n",
                    name.c_str(), recording ? "input" : "output");
            for (int i = 0; i < count; i++) {
                const char *dn = SDL_GetAudioDeviceName(devs[i]);
                LogWarn(VB_PLUGIN, "SMPTE -   %s\n", dn ? dn : "(null)");
            }
        }
        SDL_free(devs);
        return found;
    }

    virtual void addControlCallbacks(std::map<int, std::function<bool(int)>> &callbacks) override {
        if (settings["SMPTETimeCodeEnabled"] == "1") {
            framerate = std::stof(settings["SMPTETimeCodeType"]);

            std::string tcpt = settings["SMPTETimeCodeProcessing"];
            if (tcpt == "1") {
                timeCodePType = TimeCodeProcessingType::HOUR;
            } else if (tcpt == "2") {
                timeCodePType = TimeCodeProcessingType::MIN15;
            } else if (tcpt == "3") {
                timeCodePType = TimeCodeProcessingType::PLAYLIST_ITEM_DEFINED;
            } else {
                timeCodePType = TimeCodeProcessingType::PLAYLIST_POS;
            }            
            if (getFPPmode() == REMOTE_MODE) {
                if (enableInput()) {
                    actAsMaster = settings["SMPTEResendMultisync"] == "1";
#ifndef PLATFORM_OSX
                    inputEventFileRead = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
                    inputEventFileWrite = inputEventFileRead;
#else
                    int files[2];
                    pipe(files);
                    inputEventFileRead = files[0];
                    inputEventFileWrite = files[1];
                    fcntl(inputEventFileRead, F_SETFD, O_NONBLOCK);
                    fcntl(inputEventFileWrite, F_SETFD, O_NONBLOCK);
#endif
                    callbacks[inputEventFileRead] = [this](int i) {
                        uint64_t ts;
                        ssize_t s = read(i, &ts, sizeof(ts));
                        while (s > 0) {
                            s = read(i, &ts, sizeof(ts));
                        }
                        
                        std::string pl = "";
                        std::string f = "smpte-pl-" + std::to_string(currentUserBits);
                        if (FileExists(FPP_DIR_PLAYLIST(f + ".json"))) {
                            pl = f;
                        }
                        if (pl == "") {
                            pl = settings["SMPTEInputPlaylist"];
                        }
                        if (pl == "--none--") {
                            pl = "";
                        }
                        if (pl != "") {
                            uint64_t ms = currentPosMS;
                            int32_t idx = currentIdx;
                            if (idx == -99) {
                                MultiSync::INSTANCE.SyncStopAll();
                            } else {
                                MultiSync::INSTANCE.SyncPlaylistToMS(ms, idx, pl, actAsMaster);
                            }
                        }
                        return false;
                    };
                }
            } else {
                enableOutput();
            }
        }
    }
    
    
    void setDefaultSettings() {
        setIfNotFound("SMPTETimeCodeEnabled", "0");
        setIfNotFound("SMPTEOutputDevice", "");
        setIfNotFound("SMPTEOutputPipeWireSource", "0");
        setIfNotFound("SMPTEOutputOffsetMS", "0");
        setIfNotFound("SMPTEInputDevice", "");
        setIfNotFound("SMPTETimeCodeType", "30");
        setIfNotFound("SMPTEInputPlaylist", "");
        setIfNotFound("SMPTEResendMultisync", "0");
        setIfNotFound("SMPTETimeCodeProcessing", "0");
    }
    void setIfNotFound(const std::string &s, const std::string &v, bool emptyAllowed = false) {
        if (settings.find(s) == settings.end()) {
            settings[s] = v;
        } else if (!emptyAllowed && settings[s] == "") {
            settings[s] = v;
        }
    }
    
    
    static constexpr int SMPTE_SAMPLE_RATE = 48000;
    static constexpr const char *SMPTE_PW_NODE_NAME = "fpp_smpte_ltc";
    SDL_AudioStream *audioStream = nullptr;

    bool usePipeWireSource = false;
    int outputOffsetMS = 0;      // manual trim from SMPTEOutputOffsetMS
#ifdef HAS_PIPEWIRE_SOURCE
    struct pw_thread_loop *pwLoop = nullptr;
    struct pw_stream *pwStream = nullptr;
    std::vector<uint8_t> pwRing;
    std::mutex pwRingMutex;
    std::atomic<int64_t> pwAutoLatencyMS{0};
#endif

    bool        enabled = false;
    LTCDecoder *ltcDecoder = nullptr;
    ltc_off_t  decoderPos = 0;
    int        inputEventFileRead = -1;
    int        inputEventFileWrite = -1;
    std::atomic<uint64_t> currentPosMS = 0;
    std::atomic<uint32_t> currentUserBits = 0;
    std::atomic<int32_t>  currentIdx = 0;
    bool       actAsMaster = false;
    

    float      framerate = 30.0f;
    enum class TimeCodeProcessingType {
        PLAYLIST_POS,
        HOUR,
        MIN15,
        PLAYLIST_ITEM_DEFINED
    } timeCodePType;

    LTCEncoder *ltcEncoder = nullptr;
    SMPTETimecode outputTimeCode;
    uint64_t  positionMSOffset = 0;
    uint64_t lastFrame = 0;
};


extern "C" {
    FPPPlugin *createPlugin() {
        return new FPPSMPTEPlugin();
    }
}
