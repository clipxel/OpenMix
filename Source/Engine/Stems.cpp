#include "Stems.h"
#include "SincResampler.h"
#include "../Library/Library.h"
#include "../Common/Strings.h"
#include <juce_dsp/juce_dsp.h>
#include <juce_cryptography/juce_cryptography.h>
#include <thread>

#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>

#if JUCE_MAC
 #include <pthread/qos.h>
#elif JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#else
 #include <sys/resource.h>
 #include <sys/syscall.h>
 #include <unistd.h>
#endif

namespace om
{

//==============================================================================
StemData::StemData (int numSamples, StemMode m)
    : n (juce::jmax (1, numSamples)), mode (m),
      numBlocks ((juce::jmax (1, numSamples) + blockLen - 1) / blockLen),
      ready (new std::atomic<bool>[(size_t) ((juce::jmax (1, numSamples) + blockLen - 1) / blockLen)])
{
    for (int b = 0; b < numBlocks; ++b)
        ready[(size_t) b].store (false);
    for (int p = 0; p < numStored; ++p)
    {
        parts[(size_t) p].setSize (2, n);
        parts[(size_t) p].clear();
        for (int ch = 0; ch < 2; ++ch)
        {
            write[(size_t) p][(size_t) ch] = parts[(size_t) p].getWritePointer (ch);
            read[(size_t) p][(size_t) ch] = write[(size_t) p][(size_t) ch];
        }
    }
}

//==============================================================================
namespace StemDsp
{
    static float demucsWeight (int unit, int i) noexcept
    {
        float w = 1.0f;
        if (unit > 0 && i < demucsOverlap) w = (float) i / (float) demucsOverlap;
        if (i >= demucsLen - demucsOverlap) w = (float) (demucsLen - i) / (float) demucsOverlap;
        return w;
    }

    static const std::vector<float>& hann()
    {
        static const std::vector<float> w = []
        {
            std::vector<float> v ((size_t) fftSize);
            for (int j = 0; j < fftSize; ++j)      // periodic, as the network was trained
                v[(size_t) j] = (float) (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * j / fftSize));
            return v;
        }();
        return w;
    }

    Plan::Plan (StemMode m, int samples) : mode (m), n (juce::jmax (1, samples))
    {
        if (mode == StemMode::quality)
        {
            numUnits = juce::jmax (1, (n + demucsStride - 1) / demucsStride);
            norm.assign ((size_t) n, 0.0f);
            for (int c = 0; c < numUnits; ++c)
            {
                const int s = c * demucsStride, len = juce::jmin (demucsLen, n - s);
                for (int i = 0; i < len; ++i)
                    norm[(size_t) (s + i)] += demucsWeight (c, i);
            }
        }
        else
        {
            numFrames = (pad + n + hop - 1) / hop;
            numUnits = (numFrames + splitFrames - 1) / splitFrames;
            const auto& w = hann();
            norm.assign ((size_t) ((numFrames - 1) * hop + fftSize), 0.0f);
            for (int f = 0; f < numFrames; ++f)
                for (int j = 0; j < fftSize; ++j)
                    norm[(size_t) (f * hop + j)] += w[(size_t) j] * w[(size_t) j];
        }
        for (auto& v : norm)
            v = v > 1.0e-8f ? 1.0f / v : 0.0f;
    }

    std::pair<int, int> Plan::range (int unit) const noexcept
    {
        if (mode == StemMode::quality)
        {
            const int s = unit * demucsStride;
            return { s, juce::jmin (n, s + demucsLen) };
        }
        const int f0 = unit * splitFrames, f1 = juce::jmin (numFrames, f0 + splitFrames);
        return { juce::jmax (0, f0 * hop - pad), juce::jmin (n, (f1 - 1) * hop + fftSize - pad) };
    }

    int Plan::unitAt (double sample) const noexcept
    {
        const int u = mode == StemMode::quality ? (int) (sample / demucsStride)
                                                : (int) ((sample + pad) / hop / splitFrames);
        return juce::jlimit (0, numUnits - 1, u);
    }

