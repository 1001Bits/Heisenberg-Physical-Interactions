#include "physics-interaction/grab/SavedGrabOffsetStore.h"

#include "physics-interaction/PhysicsLog.h"

#include "RE/Bethesda/TESForms.h"
#include "RE/Bethesda/TESDataHandler.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace rock::saved_grab_offset
{
    namespace
    {
        constexpr auto kSavedGrabOffsetsPath = R"(Data\F4SE\Plugins\Heisenberg\SavedGrabOffsets)";

        std::string resolveStoreDirectory()
        {
            // Keep embedded ROCK's generated data under Heisenberg as well.
            // This prevents any feature from recreating Documents\...\ROCK_Config
            // after the standalone ROCK.ini was folded into Heisenberg_F4VR.ini.
            return kSavedGrabOffsetsPath;
        }

        // Plugin names become file-name components; keep letters, digits,
        // dots, dashes, underscores and spaces, replace the rest.
        std::string sanitizeForFileName(std::string_view text)
        {
            std::string out;
            out.reserve(text.size());
            for (const char c : text) {
                const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_' || c == ' ';
                out.push_back(ok ? c : '_');
            }
            return out;
        }

        struct PendingWrite
        {
            std::string path;
            std::string content;
        };

        /*
         * Process-lifetime store instance (Meyer's singleton: lazy-init on
         * first use, no static-initialization-order dependency). Its
         * destructor drains the write queue and joins the writer thread at
         * static teardown.
         */
        class Store
        {
        public:
            Store() :
                _directory(resolveStoreDirectory())
            {
            }

            ~Store()
            {
                shutdown();
            }

            std::string filePathForObject(const FormRef& object) const
            {
                char idText[16]{};
                std::snprintf(idText, sizeof(idText), "%08X", object.localFormId);
                return _directory + "\\" + sanitizeForFileName(object.plugin) + "_" + idText + ".json";
            }

            void preload()
            {
                std::error_code ec;
                if (!std::filesystem::exists(_directory, ec) || !std::filesystem::is_directory(_directory, ec)) {
                    return;
                }

                std::unordered_map<std::string, SavedGrabOffsetFile> loaded;
                std::size_t failedCount = 0;
                for (const auto& entry : std::filesystem::directory_iterator(_directory, ec)) {
                    if (ec) {
                        break;
                    }

                    // Fresh per-entry code: reusing the directory_iterator's `ec` here meant one
                    // entry's transient is_regular_file failure (e.g. a file deleted/permission-
                    // denied mid-scan) left a stale error in `ec`, which the NEXT iteration's
                    // `if (ec) break;` then misread as the directory_iterator itself having
                    // failed - silently truncating the scan and dropping every saved grab offset
                    // that happened to sort after the problem entry.
                    std::error_code entryEc;
                    if (!entry.is_regular_file(entryEc) || entry.path().extension() != ".json") {
                        continue;
                    }

                    std::ifstream stream(entry.path(), std::ios::binary);
                    if (!stream) {
                        ++failedCount;
                        continue;
                    }
                    std::ostringstream buffer;
                    buffer << stream.rdbuf();

                    SavedGrabOffsetFile file{};
                    if (!parse(buffer.str(), file, nullptr)) {
                        ++failedCount;
                        continue;
                    }
                    loaded.emplace(entry.path().string(), std::move(file));
                }

                std::lock_guard lock(_mutex);
                _cache = std::move(loaded);
                ROCK_LOG_INFO(Config, "Loaded {} saved grab offset(s) ({} unreadable)", _cache.size(), failedCount);
            }

            bool load(const FormRef& object, SavedGrabOffsetFile& out, std::string* outError) const
            {
                if (outError) {
                    outError->clear();
                }
                if (object.empty()) {
                    return false;
                }
                const auto path = filePathForObject(object);
                std::lock_guard lock(_mutex);
                const auto it = _cache.find(path);
                if (it == _cache.end()) {
                    return false;  // no saved offset for this object; normal, empty error
                }
                out = it->second;
                return true;
            }

            void save(const SavedGrabOffsetFile& file)
            {
                if (file.object.empty()) {
                    return;
                }
                PendingWrite write{ filePathForObject(file.object), serialize(file) };
                {
                    std::lock_guard lock(_mutex);
                    // Cache first, so the very next grab of this object
                    // (even before the writer thread finishes) sees it.
                    _cache[write.path] = file;

                    // Latest-wins per file: replace a still-pending write of
                    // the same object instead of queueing behind it.
                    bool replaced = false;
                    for (auto& pending : _queue) {
                        if (pending.path == write.path) {
                            pending.content = std::move(write.content);
                            replaced = true;
                            break;
                        }
                    }
                    if (!replaced) {
                        _queue.push_back(std::move(write));
                    }
                }
                ensureWriterStarted();
                _wake.notify_one();
            }

        private:
            void ensureWriterStarted()
            {
                std::lock_guard lock(_mutex);
                if (_writerStarted) {
                    return;
                }
                _writerStarted = true;
                _stop = false;
                _writer = std::thread([this]() { writerLoop(); });
            }

            void writerLoop()
            {
                for (;;) {
                    PendingWrite write;
                    {
                        std::unique_lock lock(_mutex);
                        _wake.wait(lock, [this]() { return _stop || !_queue.empty(); });
                        if (_queue.empty()) {
                            if (_stop) {
                                break;
                            }
                            continue;
                        }
                        write = std::move(_queue.front());
                        _queue.pop_front();
                    }

                    std::error_code ec;
                    std::filesystem::create_directories(std::filesystem::path(write.path).parent_path(), ec);
                    const auto tempPath = write.path + ".tmp";
                    {
                        std::ofstream stream(tempPath, std::ios::binary | std::ios::trunc);
                        if (!stream) {
                            ROCK_LOG_WARN(Config, "Saved grab offset: could not open '{}' for writing", tempPath);
                            continue;
                        }
                        stream.write(write.content.data(), static_cast<std::streamsize>(write.content.size()));
                        if (!stream) {
                            ROCK_LOG_WARN(Config, "Saved grab offset: write to '{}' failed", tempPath);
                            continue;
                        }
                    }
                    std::filesystem::rename(tempPath, write.path, ec);
                    if (ec) {
                        ROCK_LOG_WARN(Config, "Saved grab offset: rename to '{}' failed: {}", write.path, ec.message());
                    }
                }
                // Set only after the loop's _mutex-guarded section has been released (never
                // inside it) - shutdown() below treats observing this as true as its signal that
                // _mutex is safe to reacquire. See shutdown() for why that ordering matters.
                _writerExited.store(true, std::memory_order_release);
            }

            void shutdown()
            {
                {
                    std::lock_guard lock(_mutex);
                    if (!_writerStarted) {
                        return;
                    }
                    _stop = true;
                }
                _wake.notify_one();

                if (_writer.joinable()) {
                    // Bounded wait, not an unconditional join(): this destructor can run during
                    // static teardown via ExitProcess(), which force-suspends every OTHER thread
                    // in the process - including the writer, possibly mid-writerLoop() while it
                    // still holds _mutex or a CRT file-handle lock - before our destructor runs
                    // on the (sole surviving) main thread. An unconditional join(), or the old
                    // unconditional re-lock of _mutex that followed it, would then wait forever
                    // on a thread Windows will never resume, hanging the whole process on every
                    // exit. Give the writer a short window to notice _stop and finish; if it
                    // doesn't, abandon it instead of hanging - the process is exiting either way,
                    // so a detached thread is harmless, and losing the last pending write beats
                    // an unkillable hang.
                    constexpr auto kShutdownWaitTimeout = std::chrono::milliseconds(500);
                    constexpr auto kPollInterval = std::chrono::milliseconds(10);
                    const auto deadline = std::chrono::steady_clock::now() + kShutdownWaitTimeout;
                    while (!_writerExited.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
                        std::this_thread::sleep_for(kPollInterval);
                    }

                    if (_writerExited.load(std::memory_order_acquire)) {
                        // Confirmed the writer already released _mutex and is only moments from
                        // returning out of the OS thread proc - safe to join (near-instant) and
                        // safe to touch _mutex again below.
                        _writer.join();
                        std::lock_guard lock(_mutex);
                        _writerStarted = false;
                    } else {
                        // Never reacquire _mutex on this path: it may be held by a thread that
                        // will never run again. Leave _writerStarted set; this Store instance is
                        // being destroyed regardless, and no other call site can observe the
                        // stale flag afterward.
                        _writer.detach();
                    }
                } else {
                    std::lock_guard lock(_mutex);
                    _writerStarted = false;
                }
            }

            std::string _directory;
            std::thread _writer;
            mutable std::mutex _mutex;
            std::condition_variable _wake;
            std::deque<PendingWrite> _queue;
            std::unordered_map<std::string, SavedGrabOffsetFile> _cache;
            bool _stop{ false };
            bool _writerStarted{ false };
            std::atomic<bool> _writerExited{ false };
        };

        Store& instance()
        {
            static Store store;
            return store;
        }
    }

    FormRef formRefFromRuntimeId(std::uint32_t runtimeFormId)
    {
        if (runtimeFormId == 0) {
            return {};
        }
        auto* form = RE::TESForm::GetFormByID(runtimeFormId);
        if (!form) {
            return {};
        }
        auto* file = form->GetFile(0);
        if (!file) {
            return {};
        }
        FormRef ref;
        ref.plugin = std::string(file->GetFilename());
        ref.localFormId = form->GetLocalFormID();
        if (ref.plugin.empty()) {
            return {};
        }
        return ref;
    }

    void preload()
    {
        instance().preload();
    }

    bool load(const FormRef& object, SavedGrabOffsetFile& out, std::string* outError)
    {
        return instance().load(object, out, outError);
    }

    void save(const SavedGrabOffsetFile& file)
    {
        instance().save(file);
    }
}
