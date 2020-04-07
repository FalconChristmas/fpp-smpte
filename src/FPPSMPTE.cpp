#include "fpp-pch.h"

#include <ltc.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_audio.h>
#include <sys/eventfd.h>

#include "FPPSMPTE.h"
#include "Plugin.h"
#include "MultiSync.h"
#include "playlist/Playlist.h"


class FPPSMPTEPlugin : public FPPPlugin, public MultiSyncPlugin {
    
public:
    FPPSMPTEPlugin() : FPPPlugin("fpp-smpte"), audioDev(0), outputBufferSize(0) {
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
        if (outputBuffer) {
            free(outputBuffer);
        }
    }
    
    void encodeTimestamp(uint64_t ms) {
        uint64_t oms = ms;
        uint64_t sf = ms % 1000;
        ms /= 1000;
        sf *= outputFramerate;
        sf /= 1000;
        if (outputTimeCode.frame == sf && ((ms % 60) == outputTimeCode.secs)) {
            //duplicate, return
            return;
        }
        outputTimeCode.frame = sf;
        outputTimeCode.secs = ms % 60;
        ms /= 60;
        outputTimeCode.mins = ms % 60;
        ms /= 60;
        outputTimeCode.hours = ms;
        ltc_encoder_set_timecode(ltcEncoder, &outputTimeCode);
        ltc_encoder_encode_frame(ltcEncoder);
        int len = ltc_encoder_get_buffer(ltcEncoder, outputBuffer);
        //printf("TS: %lld     len: %d\n", oms, len);
        
        outputBufferSize = len;
    }
    
    virtual void SendSeqSyncPacket(const std::string &filename, int frames, float seconds) override {
        uint64_t ms = playlist->GetCurrentPosInMS();
        encodeTimestamp(ms);
    }
    virtual void SendMediaSyncPacket(const std::string &filename, float seconds) {
        uint64_t ms = playlist->GetCurrentPosInMS();
        encodeTimestamp(ms);
    }
    
    virtual void playlistCallback(const Json::Value &plj, const std::string &action, const std::string &section, int item) override {
        if (action == "stop" || action == "start") {
            encodeTimestamp(0);
        } else if (action == "playing") {
            int pos = playlist->GetPosition();
            positionMSOffset = playlist->GetPosStartInMS(pos);
            encodeTimestamp(positionMSOffset);
        }
    }

    static void OutputAudioCallback(FPPSMPTEPlugin *p, Uint8* stream, int len) {
        memset(stream, 0, len);
        
        uint32_t bufSize = p->outputBufferSize.exchange(0);
        if (bufSize && (bufSize < len)) {
            //printf("Buf %d  (%d)\n", bufSize, len);
            memcpy(stream, p->outputBuffer, bufSize);
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
            
            SDL_AudioSpec want;
            SDL_AudioSpec obtained;
            
            SDL_memset(&want, 0, sizeof(want));
            SDL_memset(&obtained, 0, sizeof(obtained));
            want.freq = 44100;
            want.format = AUDIO_S16SYS;
            want.channels = 1;
            want.samples = 2048;
            want.callback = (SDL_AudioCallback)OutputAudioCallback;
            want.userdata = this;

            audioDev = SDL_OpenAudioDevice(dev.c_str(), 0, &want, &obtained, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
            if (audioDev < 2) {
                LogInfo(VB_PLUGIN, "SMPTE - Could not open Input Audio Device: %c\n", dev.c_str());
                return false;
            }
            SDL_ClearError();
            SDL_AudioStatus as = SDL_GetAudioDeviceStatus(audioDev);
            if (as == SDL_AUDIO_PAUSED) {
                SDL_PauseAudioDevice(audioDev, 0);
            }
            
            LTC_TV_STANDARD tvCode;
            if (outputFramerate == 25) {
                tvCode = LTC_TV_625_50;
            } else if (outputFramerate == 24) {
                tvCode = LTC_TV_FILM_24;
            } else {
                tvCode = LTC_TV_525_60;
            }
            ltcEncoder = ltc_encoder_create(obtained.freq, outputFramerate,
                                            tvCode, 0);
            ltc_encoder_set_timecode(ltcEncoder, &outputTimeCode);
            int bufSize = ltc_encoder_get_buffersize(ltcEncoder);
            outputBuffer = (ltcsnd_sample_t *)calloc(bufSize, sizeof(ltcsnd_sample_t));
            
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

            //printf("msTimeStamp: %d\n", (int)msTimeStamp);
            uint64_t df = msTimeStamp > p->lastMS ? (msTimeStamp - p->lastMS) : (p->lastMS - msTimeStamp);
            if (df < 5000 && p->inputEventFile >= 0) {
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
        want.samples = 4096;
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
                if (FileExists("/home/fpp/media/playlists" + f + ".json")) {
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
    ltcsnd_sample_t * outputBuffer = nullptr;
    std::atomic<uint32_t>  outputBufferSize;
    uint64_t  positionMSOffset = 0;
};


extern "C" {
    FPPPlugin *createPlugin() {
        return new FPPSMPTEPlugin();
    }
}