    void processUnit (const Plan& plan, int unit, const float* const* mix, StemData& out, const Nets& nets, std::mutex& addLock)
    {
        const int n = plan.n;
        if (plan.mode == StemMode::quality)
        {
            constexpr int N = demucsLen;
            thread_local std::vector<float> in, res;
            in.assign ((size_t) 2 * N, 0.0f);
            res.assign ((size_t) 8 * N, 0.0f);
            const int s = unit * demucsStride, len = juce::jmin (N, n - s);
            for (int ch = 0; ch < 2; ++ch)
                std::copy_n (mix[ch] + s, len, in.data() + (size_t) ch * N);
            nets.demucs (in.data(), res.data());

            static constexpr int source[StemData::numStored] { 3, 0, 1 };   // vocals, drums, bass in the model's order
            const std::lock_guard<std::mutex> l (addLock);
            for (int p = 0; p < StemData::numStored; ++p)
                for (int ch = 0; ch < 2; ++ch)
                {
                    const float* o = res.data() + (size_t) (source[p] * 2 + ch) * N;
                    float* d = out.write[(size_t) p][(size_t) ch] + s;
                    for (int i = 0; i < len; ++i)
                        d[i] += o[i] * demucsWeight (unit, i) * plan.norm[(size_t) (s + i)];
                }
            return;
        }

        constexpr int T = splitFrames, F = netBins, B = bins;
        thread_local std::unique_ptr<juce::dsp::FFT> fft;
        if (fft == nullptr) fft = std::make_unique<juce::dsp::FFT> (12);
        thread_local std::vector<std::complex<float>> spec;
        thread_local std::vector<float> netIn, buf, masks;
        thread_local std::array<std::vector<float>, 4> est;
        thread_local std::array<std::vector<float>, (size_t) StemData::numStored * 2> acc;
        spec.resize ((size_t) 2 * T * B);
        netIn.assign ((size_t) 2 * T * F, 0.0f);
        buf.resize ((size_t) 2 * fftSize);
        masks.resize ((size_t) StemData::numStored * B);
        const auto& W = hann();

        const int f0 = unit * T, nf = juce::jmin (plan.numFrames, f0 + T) - f0;
        for (int ch = 0; ch < 2; ++ch)
            for (int f = 0; f < nf; ++f)
            {
                const int q0 = (f0 + f) * hop;
                for (int j = 0; j < fftSize; ++j)
                {
                    const int t = q0 + j - pad;
                    buf[(size_t) j] = (t >= 0 && t < n ? mix[ch][t] : 0.0f) * W[(size_t) j];
                }
                std::fill (buf.begin() + fftSize, buf.end(), 0.0f);
                fft->performRealOnlyForwardTransform (buf.data(), true);
                auto* c = reinterpret_cast<const std::complex<float>*> (buf.data());
                std::copy_n (c, B, spec.data() + (size_t) (ch * T + f) * B);
                float* m = netIn.data() + (size_t) (ch * T + f) * F;
                for (int b = 0; b < F; ++b)
                    m[b] = std::abs (c[b]);
            }

        for (int s = 0; s < 4; ++s)
        {
            est[(size_t) s].resize ((size_t) 2 * T * F);
            nets.spleeter (s, netIn.data(), est[(size_t) s].data());
        }

        const auto [t0, t1] = plan.range (unit);
        for (auto& a : acc)
            a.assign ((size_t) juce::jmax (0, t1 - t0), 0.0f);

        for (int ch = 0; ch < 2; ++ch)
            for (int f = 0; f < nf; ++f)
            {
                const size_t row = (size_t) (ch * T + f) * F;
                std::array<double, StemData::numStored> mean {};
                for (int b = 0; b < F; ++b)
                {
                    float e2[4];
                    float den = 1.0e-10f;
                    for (int s = 0; s < 4; ++s)
                    {
                        const float e = est[(size_t) s][row + (size_t) b];
                        e2[s] = e * e;
                        den += e2[s];
                    }
                    for (int p = 0; p < StemData::numStored; ++p)
                    {
                        const float mk = (e2[p] + 1.0e-10f / 4.0f) / den;
                        masks[(size_t) (p * B + b)] = mk;
                        mean[(size_t) p] += mk;
                    }
                }
                for (int p = 0; p < StemData::numStored; ++p)          // above 16 kHz: the frame's mean mask
                    std::fill (masks.begin() + p * B + F, masks.begin() + (p + 1) * B, (float) (mean[(size_t) p] / F));

                const auto* c = spec.data() + (size_t) (ch * T + f) * B;
                const int q0 = (f0 + f) * hop;
                for (int p = 0; p < StemData::numStored; ++p)
                {
                    auto* y = reinterpret_cast<std::complex<float>*> (buf.data());
                    for (int b = 0; b < B; ++b)
                        y[b] = c[b] * masks[(size_t) (p * B + b)];
                    fft->performRealOnlyInverseTransform (buf.data());
                    auto& a = acc[(size_t) (p * 2 + ch)];
                    for (int j = 0; j < fftSize; ++j)
                    {
                        const int t = q0 + j - pad;
                        if (t >= t0 && t < t1)
                            a[(size_t) (t - t0)] += buf[(size_t) j] * W[(size_t) j] * plan.norm[(size_t) (q0 + j)];
                    }
                }
            }

        const std::lock_guard<std::mutex> l (addLock);
        for (int p = 0; p < StemData::numStored; ++p)
            for (int ch = 0; ch < 2; ++ch)
                juce::FloatVectorOperations::add (out.write[(size_t) p][(size_t) ch] + t0, acc[(size_t) (p * 2 + ch)].data(), t1 - t0);
    }

