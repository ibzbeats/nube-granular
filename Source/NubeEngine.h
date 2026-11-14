#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

/*
    NubeEngine - motor del efecto "Nube Granular".

    Es C++ puro (no depende de JUCE), asi se puede probar por separado.
    Hace lo mismo que el script JSFX "nube_granular.jsfx":
      1. Guarda el audio de entrada en una memoria circular.
      2. Lanza "granos" (trocitos con ventana suave) que leen esa memoria
         con distinta altura (pitch), posicion y panorama.
      3. Filtra el resultado (pasa-bajos / pasa-altos), lo realimenta
         (feedback) y le anade reverb.
*/
class NubeEngine
{
public:
    struct Params
    {
        float mixPct         = 50.0f;     // 0..100
        float grainMs        = 150.0f;    // 10..500
        float pitchSemi      = 0.0f;      // -24..24
        float spreadPct      = 50.0f;     // 0..100
        float densityPct     = 50.0f;     // 0..100
        float scatterPct     = 40.0f;     // 0..100
        float feedbackPct    = 35.0f;     // 0..92
        float widthPct       = 70.0f;     // 0..100
        float lowpassHz      = 12000.0f;  // 200..20000
        float highpassHz     = 60.0f;     // 20..2000
        float reverbPct      = 30.0f;     // 0..100
        float reverbDecayPct = 70.0f;     // 0..100
        int   mode           = 1;         // 0 continuo, 1 octavas y quintas, 2 solo octavas
        float outGainDb      = 0.0f;      // -18..12
    };

    void prepare (double sampleRate, uint32_t seed = 0x9E3779B9u)
    {
        sr = (sampleRate >= 8000.0 && sampleRate <= 768000.0) ? sampleRate : 44100.0;
        rngState = seed != 0 ? seed : 1u;

        bufL.assign ((size_t) kBufSize, 0.0f);
        bufR.assign ((size_t) kBufSize, 0.0f);

        static const int baseLen[kRevLines] = { 1116, 1188, 1277, 1356, 556, 441,
                                                1139, 1211, 1300, 1379, 579, 464 };
        const double sc = sr / 44100.0;
        for (int k = 0; k < kRevLines; ++k)
        {
            revLen[k] = std::max (2, (int) std::floor (baseLen[k] * sc));
            rev[k].assign ((size_t) revLen[k], 0.0f);
        }

        clearState();
        setParams (params);
        mixS = mixT;
        revS = revT;
        goutS = goutT;
    }

    // Borra memoria, colas de reverb y granos (sin cambiar los parametros).
    void clearState()
    {
        std::fill (bufL.begin(), bufL.end(), 0.0f);
        std::fill (bufR.begin(), bufR.end(), 0.0f);
        for (int k = 0; k < kRevLines; ++k)
        {
            std::fill (rev[k].begin(), rev[k].end(), 0.0f);
            revIdx[k] = 0;
            revFlt[k] = 0.0;
        }
        for (auto& g : grains)
            g = Grain();
        wp = 0;
        spawnCnt = 0.0;
        lpl = lpr = hpl = hpr = 0.0;
        fbl = fbr = 0.0;
    }

    void setParams (const Params& in)
    {
        params = in;
        const double mixPct   = clampd (in.mixPct, 0.0, 100.0);
        const double grainMs  = clampd (in.grainMs, 10.0, 500.0);
        const double densPct  = clampd (in.densityPct, 0.0, 100.0);
        const double lpHz     = clampd (in.lowpassHz, 200.0, 20000.0);
        const double hpHz     = clampd (in.highpassHz, 20.0, 2000.0);
        const double decayPct = clampd (in.reverbDecayPct, 0.0, 100.0);

        mixT    = mixPct / 100.0;
        gsize   = grainMs * 0.001 * sr;
        pitch   = clampd (in.pitchSemi, -24.0, 24.0);
        spread  = clampd (in.spreadPct, 0.0, 100.0) / 100.0;
        overlap = 1.0 + densPct / 100.0 * 7.0;
        scat    = clampd (in.scatterPct, 0.0, 100.0) / 100.0;
        fb      = clampd (in.feedbackPct, 0.0, 92.0) / 100.0;
        width   = clampd (in.widthPct, 0.0, 100.0) / 100.0;
        alp     = 1.0 - std::exp (-2.0 * kPi * lpHz / sr);
        ahp     = 1.0 - std::exp (-2.0 * kPi * hpHz / sr);
        revT    = clampd (in.reverbPct, 0.0, 100.0) / 100.0;
        rfb     = 0.70 + 0.28 * decayPct / 100.0;
        rnorm   = 0.237 * std::pow (1.0 - rfb, 0.25);
        mode    = std::min (2, std::max (0, in.mode));
        goutT   = std::exp (clampd (in.outGainDb, -18.0, 12.0) * 0.1151292546);
        gnorm   = 2.0 / overlap;
        kout    = 0.8165 * std::sqrt (overlap);
    }

