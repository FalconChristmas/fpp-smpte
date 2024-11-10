#include <fpp-pch.h>

#include <ltc.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_audio.h>
#ifndef PLATFORM_OSX
#include <sys/eventfd.h>
#endif
#include <cinttypes>

#include "FPPSMPTE.h"
#include "Plugin.h"
#include "MultiSync.h"
#include "playlist/Playlist.h"
#include "channeloutput/channeloutputthread.h"


class FPPSMPTEPlugin : public FPPPlugin, public MultiSyncPlugin {
    
public:
    FPPSMPTEPlugin() : FPPPlugin("fpp-smpte"), audioDev(0) {
        LogInfo(VB_PLUGIN, "Initializing SMPTE Plugin\n");
        setDefaultSettings();
        SDL_Init(SDL_INIT_AUDIO);
        
        memset(&outputTimeCode, 0, sizeof(outputTimeCode));
    }
    virtual ~FPPSMPTEPlugin() {
        MultiSync::INSTANCE.removeMultiSyncPlugin(this);
        if (audioDev > 1) {
            SDL_PauseAudioDevice(audioDev, 1);
            SDL_CloseAudioDevice(audioDev);
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
    
    void encodeTimestamp(uint64_t ms) {
        int len = SDL_GetQueuedAudioSize(audioDev);
        if (len > 2048) {
            return;
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
            SDL_QueueAudio(audioDev, buf, len);
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
            SDL_QueueAudio(audioDev, buf, len);
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
        if (audioDev > 1) {
            encodeTimestamp(getTimestampFromPlaylist());
        }
    }
    virtual void SendMediaSyncPacket(const std::string &filename, float seconds) override {
        if (audioDev > 1) {
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
            if (audioDev <= 1) {
                startAudio();
            }
            if (audioDev > 1) {
                encodeTimestamp(getTimestampFromPlaylist());
            }
        }
    }
    
    void startAudio() {
        if (audioDev <= 1 && enabled) {
            lastFrame = 0;
            std::string dev = settings["SMPTEOutputDevice"];
            if (dev == "") {
                LogInfo(VB_PLUGIN, "SMPTE - No Output Audio Device selected\n");
                return;
            }
            
            SDL_AudioSpec want;
            SDL_AudioSpec obtained;
            
            SDL_memset(&want, 0, sizeof(want));
            SDL_memset(&obtained, 0, sizeof(obtained));
            want.freq = 48000;
            want.format = AUDIO_S16SYS;
            want.channels = 1;
            want.samples = 1048;
            want.callback = nullptr;

            /*
            int count = SDL_GetNumAudioDevices(0);
            for (int x = 0; x < count; x++) {
                std::string de = SDL_GetAudioDeviceName(x, 0);
                printf("Audio device %d: %s        %s\n", x, de.c_str(), (dev == de ? "true" : "false"));
            }
            */
           
            SDL_ClearError();
            audioDev = SDL_OpenAudioDevice(dev.c_str(), 0, &want, &obtained, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
            if (audioDev < 2) {
                LogInfo(VB_PLUGIN, "SMPTE - Could not open Output Audio Device(%d): %s\nError: %s\n", audioDev, dev.c_str(), SDL_GetError());
                return;
            }
            SDL_ClearError();
            SDL_AudioStatus as = SDL_GetAudioDeviceStatus(audioDev);
            if (as == SDL_AUDIO_PAUSED) {
                SDL_PauseAudioDevice(audioDev, 0);
            }
            
            ltc_encoder_set_buffersize(ltcEncoder, obtained.freq, framerate);
            ltc_encoder_reinit(ltcEncoder, obtained.freq, framerate,
                    framerate==25?LTC_TV_625_50:LTC_TV_525_60, 0);
            ltc_encoder_set_filter(ltcEncoder, 0);
            //ltc_encoder_set_filter(ltcEncoder, 25.0);
            //ltc_encoder_set_volume(ltcEncoder, -18.0);

            encodeTimestamp(0);
        }
    }
    void stopAudio() {
        if (audioDev > 1) {
            SDL_PauseAudioDevice(audioDev, 1);
            SDL_CloseAudioDevice(audioDev);
            audioDev = 0;
            lastFrame = 0;
        }
    }
    
    bool enableOutput() {
        if (getFPPmode() & PLAYER_MODE) {
            std::string dev = settings["SMPTEOutputDevice"];
            if (dev == "") {
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
    static void InputAudioCallback(FPPSMPTEPlugin *p, Uint8* stream, int len) {
        ltc_decoder_write_u16(p->ltcDecoder, (uint16_t*)stream, len / 2, p->decoderPos);
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
        SDL_AudioSpec obtained;
        
        SDL_memset(&want, 0, sizeof(want));
        SDL_memset(&obtained, 0, sizeof(obtained));
        want.freq = 48000;
        want.format = AUDIO_S16SYS;
        want.channels = 1;
        want.samples = 1024;
        want.callback = (SDL_AudioCallback)InputAudioCallback;
        want.userdata = this;
        audioDev = SDL_OpenAudioDevice(dev.c_str(), 1, &want, &obtained, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
        if (audioDev < 2) {
            LogInfo(VB_PLUGIN, "SMPTE - Could not open Input Audio Device: %c\n", dev.c_str());
            return false;
        }
        SDL_ClearError();
        SDL_AudioStatus as = SDL_GetAudioDeviceStatus(audioDev);
        if (as == SDL_AUDIO_PAUSED) {
            SDL_PauseAudioDevice(audioDev, 0);
        }
        return true;
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
    
    
    SDL_AudioDeviceID audioDev = 0;
    
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