    void markReady (const Plan& plan, const std::vector<uint8_t>& done, int unit, StemData& out)
    {
        const auto [s, e] = plan.range (unit);
        for (int b = s / StemData::blockLen; b * StemData::blockLen < e && b < out.numBlocks; ++b)
        {
            if (out.ready[(size_t) b].load()) continue;
            const int bs = b * StemData::blockLen, be = juce::jmin (plan.n, bs + StemData::blockLen);
            bool all = true;
            for (int k = juce::jmax (0, unit - 2); k <= juce::jmin (plan.numUnits - 1, unit + 2) && all; ++k)
            {
                const auto [ks, ke] = plan.range (k);
                if (ks < be && ke > bs && ! done[(size_t) k]) all = false;
            }
            if (all)
            {
                out.ready[(size_t) b].store (true, std::memory_order_release);
                ++out.readyCount;
            }
        }
    }

    void separateAll (const float* const* mix, int numSamples, StemData& out, const Nets& nets)
    {
        const Plan plan (out.mode, numSamples);
        std::mutex m;
        std::vector<uint8_t> done ((size_t) plan.numUnits, 0);
        for (int u = 0; u < plan.numUnits; ++u)
        {
            processUnit (plan, u, mix, out, nets, m);
            done[(size_t) u] = 1;
            markReady (plan, done, u, out);
        }
    }
}