    // Procesa un bloque estereo "in place" (la entrada se sustituye por la salida).
    void process (float* left, float* right, int numSamples)
    {
        for (int s = 0; s < numSamples; ++s)
        {
            double inl = left[s];
            double inr = right[s];
            if (! std::isfinite (inl)) inl = 0.0;
            if (! std::isfinite (inr)) inr = 0.0;

            // suavizado de parametros (evita clicks)
            mixS  += 0.002 * (mixT - mixS);
            revS  += 0.002 * (revT - revS);
            goutS += 0.002 * (goutT - goutS);

            // ---------- nacimiento de granos ----------
            spawnCnt -= 1.0;
            if (spawnCnt <= 0.0)
            {
                spawnGrain();
                spawnCnt += gsize / overlap * (0.6 + rnd (0.8));
            }

            // ---------- sintesis granular ----------
            double wl = 0.0, wr = 0.0;
            for (int gi = 0; gi < kMaxGrains; ++gi)
            {
                Grain& g = grains[(size_t) gi];
                if (! g.active)
                    continue;

                const int i0 = ((int) g.pos) & kMask;
                const int i1 = (i0 + 1) & kMask;
                const double fr = g.pos - std::floor (g.pos);
                const double xl = bufL[(size_t) i0] + fr * (bufL[(size_t) i1] - bufL[(size_t) i0]);
                const double xr = bufR[(size_t) i0] + fr * (bufR[(size_t) i1] - bufR[(size_t) i0]);

                double gw = std::sin (g.age / g.len * kPi);
                gw *= gw;
                const double gm = (xl + xr) * 0.5;
                wl += gw * ((1.0 - width) * xl + width * kSqrt2 * gm * g.pl);
                wr += gw * ((1.0 - width) * xr + width * kSqrt2 * gm * g.pr);

                g.pos += g.inc;
                if (g.pos >= (double) kBufSize)
                    g.pos -= (double) kBufSize;
                g.age += 1.0;
                if (g.age >= g.len)
                    g.active = false;
            }

            // ---------- filtros ----------
            wl *= gnorm;
            wr *= gnorm;
            lpl += alp * (wl - lpl);
            lpr += alp * (wr - lpr);
            hpl += ahp * (lpl - hpl);
            hpr += ahp * (lpr - hpr);
            const double fl = lpl - hpl;   // senal filtrada (camino del feedback)
            const double fr2 = lpr - hpr;

            // ---------- escritura en memoria: entrada + feedback anterior ----------
            bufL[(size_t) wp] = (float) (inl + fbl);
            bufR[(size_t) wp] = (float) (inr + fbr);
            wp = (wp + 1) & kMask;

            // feedback con saturacion suave (evita que se dispare)
            fbl = fb * softClip (fl);
            fbr = fb * softClip (fr2);

            const double yl = fl * kout;
            const double yr = fr2 * kout;

            // ---------- reverb ----------
            double al = 0.0, ar = 0.0;
            for (int k = 0; k < 4; ++k)
                al += comb (k, yl);
            for (int k = 4; k < 6; ++k)
                al = allpass (k, al);
            for (int k = 6; k < 10; ++k)
                ar += comb (k, yr);
            for (int k = 10; k < 12; ++k)
                ar = allpass (k, ar);
            const double rvl = al * rnorm;
            const double rvr = ar * rnorm;

            // ---------- mezcla final ----------
            const double rcomp = 1.0 / std::sqrt (1.0 + revS * revS);
            const double ml = (yl + rvl * revS) * rcomp;
            const double mr = (yr + rvr * revS) * rcomp;
            double outL = (inl * (1.0 - mixS) + ml * mixS) * goutS;
            double outR = (inr * (1.0 - mixS) + mr * mixS) * goutS;

            if (! std::isfinite (outL) || ! std::isfinite (outR))
            {
                // Seguridad: si algo se descontrola, se reinicia el motor y se deja pasar el original.
                clearState();
                outL = inl;
                outR = inr;
            }

            left[s]  = (float) outL;
            right[s] = (float) outR;
        }
    }

private:
    static constexpr int kBufSize = 1 << 19;       // memoria de audio por canal (524288 muestras)
    static constexpr int kMask = kBufSize - 1;
    static constexpr int kMaxGrains = 32;
    static constexpr int kRevLines = 12;
    static constexpr double kPi = 3.14159265358979323846;
    static constexpr double kSqrt2 = 1.41421356237309504880;
    static constexpr double kRevDamp = 0.35;

