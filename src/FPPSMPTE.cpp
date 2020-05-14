#include "fpp-pch.h"

#include <ltc.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_audio.h>
#include <sys/eventfd.h>

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
        if (settings["EnableSMPTEOutput"] == "1") {
            enableOutput();
        }
    }
    virtual ~FPPSMPTEPlugin() {
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
        if (inputEventFile >= 0) {
            close(inputEventFile);
        }
    }
    
    uint64_t lastFrame;
    void encodeTimestamp(uint64_t ms) {
        int len = SDL_GetQueuedAudioSize(audioDev);
        if (len > 2048) {
            return;
        }
        
        uint64_t frame = ms;
        frame *= outputFramerate;
        frame /= 1000;
        
        if (lastFrame != frame) {
            if (lastFrame == (frame - 1)) {
                ltc_encoder_inc_timecode(ltcEncoder);
            } else {
                uint64_t sf = ms % 1000;
                ms /= 1000;
                sf *= outputFramerate;
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
            ltcsnd_sample_t *buf = ltc_encoder_get_bufptr(ltcEncoder, &len, 1);
            SDL_QueueAudio(audioDev, buf, len);
        }
        int i = GetChannelOutputRefreshRate();
        ms += (1000/i);
        frame = ms;
        frame *= outputFramerate;
        frame /= 1000;
        if ((lastFrame+1) < frame) {
            //next frame will be skipped, queue it now
            ltc_encoder_inc_timecode(ltcEncoder);
            lastFrame++;
            ltc_encoder_encode_frame(ltcEncoder);
            ltcsnd_sample_t *buf = ltc_encoder_get_bufptr(ltcEncoder, &len, 1);
            SDL_QueueAudio(audioDev, buf, len);
        }
    }
    
    virtual void SendSeqSyncPacket(const std::string &filename, int frames, float seconds) override {
        if (audioDev > 1) {
            uint64_t ms = playlist->GetCurrentPosInMS();
            encodeTimestamp(ms);
        }
    }
    virtual void SendMediaSyncPacket(const std::string &filename, float seconds) {
        if (audioDev > 1) {
            uint64_t ms = playlist->GetCurrentPosInMS();
            encodeTimestamp(ms);
        }
    }
    
    virtual void playlistCallback(const Json::Value &plj, const std::string &action, const std::string &section, int item) override {
        if (action == "stop") {
            stopAudio();
        } else if (action == "start") {
            startAudio();
        } else if (action == "playing") {
            if (audioDev <= 1) {
                startAudio();
            }
            if (audioDev > 1) {
                int pos = playlist->GetPosition();
                positionMSOffset = playlist->GetPosStartInMS(pos);
                encodeTimestamp(positionMSOffset);
            }
        }
    }
    
    void startAudio() {
        if (audioDev <= 1) {
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

            audioDev = SDL_OpenAudioDevice(dev.c_str(), 0, &want, &obtained, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
            if (audioDev < 2) {
                LogInfo(VB_PLUGIN, "SMPTE - Could not open Output Audio Device: %c\n", dev.c_str());
                return;
            }
            SDL_ClearError();
            SDL_AudioStatus as = SDL_GetAudioDeviceStatus(audioDev);
            if (as == SDL_AUDIO_PAUSED) {
                SDL_PauseAudioDevice(audioDev, 0);
            }
            
            ltc_encoder_set_bufsize(ltcEncoder, obtained.freq, outputFramerate);
            ltc_encoder_reinit(ltcEncoder, obtained.freq, outputFramerate,
                    outputFramerate==25?LTC_TV_625_50:LTC_TV_525_60, 0);
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
        }
    }
    
    bool enableOutput() {
        if (getFPPmode() & PLAYER_MODE) {
            std::string dev = settings["SMPTEOutputDevice"];
            if (dev == "") {
                LogInfo(VB_PLUGIN, "SMPTE - No Output Audio Device selected\n");
                return false;
            }
            outputFramerate = std::stof(settings["SMPTEOutputFrameRate"]);
            
            
            LTC_TV_STANDARD tvCode;
            if (outputFramerate == 25) {
                tvCode = LTC_TV_625_50;
            } else if (outputFramerate == 24) {
                tvCode = LTC_TV_FILM_24;
            } else {
                tvCode = LTC_TV_525_60;
            }
            ltcEncoder = ltc_encoder_create(48000, outputFramerate, tvCode, 0);
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
            f /= p->inputFramerate;
            f *= 1000;
            msTimeStamp += f;

            uint64_t df = msTimeStamp > p->lastMS ? (msTimeStamp - p->lastMS) : (p->lastMS - msTimeStamp);
            if (df > 0 && df < 5000 && p->inputEventFile >= 0) {
                //printf("msTimeStamp: %d     frame: %d\n", (int)msTimeStamp, (int)stime.frame);
                p->currentPosMS = msTimeStamp;
                p->currentUserBits =  getUserBits(&frame.ltc);
                //printf("Frame: h: %d     m: %d    s:   %d    f: %d       ts: %d\n", stime.hours, stime.mins, stime.secs, stime.frame, (int)msTimeStamp);
                write(p->inputEventFile, &msTimeStamp, sizeof(msTimeStamp));
            }
            p->lastMS = msTimeStamp;
        }
    }
    bool enableInput() {
        if (getFPPmode() != REMOTE_MODE) {
            return false;
        }
        std::string dev = settings["SMPTEInputDevice"];
        if (dev == "") {
            LogInfo(VB_PLUGIN, "SMPTE - No Input Audio Device selected\n");
            return false;
        }
        ltcDecoder = ltc_decoder_create(1920, 16);
        inputFramerate = std::stof(settings["SMPTEInputFrameRate"]);
        
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

    virtual void addControlCallbacks(std::map<int, std::function<bool(int)>> &callbacks) {
        if (settings["EnableSMPTEInput"] == "1" && enableInput()) {
            actAsMaster = settings["SMPTEResendMultisync"] == "1";
            inputEventFile = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
            callbacks[inputEventFile] = [this](int i) {
                uint64_t ts;
                ssize_t s = read(i, &ts, sizeof(ts));
                while (s > 0) {
                    s = read(i, &ts, sizeof(ts));
                }
                
                std::string pl = "";
                std::string f = "smpte-pl-" + std::to_string(currentUserBits);
                if (FileExists("/home/fpp/media/playlists/" + f + ".json")) {
                    pl = f;
                }
                if (pl == "") {
                    pl = settings["SMPTEInputPlaylist"];
                }
                if (pl == "--none--") {
                    pl = "";
                }
                MultiSync::INSTANCE.SyncPlaylistToMS(currentPosMS, pl, actAsMaster);
                return false;
            };
        }
    }
    
    
    void setDefaultSettings() {
        setIfNotFound("EnableSMPTEOutput", "0");
        setIfNotFound("EnableSMPTEInput", "0");
        setIfNotFound("SMPTEOutputDevice", "");
        setIfNotFound("SMPTEOutputFrameRate", "30");
        setIfNotFound("SMPTEInputDevice", "");
        setIfNotFound("SMPTEInputFrameRate", "30");
        setIfNotFound("SMPTEInputPlaylist", "--none--");
        setIfNotFound("SMPTEResendMultisync", "0");
    }
    void setIfNotFound(const std::string &s, const std::string &v, bool emptyAllowed = false) {
        if (settings.find(s) == settings.end()) {
            settings[s] = v;
        } else if (!emptyAllowed && settings[s] == "") {
            settings[s] = v;
        }
    }
    
    
    SDL_AudioDeviceID audioDev;
    
    LTCDecoder *ltcDecoder = nullptr;
    ltc_off_t  decoderPos = 0;
    float      inputFramerate = 30.0f;
    int        inputEventFile = -1;
    std::atomic<uint64_t> currentPosMS = 0;
    std::atomic<uint32_t> currentUserBits = 0;
    bool       actAsMaster = false;
    
    
    LTCEncoder *ltcEncoder = nullptr;
    float      outputFramerate = 30.0f;
    SMPTETimecode outputTimeCode;
    uint64_t  positionMSOffset = 0;
};


extern "C" {
    FPPPlugin *createPlugin() {
        return new FPPSMPTEPlugin();
    }
}