//==============================================================================
namespace
{
    void lowerThisThread()
    {
       #if JUCE_MAC
        pthread_set_qos_class_self_np (QOS_CLASS_UTILITY, 0);
       #elif JUCE_WINDOWS
        SetThreadPriority (GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
       #else
        setpriority (PRIO_PROCESS, (id_t) syscall (SYS_gettid), 10);
       #endif
    }

    // ONNX Runtime's own worker threads, created below normal priority too.
    OrtCustomThreadHandle createOrtThread (void*, OrtThreadWorkerFn fn, void* param)
    {
        auto* t = new std::thread ([fn, param] { lowerThisThread(); fn (param); });
        return reinterpret_cast<OrtCustomThreadHandle> (t);
    }

    void joinOrtThread (OrtCustomThreadHandle h)
    {
        auto* t = reinterpret_cast<std::thread*> (const_cast<OrtCustomHandleType*> (h));
        if (t->joinable()) t->join();
        delete t;
    }

    struct Runtime
    {
        static Runtime& get()
        {
            static auto* r = new Runtime();   // never destroyed: the library stays loaded until the process ends
            return *r;
        }

        bool ok = false;
        juce::String error;
        std::unique_ptr<Ort::Env> env;

    private:
        juce::DynamicLibrary lib;

        static juce::Array<juce::File> candidates()
        {
            juce::Array<juce::File> c;
            const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
           #if JUCE_MAC
            const char* name = "libonnxruntime.dylib";
            c.add (juce::File::getSpecialLocation (juce::File::currentApplicationFile).getChildFile ("Contents/Frameworks").getChildFile (name));
           #elif JUCE_WINDOWS
            const char* name = "onnxruntime.dll";
           #else
            const char* name = "libonnxruntime.so";
            c.add (exe.getParentDirectory().getSiblingFile ("share").getChildFile ("openmix").getChildFile (name));   // instalar.sh
           #endif
            c.add (exe.getParentDirectory().getChildFile (name));
            c.add (StemSeparator::folder().getChildFile (name));
            return c;
        }

        Runtime()
        {
           #if JUCE_MAC && ! JUCE_ARM
            error = "Los stems necesitan una Mac con Apple Silicon."_u;
           #else
            load();
           #endif
        }

        void load()
        {
            for (const auto& f : candidates())
            {
                if (! f.existsAsFile() || ! lib.open (f.getFullPathName()))
                    continue;
                using GetBase = const OrtApiBase* (*)();
                if (auto fn = (GetBase) lib.getFunction ("OrtGetApiBase"))
                    if (const auto* base = fn())
                        if (const auto* api = base->GetApi (ORT_API_VERSION))
                        {
                            Ort::InitApi (api);
                            try
                            {
                                env = std::make_unique<Ort::Env> (ORT_LOGGING_LEVEL_ERROR, "OpenMix");
                                ok = true;
                                return;
                            }
                            catch (const std::exception& e) { error = e.what(); }
                        }
                lib.close();
            }
            if (error.isEmpty())
                error = "No se encontró el motor de separación (ONNX Runtime) en esta instalación."_u;
        }
    };

    std::unique_ptr<Ort::Session> makeSession (const juce::File& model, int threads)
    {
        auto& rt = Runtime::get();
        Ort::SessionOptions so;
        so.SetGraphOptimizationLevel (GraphOptimizationLevel::ORT_ENABLE_ALL);
        so.SetIntraOpNumThreads (threads);
        so.SetInterOpNumThreads (1);
        so.DisableCpuMemArena();      // HT-Demucs peaks at about 5 GB per run: give it back between runs
        so.DisableMemPattern();
        so.AddConfigEntry ("session.intra_op.allow_spinning", "0");   // idle threads sleep instead of burning CPU
        so.AddConfigEntry ("session.inter_op.allow_spinning", "0");
        Ort::ThrowOnError (Ort::GetApi().SessionOptionsSetCustomCreateThreadFn (so, createOrtThread));
        Ort::ThrowOnError (Ort::GetApi().SessionOptionsSetCustomJoinThreadFn (so, joinOrtThread));
       #if JUCE_WINDOWS
        return std::make_unique<Ort::Session> (*rt.env, model.getFullPathName().toWideCharPointer(), so);
       #else
        return std::make_unique<Ort::Session> (*rt.env, model.getFullPathName().toRawUTF8(), so);
       #endif
    }

    struct ModelFile { const char* name; const char* url; const char* sha256; juce::int64 size; };

    const std::vector<ModelFile>& modelFiles (StemMode m)
    {
        // Pinned revisions, verified after download.
        static const std::vector<ModelFile> quality {
            { "htdemucs_fp16weights.onnx",
              "https://huggingface.co/StemSplitio/htdemucs-onnx/resolve/d54ed9eb60e258ea82131c6ee14578628816456a/htdemucs_fp16weights.onnx",
              "d05c269d0178d2a72ad484b10b11dd370193fc923201c3b27a99f848745db70a", 165612636 } };
        static const std::vector<ModelFile> fast {
            { "spleeter-vocals.onnx", "https://huggingface.co/Best-Practice/spleeter-4stems-onnx/resolve/486643021f18a4d057d282fff1d11b89a30e9887/vocals.wide.fp16.onnx",
              "13c7cd7a17d63ee8121dcb0f890ae7ff883b4e6235723a1e993fa5775924723e", 19681064 },
            { "spleeter-drums.onnx", "https://huggingface.co/Best-Practice/spleeter-4stems-onnx/resolve/486643021f18a4d057d282fff1d11b89a30e9887/drums.wide.fp16.onnx",
              "6249b765acde30520e9a5c0124aa0ffdae5c8615fa91da59b32653a3a5674764", 19681063 },
            { "spleeter-bass.onnx", "https://huggingface.co/Best-Practice/spleeter-4stems-onnx/resolve/486643021f18a4d057d282fff1d11b89a30e9887/bass.wide.fp16.onnx",
              "fa1c4ed68a97167c79908ee574ecf0bf97d667a9904a629a6103e5a63d8d4740", 19681062 },
            { "spleeter-other.onnx", "https://huggingface.co/Best-Practice/spleeter-4stems-onnx/resolve/486643021f18a4d057d282fff1d11b89a30e9887/other.wide.fp16.onnx",
              "3dcf5740d23f1c3c7ea925865dc5f08bc823a107b7b6257d39bbb00b4997f6c4", 19681063 } };
        return m == StemMode::quality ? quality : fast;
    }

    juce::File modelPath (const ModelFile& m) { return StemSeparator::folder().getChildFile (m.name); }
}

//==============================================================================
struct StemSeparator::Job
{
    LoadedTrackPtr track;
    std::weak_ptr<StemData> out;
    StemMode mode;
    std::function<double()> playhead;
    int n = 0;
    int phase = 0;                         // 0 not prepared · 1 preparing · 2 ready to run units
    std::unique_ptr<StemDsp::Plan> plan;
    std::vector<float> mix[2];             // the track at 44.1 kHz
    std::vector<uint8_t> state;            // per unit: 0 free · 1 running · 2 done
    std::vector<uint8_t> done;
    int remaining = 0;
    std::mutex addLock;