    struct Grain
    {
        bool active = false;
        double pos = 0.0;   // posicion de lectura en la memoria circular
        double inc = 1.0;   // velocidad de lectura (= razon de pitch)
        double age = 0.0;
        double len = 1.0;
        double pl = 0.7071;
        double pr = 0.7071;
    };

    static double clampd (double v, double lo, double hi)
    {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    static double softClip (double x)
    {
        const double t = clampd (x, -3.0, 3.0);
        return t * (27.0 + t * t) / (27.0 + 9.0 * t * t);
    }

    // numero aleatorio en [0, range)
    double rnd (double range)
    {
        rngState ^= rngState << 13;
        rngState ^= rngState >> 17;
        rngState ^= rngState << 5;
        return (double) (rngState >> 8) * (1.0 / 16777216.0) * range;
    }

    void spawnGrain()
    {
        int gk = -1;
        for (int i = 0; i < kMaxGrains; ++i)
        {
            if (! grains[(size_t) i].active)
            {
                gk = i;
                break;
            }
        }
        if (gk < 0)
            return;

        const double maxD = (double) kBufSize - 64.0;
        const double maxEx = maxD * 0.4;

        double glen = gsize * (0.75 + rnd (0.5));
        double semi = pitch;
        if (mode == 0)
        {
            semi += (rnd (2.0) - 1.0) * 12.0 * spread;
        }
        else if (rnd (1.0) < spread)
        {
            if (mode == 1)
            {
                const int q = (int) std::floor (rnd (4.0));
                semi += (q == 0 ? -12.0 : (q == 1 ? -7.0 : (q == 2 ? 7.0 : 12.0)));
            }
            else
            {
                semi += (rnd (1.0) < 0.5 ? -12.0 : 12.0);
            }
        }
        semi = clampd (semi, -36.0, 36.0);

        const double rr = std::pow (2.0, semi / 12.0);
        const double dv = std::abs (rr - 1.0);
        double ex = dv * glen;
        if (ex > maxEx)
        {
            glen = maxEx / dv;
            ex = maxEx;
        }

        // retardo inicial de lectura: nunca cruza la cabeza de escritura ni se sale de la memoria
        const double lo = (rr > 1.0) ? ex + 32.0 : 32.0;
        const double hi = (rr < 1.0) ? maxD - ex : maxD;
        const double rg = std::min (hi - lo, scat * sr);
        const double d0 = lo + rnd (1.0) * rg;

        double p0 = (double) wp - d0;
        if (p0 < 0.0)
            p0 += (double) kBufSize;
        if (p0 >= (double) kBufSize)
            p0 -= (double) kBufSize;

        const double ang = ((rnd (2.0) - 1.0) * width * 0.5 + 0.5) * kPi * 0.5;

        Grain& g = grains[(size_t) gk];
        g.active = true;
        g.pos = p0;
        g.inc = rr;
        g.age = 0.0;
        g.len = glen;
        g.pl = std::cos (ang);
        g.pr = std::sin (ang);
    }

    // linea de reverb tipo "comb" con amortiguacion
    double comb (int k, double in)
    {
        std::vector<float>& line = rev[k];
        int& ix = revIdx[k];
        const double co = line[(size_t) ix];
        revFlt[k] = co * (1.0 - kRevDamp) + revFlt[k] * kRevDamp;
        line[(size_t) ix] = (float) (in + revFlt[k] * rfb);
        if (++ix >= revLen[k])
            ix = 0;
        return co;
    }

    // filtro "allpass" para difuminar la cola del reverb
    double allpass (int k, double in)
    {
        std::vector<float>& line = rev[k];
        int& ix = revIdx[k];
        const double co = line[(size_t) ix];
        line[(size_t) ix] = (float) (in + co * 0.5);
        if (++ix >= revLen[k])
            ix = 0;
        return co - in;
    }

    // ---- estado ----
    double sr = 44100.0;
    uint32_t rngState = 1u;
    Params params;

    std::vector<float> bufL, bufR;
    int wp = 0;
    Grain grains[kMaxGrains];
    double spawnCnt = 0.0;

    std::vector<float> rev[kRevLines];
    int revLen[kRevLines] = {};
    int revIdx[kRevLines] = {};
    double revFlt[kRevLines] = {};

    double lpl = 0.0, lpr = 0.0, hpl = 0.0, hpr = 0.0;
    double fbl = 0.0, fbr = 0.0;
    double mixS = 0.5, revS = 0.3, goutS = 1.0;

    // ---- valores derivados de los parametros ----
    double mixT = 0.5, revT = 0.3, goutT = 1.0;
    double gsize = 6615.0, pitch = 0.0, spread = 0.5, overlap = 4.5, scat = 0.4, fb = 0.35, width = 0.7;
    double alp = 0.5, ahp = 0.01, rfb = 0.9, rnorm = 0.1, gnorm = 0.44, kout = 1.7;
    int mode = 1;
};
