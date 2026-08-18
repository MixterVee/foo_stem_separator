from pathlib import Path

path = Path('stem_dsp.cpp')
s = path.read_text(encoding='utf-8-sig')


def repl(old: str, new: str, label: str) -> None:
    global s
    count = s.count(old)
    if count != 1:
        raise RuntimeError(f'{label}: expected 1 marker, found {count}')
    s = s.replace(old, new, 1)


repl(
'''            // Pre-cache is enabled by default. Warm the ONNX/Spleeter session on
            // the cache worker immediately so the first user stem request does not
            // also pay DLL/model/session creation. This never blocks foobar's UI or
            // ordinary Original playback. If pre-cache is disabled, preserve lazy
            // initialization exactly as before.
            std::unique_ptr<onnxstem::engine> engine;
            if (stem_precache::enabled()) {
                engine = std::make_unique<onnxstem::engine>();
                engine->ready();
            }
''',
'''            // Disk restore must run before any expensive ONNX initialization.
            // Cached tracks can therefore become ready immediately after restart
            // without paying model/session startup at all. For uncached tracks the
            // first real stem job still constructs the engine lazily below.
            std::unique_ptr<onnxstem::engine> engine;
''',
'engine warmup ordering')

repl(
'''                    if (decoded && !input.empty() && stem_payload_ok && job.need_stems) {
                        if (job.start_seconds <= 0.000001) {
                            apply_first_block_fade(vocals);
                            apply_first_block_fade(instrumental);
                        }
                        const uint64_t start_frame = static_cast<uint64_t>(
                            job.start_seconds * static_cast<double>(kCacheRate) + 0.5);
                        persistent_stem_cache::save(
                            job.path, start_frame, vocals, instrumental);
                    }

                    {
''',
'''                    if (decoded && !input.empty() && stem_payload_ok && job.need_stems) {
                        if (job.start_seconds <= 0.000001) {
                            apply_first_block_fade(vocals);
                            apply_first_block_fade(instrumental);
                        }
                    }

                    std::shared_ptr<cache_segment> persist_segment;

                    {
''',
'defer compressed save')

repl(
'''                                // Completed segments remain valid for the whole track.
                                // This also makes short reverse moves instant instead of
                                // re-running Spleeter after every release/seek.
                                m_segments.push_back(std::make_shared<cache_segment>(std::move(seg)));
''',
'''                                // Completed segments remain valid for the whole track.
                                // This also makes short reverse moves instant instead of
                                // re-running Spleeter after every release/seek.
                                auto completed_segment =
                                    std::make_shared<cache_segment>(std::move(seg));
                                if (job.need_stems) {
                                    persist_segment = completed_segment;
                                }
                                m_segments.push_back(std::move(completed_segment));
''',
'publish completed segment')

repl(
'''                    m_ready_cv.notify_all();
                }
                catch (const std::exception&) {
''',
'''                    // Make the newly separated PCM visible to playback and release
                    // any mode-switch waiter before doing potentially slower lossless
                    // compression and disk I/O. This preserves the sample-locked
                    // Original -> Vocals/Instrumental handoff.
                    m_ready_cv.notify_all();

                    if (persist_segment) {
                        const uint64_t start_frame = static_cast<uint64_t>(
                            persist_segment->start_seconds *
                            static_cast<double>(kCacheRate) + 0.5);
                        persistent_stem_cache::save(
                            job.path,
                            start_frame,
                            persist_segment->vocals,
                            persist_segment->instrumental);
                    }
                }
                catch (const std::exception&) {
''',
'publish before persistence')

path.write_text(s, encoding='utf-8')
print('Applied cache compression restore/handoff timing patch.')