    void prepare()
    {
        const auto& a = track->audio;
        const int srcN = a.getNumSamples();
        const double k = track->sampleRate / StemData::rate;
        for (int ch = 0; ch < 2; ++ch)
        {
            mix[ch].resize ((size_t) n);
            const float* src = a.getReadPointer (ch);
            if (std::abs (k - 1.0) < 1.0e-9)
                std::copy_n (src, juce::jmin (n, srcN), mix[ch].data());
            else
            {
                const auto& sinc = SincResampler::get();
                for (int i = 0; i < n; ++i)
                    mix[ch][(size_t) i] = sinc.interpolate (src, srcN, i * k, k);
            }
        }
        plan = std::make_unique<StemDsp::Plan> (mode, n);
        state.assign ((size_t) plan->numUnits, 0);
        done.assign ((size_t) plan->numUnits, 0);
        remaining = plan->numUnits;
    }
};

struct StemSeparator::Worker : juce::Thread
{
    Worker (StemSeparator& o, int i) : juce::Thread ("OpenMix stems " + juce::String (i)), owner (o) {}
    ~Worker() override { stopThread (60000); }

    StemSeparator& owner;
    int sessionsMode = -1;
    std::vector<std::unique_ptr<Ort::Session>> sessions;
    double idleSince = 0.0;

    bool ensureSessions (StemMode m, juce::String& error)
    {
        if (sessionsMode == (int) m && ! sessions.empty())
            return true;
        sessions.clear();
        sessionsMode = -1;
        try
        {
            for (const auto& f : modelFiles (m))
                sessions.push_back (makeSession (modelPath (f), m == StemMode::quality ? owner.threadBudget : juce::jmax (1, owner.threadBudget / 2)));
            sessionsMode = (int) m;
            return true;
        }
        catch (const std::exception& e)
        {
            sessions.clear();
            error = "No se pudo abrir el modelo de stems: "_u + juce::String (e.what());
            return false;
        }
    }

    StemDsp::Nets nets()
    {
        StemDsp::Nets nt;
        auto mem = std::make_shared<Ort::MemoryInfo> (Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault));
        nt.demucs = [this, mem] (const float* in, float* out)
        {
            const int64_t shape[3] { 1, 2, StemDsp::demucsLen };
            auto x = Ort::Value::CreateTensor<float> (*mem, const_cast<float*> (in), (size_t) 2 * StemDsp::demucsLen, shape, 3);
            const char* inN[] { "mix" };
            const char* outN[] { "stems" };
            auto y = sessions[0]->Run (Ort::RunOptions { nullptr }, inN, &x, 1, outN, 1);
            std::copy_n (y[0].GetTensorData<float>(), (size_t) 8 * StemDsp::demucsLen, out);
        };
        nt.spleeter = [this, mem] (int stem, const float* in, float* out)
        {
            const int64_t shape[4] { 2, 1, StemDsp::splitFrames, StemDsp::netBins };
            const size_t count = (size_t) 2 * StemDsp::splitFrames * StemDsp::netBins;
            auto x = Ort::Value::CreateTensor<float> (*mem, const_cast<float*> (in), count, shape, 4);
            const char* inN[] { "x" };
            const char* outN[] { "y" };
            auto y = sessions[(size_t) stem]->Run (Ort::RunOptions { nullptr }, inN, &x, 1, outN, 1);
            std::copy_n (y[0].GetTensorData<float>(), count, out);
        };
        return nt;
    }

