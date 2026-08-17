from pathlib import Path

p = Path('stem_dsp.cpp')
s = p.read_text(encoding='utf-8')

old = '''constexpr double kOriginalQuickCacheSeconds = 4.0;\nconstexpr double kOriginalQuickOverlapSeconds = 0.75;\nconstexpr double kOriginalPrefetchSeconds = 1.5;\nconstexpr double kSwitchFadeSeconds = 0.050;\nconstexpr double kCacheHandoffFadeSeconds = 0.080;\nconstexpr double kDecodeSeekPrerollSeconds = 5.0;\n'''
new = '''// Keep random Original platter requests tiny and quick. A 1.5-second window\n// exactly covers the transport renderer's current 1.25-second directional\n// safety margin plus a small overlap, so the worker publishes usable PCM\n// without decoding a long block first.\nconstexpr double kOriginalQuickCacheSeconds = 1.5;\nconstexpr double kOriginalQuickOverlapSeconds = 0.25;\nconstexpr double kOriginalPrefetchSeconds = 0.50;\nconstexpr double kSwitchFadeSeconds = 0.050;\nconstexpr double kCacheHandoffFadeSeconds = 0.080;\nconstexpr double kDecodeSeekPrerollSeconds = 5.0;\n// Stem analysis keeps the conservative 5-second preroll used by the stable\n// VBR path. Original scratch PCM only needs timestamp-accurate decoder output,\n// so use a short preroll to avoid decoding/discarding five seconds on every\n// random hand movement.\nconstexpr double kOriginalDecodeSeekPrerollSeconds = 0.50;\n'''
if old not in s:
    raise SystemExit('quick cache constants anchor not found')
s = s.replace(old, new, 1)

old = '''    static bool reanchor_decoder(\n        sequential_decoder_state& state,\n        const std::wstring& path,\n        double target_seconds) {\n'''
new = '''    static bool reanchor_decoder(\n        sequential_decoder_state& state,\n        const std::wstring& path,\n        double target_seconds,\n        double seek_preroll_seconds) {\n'''
if old not in s:
    raise SystemExit('reanchor signature anchor not found')
s = s.replace(old, new, 1)

old = '''        double seek_seconds =\n            target_seconds -\n            kDecodeSeekPrerollSeconds;\n'''
new = '''        double seek_seconds =\n            target_seconds -\n            (std::max)(0.0, seek_preroll_seconds);\n'''
if old not in s:
    raise SystemExit('reanchor preroll anchor not found')
s = s.replace(old, new, 1)

old = '''    static bool decode_exact_block(\n        sequential_decoder_state& state,\n        double requested_start_seconds,\n        bool force_reanchor,\n        double window_seconds,\n        double overlap_seconds,\n        std::vector<float>& audio) {\n'''
new = '''    static bool decode_exact_block(\n        sequential_decoder_state& state,\n        double requested_start_seconds,\n        bool force_reanchor,\n        double window_seconds,\n        double overlap_seconds,\n        double seek_preroll_seconds,\n        std::vector<float>& audio) {\n'''
if old not in s:
    raise SystemExit('decode signature anchor not found')
s = s.replace(old, new, 1)

old = '''            if (!reanchor_decoder(\n                    state,\n                    state.path,\n                    requested_start_seconds)) {\n'''
new = '''            if (!reanchor_decoder(\n                    state,\n                    state.path,\n                    requested_start_seconds,\n                    seek_preroll_seconds)) {\n'''
count = s.count(old)
if count != 2:
    raise SystemExit(f'expected 2 reanchor calls, found {count}')
s = s.replace(old, new, 2)

old = '''                    const double decode_overlap = job.need_stems\n                        ? kCacheOverlapSeconds\n                        : kOriginalQuickOverlapSeconds;\n\n                    const bool decoded =\n                        decode_exact_block(\n                            active_decoder,\n                            job.start_seconds,\n                            job.force_reanchor,\n                            decode_window,\n                            decode_overlap,\n                            input);\n'''
new = '''                    const double decode_overlap = job.need_stems\n                        ? kCacheOverlapSeconds\n                        : kOriginalQuickOverlapSeconds;\n                    const double decode_preroll = job.need_stems\n                        ? kDecodeSeekPrerollSeconds\n                        : kOriginalDecodeSeekPrerollSeconds;\n\n                    const bool decoded =\n                        decode_exact_block(\n                            active_decoder,\n                            job.start_seconds,\n                            job.force_reanchor,\n                            decode_window,\n                            decode_overlap,\n                            decode_preroll,\n                            input);\n'''
if old not in s:
    raise SystemExit('decode call anchor not found')
s = s.replace(old, new, 1)

p.write_text(s, encoding='utf-8')
print('patched low-latency Original scratch decode')
