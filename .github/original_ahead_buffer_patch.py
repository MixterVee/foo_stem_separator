from pathlib import Path

p = Path('stem_dsp.cpp')
s = p.read_text(encoding='utf-8')

old = '''constexpr double kOriginalQuickCacheSeconds = 1.5;\nconstexpr double kOriginalQuickOverlapSeconds = 0.25;\nconstexpr double kOriginalPrefetchSeconds = 0.50;\n'''
new = '''constexpr double kOriginalQuickCacheSeconds = 1.5;\nconstexpr double kOriginalQuickOverlapSeconds = 0.25;\n// Ordinary Original decoding is cheap and must stay well ahead of a platter.\n// Keep quick random transport jobs tiny, but let the separate sequential\n// background decoder publish a much larger future window that rapid forward\n// scratches can read immediately.\nconstexpr double kOriginalBackgroundCacheSeconds = 30.0;\nconstexpr double kOriginalBackgroundOverlapSeconds = 3.0;\nconstexpr double kOriginalBackgroundPrefetchSeconds = 12.0;\n'''
if old not in s:
    raise SystemExit('Original cache constants anchor not found')
s = s.replace(old, new, 1)

old = '''                // Center a compact Original window on the grab point. It is\n                // decoder-only and is intentionally much smaller than a stem\n                // analysis block so first-motion PCM becomes available quickly.\n                const double preview_start = (std::max)(\n                    0.0, seconds - kOriginalQuickCacheSeconds * 0.5);\n                m_jobs.emplace_front(cache_job{\n                    m_generation, m_path, preview_start, true, true, false});\n                m_job_pending = true;\n'''
new = '''                // First publish a compact transport window around the grab point.\n                // Then, on the independent sequential decoder timeline, fill a\n                // large future region. Rapid scrub retargeting may coalesce the\n                // transport-preview jobs but never cancels this background job.\n                const double preview_start = (std::max)(\n                    0.0, seconds - kOriginalQuickCacheSeconds * 0.5);\n                m_jobs.emplace_front(cache_job{\n                    m_generation, m_path, preview_start, true, true, false});\n\n                const double background_start = (std::max)(\n                    0.0, seconds - kOriginalBackgroundOverlapSeconds);\n                m_jobs.emplace_back(cache_job{\n                    m_generation, m_path, background_start, true, false, false});\n                m_job_pending = true;\n'''
if old not in s:
    raise SystemExit('seek Original queue anchor not found')
s = s.replace(old, new, 1)

old = '''            double next = 0.0;\n            if (covering_end < 0.0) {\n                // A seek/jump landed outside cached Original PCM. Center a fresh\n                // quick window so both scratch directions become available fast.\n                next = (std::max)(\n                    0.0, playback_seconds - kOriginalQuickCacheSeconds * 0.5);\n            } else if (covering_end - playback_seconds <=\n                       kOriginalPrefetchSeconds) {\n                // Extend ahead with the normal overlap before the playhead reaches\n                // the current window edge.\n                next = (std::max)(\n                    0.0, covering_end - kOriginalQuickOverlapSeconds);\n            } else {\n                return;\n            }\n\n            m_jobs.emplace_back(cache_job{\n                m_generation, m_path, next, true, false, false});\n'''
new = '''            double next = 0.0;\n            if (covering_end < 0.0) {\n                // A seek/jump landed outside cached Original PCM. Start a large\n                // background window slightly behind the playhead so the platter\n                // immediately gains both history and substantial future material.\n                next = (std::max)(\n                    0.0, playback_seconds - kOriginalBackgroundOverlapSeconds);\n            } else if (covering_end - playback_seconds <=\n                       kOriginalBackgroundPrefetchSeconds) {\n                // Extend the sequential future cache long before the platter can\n                // reach its edge. This job is decoder-only and normally finishes\n                // far faster than real-time playback.\n                next = (std::max)(\n                    0.0, covering_end - kOriginalBackgroundOverlapSeconds);\n            } else {\n                return;\n            }\n\n            m_jobs.emplace_back(cache_job{\n                m_generation, m_path, next, false, false, false});\n'''
if old not in s:
    raise SystemExit('ensure_ahead Original anchor not found')
s = s.replace(old, new, 1)

old = '''                    const double decode_window = job.need_stems\n                        ? kCacheSeconds\n                        : kOriginalQuickCacheSeconds;\n                    const double decode_overlap = job.need_stems\n                        ? kCacheOverlapSeconds\n                        : kOriginalQuickOverlapSeconds;\n                    const double decode_preroll = job.need_stems\n                        ? kDecodeSeekPrerollSeconds\n                        : kOriginalDecodeSeekPrerollSeconds;\n'''
new = '''                    const double decode_window = job.need_stems\n                        ? kCacheSeconds\n                        : (job.transport_preview\n                            ? kOriginalQuickCacheSeconds\n                            : kOriginalBackgroundCacheSeconds);\n                    const double decode_overlap = job.need_stems\n                        ? kCacheOverlapSeconds\n                        : (job.transport_preview\n                            ? kOriginalQuickOverlapSeconds\n                            : kOriginalBackgroundOverlapSeconds);\n                    const double decode_preroll = job.need_stems\n                        ? kDecodeSeekPrerollSeconds\n                        : kOriginalDecodeSeekPrerollSeconds;\n'''
if old not in s:
    raise SystemExit('worker Original window selector anchor not found')
s = s.replace(old, new, 1)

p.write_text(s, encoding='utf-8')
print('patched large background Original ahead buffer')