    void run() override
    {
        lowerThisThread();
        while (! threadShouldExit())
        {
            auto w = owner.pick();
            if (w.job == nullptr)
            {
                const double now = juce::Time::getMillisecondCounterHiRes();
                if (idleSince <= 0.0) idleSince = now;
                if (! sessions.empty() && now - idleSince > 30000.0)
                {
                    sessions.clear();          // frees the model's memory while nothing is being separated
                    sessionsMode = -1;
                }
                std::unique_lock<std::mutex> l (owner.lock);
                owner.wake.wait_for (l, std::chrono::milliseconds (500));
                continue;
            }
            idleSince = 0.0;

            if (w.unit < 0)                     // this worker prepares the job
            {
                w.job->prepare();
                const std::lock_guard<std::mutex> l (owner.lock);
                w.job->phase = 2;
                owner.wake.notify_all();
                continue;
            }

            juce::String error;
            bool ok = ensureSessions (w.job->mode, error);
            if (ok)
            {
                try
                {
                    const float* mix[2] { w.job->mix[0].data(), w.job->mix[1].data() };
                    StemDsp::processUnit (*w.job->plan, w.unit, mix, *w.out, nets(), w.job->addLock);
                }
                catch (const std::exception& e)
                {
                    ok = false;
                    error = "Falló la separación: "_u + juce::String (e.what());
                }
            }
            owner.finish (w, ok, error);
        }
    }
};

//==============================================================================
StemSeparator::StemSeparator()
{
    const int cores = juce::jmax (1, juce::SystemStats::getNumPhysicalCpus());
    const int budget = juce::jlimit (2, 8, cores - 2);   // leave room for audio, drawing and the other apps
    // Rápido: two splits at a time. Calidad: one segment at a time with every thread (its memory is the limit).
    const int numWorkers = 2;
    threadBudget = budget;
    for (int i = 0; i < numWorkers; ++i)
        workers.push_back (std::make_unique<Worker> (*this, i));
}

StemSeparator::~StemSeparator()
{
    quitting = true;
    for (auto& w : workers) w->signalThreadShouldExit();
    wake.notify_all();
    workers.clear();
}

juce::File StemSeparator::folder()
{
    return Library::dataFolder().getChildFile ("Stems");
}

bool StemSeparator::runtimeAvailable()       { return Runtime::get().ok; }
juce::String StemSeparator::runtimeError()   { return Runtime::get().error; }

bool StemSeparator::modelInstalled (StemMode m) const
{
    for (const auto& f : modelFiles (m))
        if (modelPath (f).getSize() != f.size)
            return false;
    return true;
}

juce::int64 StemSeparator::downloadSize (StemMode m) const
{
    juce::int64 total = 0;
    for (const auto& f : modelFiles (m))
        if (modelPath (f).getSize() != f.size)
            total += f.size;
    return total;
}

void StemSeparator::install (StemMode m, std::function<void (float)> progress, std::function<void (juce::String)> done)
{
    if (installing.exchange (true))
        return;
    std::thread ([this, m, progress, done]
    {
        lowerThisThread();
        juce::String error;
        folder().createDirectory();
        const auto& files = modelFiles (m);
        juce::int64 total = 0, got = 0;
        for (const auto& f : files) total += f.size;
        double lastReport = 0.0;
        for (const auto& f : files)
        {
            const auto dest = modelPath (f);
            if (dest.getSize() == f.size) { got += f.size; continue; }
            const auto part = dest.withFileExtension ("part");
            part.deleteFile();
            {
                auto in = juce::URL (f.url).createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                                                   .withConnectionTimeoutMs (20000)
                                                                   .withNumRedirectsToFollow (8));
                juce::FileOutputStream os (part);
                if (in == nullptr || ! os.openedOk())
                {
                    error = in == nullptr ? "No se pudo conectar para bajar el modelo. Revisá la conexión a internet."_u
                                          : "No se pudo guardar el modelo en el disco."_u;
                    break;
                }
                juce::HeapBlock<char> chunk (1 << 16);
                for (;;)
                {
                    if (quitting.load()) { error = "Cancelado"; break; }
                    const int r = in->read (chunk, 1 << 16);
                    if (r <= 0) break;
                    if (! os.write (chunk, (size_t) r)) { error = "No hay espacio en el disco para el modelo."_u; break; }
                    got += r;
                    const double now = juce::Time::getMillisecondCounterHiRes();
                    if (progress && now - lastReport > 150.0)
                    {
                        lastReport = now;
                        const float p = (float) ((double) got / (double) juce::jmax ((juce::int64) 1, total));
                        juce::MessageManager::callAsync ([progress, p] { progress (p); });
                    }
                }
                os.flush();
            }
            if (error.isEmpty() && part.getSize() != f.size)
                error = "La descarga del modelo quedó incompleta. Probá de nuevo."_u;
            if (error.isEmpty() && juce::SHA256 (part).toHexString() != juce::String (f.sha256))
                error = "El modelo bajado no coincide con el original (verificación SHA-256)."_u;
            if (error.isNotEmpty() || ! part.moveFileTo (dest))
            {
                part.deleteFile();
                if (error.isEmpty()) error = "No se pudo guardar el modelo en el disco."_u;
                break;
            }
        }
        installing = false;
        juce::MessageManager::callAsync ([done, error] { if (done) done (error); });
    }).detach();
}

