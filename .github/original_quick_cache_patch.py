from pathlib import Path

p = Path('stem_dsp.cpp')
s = p.read_text(encoding='utf-8')

old = '''constexpr double kOriginalPrefetchSeconds = 5.0;\nconstexpr double kSwitchFadeSeconds = 0.050;\n'''
new = '''// Original transport does not need a 20-second analysis block. Publish a\n// compact decoder-only window quickly, then keep extending it in the background.\nconstexpr double kOriginalQuickCacheSeconds = 4.0;\nconstexpr double kOriginalQuickOverlapSeconds = 0.75;\nconstexpr double kOriginalPrefetchSeconds = 1.5;\nconstexpr double kSwitchFadeSeconds = 0.050;\n'''
if old not in s:
    raise SystemExit('Original prefetch constants anchor not found')
s = s.replace(old, new, 1)

old = '''                // Center the cheap 20-second Original window on the grab point.\n                // A forward-biased window left only 0.5 s available for an\n                // immediate backward scratch. Centering gives the platter useful\n                // cached travel in both directions before any follow-up prefetch.\n                const double preview_start = (std::max)(\n                    0.0, seconds - kCacheSeconds * 0.5);\n'''
new = '''                // Center a compact Original window on the grab point. It is\n                // decoder-only and is intentionally much smaller than a stem\n                // analysis block so first-motion PCM becomes available quickly.\n                const double preview_start = (std::max)(\n                    0.0, seconds - kOriginalQuickCacheSeconds * 0.5);\n'''
if old not in s:
    raise SystemExit('seek Original preview anchor not found')
s = s.replace(old, new, 1)

old = '''                // A seek/jump landed outside cached Original PCM. Center a fresh\n                // 20-second window so both scratch directions become available.\n                next = (std::max)(\n                    0.0, playback_seconds - kCacheSeconds * 0.5);\n'''
new = '''                // A seek/jump landed outside cached Original PCM. Center a fresh\n                // quick window so both scratch directions become available fast.\n                next = (std::max)(\n                    0.0, playback_seconds - kOriginalQuickCacheSeconds * 0.5);\n'''
if old not in s:
    raise SystemExit('ensure_ahead center anchor not found')
s = s.replace(old, new, 1)

old = '''                next = (std::max)(\n                    0.0, covering_end - kCacheOverlapSeconds);\n'''
new = '''                next = (std::max)(\n                    0.0, covering_end - kOriginalQuickOverlapSeconds);\n'''
if old not in s:
    raise SystemExit('Original overlap anchor not found')
s = s.replace(old, new, 1)

old = '''        double start = reverse\n            ? (std::max)(0.0, position_seconds - (kCacheSeconds - 0.5))\n            : (std::max)(0.0, position_seconds - 0.5);\n\n        m_jobs.emplace_front(cache_job{\n'''
new = '''        const bool original_preview = mode == stemmode::mode::original;\n        const double transport_window = original_preview\n            ? kOriginalQuickCacheSeconds\n            : kCacheSeconds;\n        const double edge_preroll = original_preview ? 0.25 : 0.5;\n\n        double start = reverse\n            ? (std::max)(0.0, position_seconds -\n                (transport_window - edge_preroll))\n            : (std::max)(0.0, position_seconds - edge_preroll);\n\n        m_jobs.emplace_front(cache_job{\n'''
if old not in s:
    raise SystemExit('request_transport start anchor not found')
s = s.replace(old, new, 1)

old = '''    static bool decode_exact_block(\n        sequential_decoder_state& state,\n        double requested_start_seconds,\n        bool force_reanchor,\n        std::vector<float>& audio) {\n'''
new = '''    static bool decode_exact_block(\n        sequential_decoder_state& state,\n        double requested_start_seconds,\n        bool force_reanchor,\n        double window_seconds,\n        double overlap_seconds,\n        std::vector<float>& audio) {\n'''
if old not in s:
    raise SystemExit('decode_exact_block signature anchor not found')
s = s.replace(old, new, 1)

old = '''        const uint64_t window_frames =\n            static_cast<uint64_t>(\n                kCacheSeconds *\n                static_cast<double>(\n                    kCacheRate) +\n                0.5);\n'''
new = '''        const uint64_t window_frames =\n            static_cast<uint64_t>(\n                window_seconds *\n                static_cast<double>(\n                    kCacheRate) +\n                0.5);\n'''
if old not in s:
    raise SystemExit('window_frames anchor not found')
s = s.replace(old, new, 1)

old = '''        const uint64_t hop_frames =\n            static_cast<uint64_t>(\n                (kCacheSeconds -\n                 kCacheOverlapSeconds) *\n                static_cast<double>(\n                    kCacheRate) +\n                0.5);\n'''
new = '''        const uint64_t hop_frames =\n            static_cast<uint64_t>(\n                ((std::max)(0.001, window_seconds - overlap_seconds)) *\n                static_cast<double>(\n                    kCacheRate) +\n                0.5);\n'''
if old not in s:
    raise SystemExit('hop_frames anchor not found')
s = s.replace(old, new, 1)

old = '''                    const bool decoded =\n                        decode_exact_block(\n                            active_decoder,\n                            job.start_seconds,\n                            job.force_reanchor,\n                            input);\n'''
new = '''                    const double decode_window = job.need_stems\n                        ? kCacheSeconds\n                        : kOriginalQuickCacheSeconds;\n                    const double decode_overlap = job.need_stems\n                        ? kCacheOverlapSeconds\n                        : kOriginalQuickOverlapSeconds;\n\n                    const bool decoded =\n                        decode_exact_block(\n                            active_decoder,\n                            job.start_seconds,\n                            job.force_reanchor,\n                            decode_window,\n                            decode_overlap,\n                            input);\n'''
if old not in s:
    raise SystemExit('decode_exact_block call anchor not found')
s = s.replace(old, new, 1)

p.write_text(s, encoding='utf-8')
print('patched quick Original PCM cache windows')