StemDataPtr StemSeparator::separate (LoadedTrackPtr track, StemMode mode, std::function<double()> playhead)
{
    if (track == nullptr || track->numSamples() <= 0)
        return nullptr;
    const std::lock_guard<std::mutex> l (lock);
    for (auto& j : jobs)
        if (j->track == track && j->mode == mode)
            if (auto o = j->out.lock())
            {
                j->playhead = playhead;        // the newest deck playing it leads
                return o;
            }

    const int n = (int) std::ceil (track->numSamples() * StemData::rate / track->sampleRate);
    auto out = std::make_shared<StemData> (n, mode);
    auto job = std::make_shared<Job>();
    job->track = track;
    job->out = out;
    job->mode = mode;
    job->playhead = std::move (playhead);
    job->n = n;
    jobs.insert (jobs.begin(), job);          // the newest load goes first
    for (auto& w : workers)
        if (! w->isThreadRunning()) w->startThread();
    wake.notify_all();
    return out;
}

StemSeparator::Pick StemSeparator::pick()
{
    const std::lock_guard<std::mutex> l (lock);
    // Stems nobody holds any more (track unloaded): stop.
    jobs.erase (std::remove_if (jobs.begin(), jobs.end(), [] (const auto& j) { return j->out.expired() && j->phase != 1; }), jobs.end());
    bool qualityRunning = false;
    for (auto& j : jobs)
        if (j->mode == StemMode::quality && j->phase == 2)
            qualityRunning = qualityRunning || std::find (j->state.begin(), j->state.end(), (uint8_t) 1) != j->state.end();
    for (auto& j : jobs)
    {
        auto out = j->out.lock();
        if (out == nullptr) continue;
        if (j->phase == 0) { j->phase = 1; return { j, out, -1 }; }
        if (j->phase != 2) continue;
        if (j->mode == StemMode::quality && qualityRunning) continue;   // one HT-Demucs run at a time (memory)
        const double at = j->playhead ? j->playhead() * StemData::rate : 0.0;
        int from = j->plan->unitAt (at);
        while (from > 0 && j->plan->range (from - 1).second > (int) at)   // the unit before overlaps the playhead
            --from;
        for (int pass = 0; pass < 2; ++pass)
            for (int u = pass == 0 ? from : 0; u < (pass == 0 ? j->plan->numUnits : from); ++u)
                if (j->state[(size_t) u] == 0)
                {
                    j->state[(size_t) u] = 1;
                    return { j, out, u };
                }
    }
    return {};
}

void StemSeparator::finish (const Pick& w, bool ok, const juce::String& error)
{
    const std::lock_guard<std::mutex> l (lock);
    auto& j = *w.job;
    if (! ok)
    {
        w.out->fail (error);
        jobs.erase (std::remove (jobs.begin(), jobs.end(), w.job), jobs.end());
        return;
    }
    j.state[(size_t) w.unit] = 2;
    j.done[(size_t) w.unit] = 1;
    StemDsp::markReady (*j.plan, j.done, w.unit, *w.out);
    if (--j.remaining <= 0)
        jobs.erase (std::remove (jobs.begin(), jobs.end(), w.job), jobs.end());
}

} // namespace om
