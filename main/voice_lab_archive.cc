#include "voice_lab_archive.h"

#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <cJSON.h>
#include <dirent.h>
#include <psa/crypto.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <new>

namespace {
constexpr size_t kSegmentSamples = 16000;
constexpr char kTag[] = "VoiceLabArchive";
std::mutex file_mutex;
bool SafeId(const std::string& id) {
    return !id.empty() && id.size() <= 128 &&
           id.find_first_not_of(
               "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") ==
               std::string::npos;
}
std::string Read(const std::string& path, size_t limit = 65536) {
    std::lock_guard<std::mutex> lock(file_mutex);
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        f = fopen((path + ".bak").c_str(), "rb");
    if (!f)
        return {};
    std::string result(limit, '\0');
    auto size = fread(result.data(), 1, limit, f);
    bool good = !ferror(f) && fgetc(f) == EOF;
    fclose(f);
    result.resize(size);
    return good ? result : std::string{};
}
bool Write(const std::string& path, const void* data, size_t size) {
    std::lock_guard<std::mutex> lock(file_mutex);
    auto tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f)
        return false;
    bool ok = fwrite(data, 1, size, f) == size && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0)
        ok = false;
    if (ok) {
        // FAT rename does not replace an existing destination. Retain the old
        // committed file until the new one is installed; Read recovers it if
        // power is lost between the two renames.
        const auto backup = path + ".bak";
        struct stat st{};
        if (stat(path.c_str(), &st) == 0) {
            unlink(backup.c_str());
            ok = rename(path.c_str(), backup.c_str()) == 0;
        }
        if (ok)
            ok = rename(tmp.c_str(), path.c_str()) == 0;
        if (ok)
            unlink(backup.c_str());
    }
    if (!ok)
        ESP_LOGE(kTag, "SD commit failed: %s errno=%d", path.c_str(), errno);
    return ok;
}
std::string Json(cJSON* root) {
    char* raw = cJSON_PrintUnformatted(root);
    std::string result = raw ? raw : "";
    cJSON_free(raw);
    return result;
}
std::string Digest(const std::string& bytes) {
    unsigned char digest[32];
    size_t length = 0;
    if (psa_crypto_init() != PSA_SUCCESS ||
        psa_hash_compute(PSA_ALG_SHA_256, reinterpret_cast<const unsigned char*>(bytes.data()),
                         bytes.size(), digest, sizeof(digest), &length) != PSA_SUCCESS ||
        length != 32)
        return {};
    char text[65];
    for (unsigned i = 0; i < 32; ++i)
        snprintf(text + i * 2, 3, "%02x", digest[i]);
    return text;
}
std::string ChunkName(unsigned sequence) {
    char name[32];
    snprintf(name, sizeof(name), "/%010u.pcm", sequence);
    return name;
}
}  // namespace

bool VoiceLabArchive::Initialize(const std::string& root, Upload upload) {
    std::lock_guard<std::mutex> lock(operation_);
    if (queue_)
        return true;
    if (root.empty())
        return false;
    root_ = root + "/voice-lab";
    mkdir(root_.c_str(), 0770);
    struct stat st{};
    if (stat(root_.c_str(), &st) || !S_ISDIR(st.st_mode))
        return false;
    // A reboot ends capture, never silently resumes a previous authorization.
    // Keep the last committed prefix and explicitly mark the interruption.
    if (DIR* tasks = opendir(root_.c_str())) {
        while (auto* entry = readdir(tasks)) {
            if (!SafeId(entry->d_name))
                continue;
            const auto folder = root_ + "/" + entry->d_name;
            const auto path = folder + "/manifest.json";
            auto bytes = Read(path, 4096);
            cJSON* manifest = cJSON_Parse(bytes.c_str());
            if (!manifest)
                continue;
            if (!cJSON_IsTrue(cJSON_GetObjectItem(manifest, "stopped"))) {
                cJSON_ReplaceItemInObject(manifest, "stopped", cJSON_CreateBool(true));
                cJSON_ReplaceItemInObject(manifest, "interrupted", cJSON_CreateBool(true));
                cJSON_ReplaceItemInObject(manifest, "manualRecovery", cJSON_CreateBool(true));
                cJSON_ReplaceItemInObject(manifest, "storageError",
                                          cJSON_CreateString("capture_interrupted_by_restart"));
                bytes = Json(manifest);
                if (!Write(path, bytes.data(), bytes.size())) {
                    cJSON_Delete(manifest);
                    closedir(tasks);
                    return false;
                }
            }
            cJSON_Delete(manifest);
        }
        closedir(tasks);
    }
    upload_ = std::move(upload);
    queue_ = xQueueCreate(128, sizeof(std::vector<int16_t>*));
    if (!queue_)
        return false;
    segment_.reserve(kSegmentSamples);
    TaskHandle_t writer = nullptr;
    if (xTaskCreate([](void* arg) { static_cast<VoiceLabArchive*>(arg)->WriteLoop(); },
                    "vl_sd_writer", 6144, this, 3, &writer) != pdPASS) {
        vQueueDelete(queue_);
        queue_ = nullptr;
        return false;
    }
    if (xTaskCreate([](void* arg) { static_cast<VoiceLabArchive*>(arg)->UploadLoop(); },
                    "vl_sd_upload", 8192, this, 1, nullptr) != pdPASS) {
        vTaskDelete(writer);
        vQueueDelete(queue_);
        queue_ = nullptr;
        return false;
    }
    return true;
}

bool VoiceLabArchive::Begin(const std::string& run, uint64_t boot, uint64_t sample_start) {
    std::lock_guard<std::mutex> lock(operation_);
    if (!queue_ || active_ || !SafeId(run))
        return false;
    directory_ = root_ + "/" + run;
    if (mkdir(directory_.c_str(), 0770) != 0)
        return false;  // Never overwrite an older task.
    run_ = run;
    boot_ = std::to_string(boot);
    sample_start_ = sample_start;
    samples_ = 0;
    sequence_ = 0;
    segment_.clear();
    failed_ = false;
    manual_ = false;
    finishing_ = false;
    if (!SaveManifest(false, false))
        return false;
    bool registered = false;
    for (unsigned attempt = 0; attempt < 3 && !registered; ++attempt) {
        const auto registration =
            upload_("PUT", "/" + run_, Read(directory_ + "/manifest.json", 4096), false);
        cJSON* receipt = cJSON_Parse(registration.c_str());
        const auto id = cJSON_GetObjectItem(receipt, "runId");
        registered = cJSON_IsString(id) && run_ == id->valuestring;
        cJSON_Delete(receipt);
        if (!registered)
            ESP_LOGW(kTag, "Archive registration attempt %u failed: run=%s", attempt + 1,
                     run_.c_str());
    }
    if (!registered) {
        ESP_LOGE(kTag, "Archive registration failed: run=%s", run_.c_str());
        SaveManifest(true, true);
        return false;
    }
    active_ = true;
    return true;
}

bool VoiceLabArchive::Push(const std::vector<int16_t>& pcm) {
    if (!active_ || failed_ || finishing_)
        return false;
    if (pcm.empty() || pcm.size() > kSegmentSamples) {
        failed_ = true;
        return false;
    }
    auto* copy = new (std::nothrow) std::vector<int16_t>(pcm);
    if (!copy || xQueueSend(queue_, &copy, 0) != pdTRUE) {
        delete copy;
        failed_ = true;
        ESP_LOGE(kTag, "SD capture queue full; recording interrupted");
        return false;
    }
    return true;
}

bool VoiceLabArchive::SaveManifest(bool stopped, bool interrupted) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "bootId", boot_.c_str());
    cJSON_AddNumberToObject(root, "sampleStart", static_cast<double>(sample_start_));
    cJSON_AddNumberToObject(root, "capturedSamples", static_cast<double>(samples_.load()));
    cJSON_AddBoolToObject(root, "stopped", stopped);
    cJSON_AddBoolToObject(root, "interrupted", interrupted || failed_);
    cJSON_AddBoolToObject(root, "manualRecovery", manual_);
    cJSON_AddNumberToObject(root, "localBytes", static_cast<double>(samples_.load() * 2));
    cJSON_AddStringToObject(root, "storageError", failed_ ? "sd_write_or_queue_failed" : "");
    auto body = Json(root);
    cJSON_Delete(root);
    return Write(directory_ + "/manifest.json", body.data(), body.size());
}

bool VoiceLabArchive::Seal() {
    if (segment_.empty())
        return true;
    uint64_t total = 0, free = 0;
    const auto mount = root_.substr(0, root_.size() - strlen("/voice-lab"));
    if (esp_vfs_fat_info(mount.c_str(), &total, &free) != ESP_OK || free < 1024 * 1024) {
        ESP_LOGE(kTag, "SD space exhausted or unavailable; unuploaded files retained");
        return false;
    }
    if (!Write(directory_ + ChunkName(sequence_), segment_.data(), segment_.size() * 2))
        return false;
    ++sequence_;
    samples_.fetch_add(segment_.size());
    segment_.clear();
    return SaveManifest(false, false);
}

void VoiceLabArchive::WriteLoop() {
    while (true) {
        std::vector<int16_t>* pcm = nullptr;
        if (xQueueReceive(queue_, &pcm, pdMS_TO_TICKS(20)) == pdTRUE) {
            for (auto sample : *pcm) {
                if (failed_)
                    break;
                segment_.push_back(sample);
                if (segment_.size() == kSegmentSamples && !Seal()) {
                    failed_ = true;
                    segment_.clear();
                    break;
                }
            }
            delete pcm;
        }
        if (finishing_ && uxQueueMessagesWaiting(queue_) == 0) {
            if (!failed_ && !Seal())
                failed_ = true;
            if (!SaveManifest(true, finish_interrupted_))
                failed_ = true;
            finishing_ = false;
            active_ = false;
        }
    }
}

bool VoiceLabArchive::Finish(bool interrupted) {
    if (!active_)
        return !failed_;
    finish_interrupted_ = interrupted;
    finishing_ = true;
    for (unsigned i = 0; active_ && i < 500; ++i)
        vTaskDelay(pdMS_TO_TICKS(10));
    return !active_ && !failed_;
}

void VoiceLabArchive::UploadLoop() {
    while (true) {
        bool transferred = false;
        DIR* dir = opendir(root_.c_str());
        if (dir) {
            while (auto* entry = readdir(dir)) {
                std::string run = entry->d_name;
                if (!SafeId(run))
                    continue;
                std::string folder = root_ + "/" + run;
                struct stat synced{};
                if (stat((folder + "/synced.json").c_str(), &synced) == 0)
                    continue;
                auto report = Read(folder + "/manifest.json", 4096);
                if (report.empty())
                    continue;
                uint64_t local_bytes = 0;
                if (DIR* inventory = opendir(folder.c_str())) {
                    while (auto* file = readdir(inventory)) {
                        std::string name = file->d_name;
                        struct stat info{};
                        if (name.size() == 14 && name.substr(10) == ".pcm" &&
                            stat((folder + "/" + name).c_str(), &info) == 0)
                            local_bytes += info.st_size;
                    }
                    closedir(inventory);
                }
                if (cJSON* metadata = cJSON_Parse(report.c_str())) {
                    cJSON_ReplaceItemInObject(metadata, "localBytes",
                                              cJSON_CreateNumber(local_bytes));
                    report = Json(metadata);
                    cJSON_Delete(metadata);
                }
                auto response = upload_("PUT", "/" + run, report, false);
                cJSON* status = cJSON_Parse(response.c_str());
                if (!status)
                    continue;
                const bool complete = cJSON_IsTrue(cJSON_GetObjectItem(status, "complete"));
                const bool manual = cJSON_IsTrue(cJSON_GetObjectItem(status, "manualRecovery"));
                const bool requested = cJSON_IsTrue(cJSON_GetObjectItem(status, "requested"));
                const bool stopped = cJSON_IsTrue(cJSON_GetObjectItem(status, "stopped"));
                const auto captured = cJSON_GetObjectItem(status, "capturedSamples");
                const uint64_t captured_bytes =
                    cJSON_IsNumber(captured) ? captured->valuedouble * 2 : 0;
                cJSON_Delete(status);
                if (complete) {
                    if (Write(folder + "/synced.json", response.data(), response.size())) {
                        std::lock_guard<std::mutex> lock(operation_);
                        if (run == run_ && !active_)
                            manual_ = false;
                    }
                    continue;
                }
                if (manual && !requested)
                    continue;
                DIR* chunks = opendir(folder.c_str());
                bool pending = false;
                unsigned uploaded_chunks = 0;
                if (!chunks)
                    continue;
                while (auto* chunk = readdir(chunks)) {
                    std::string name = chunk->d_name;
                    if (name.size() != 14 || name.substr(10) != ".pcm" ||
                        name.substr(0, 10).find_first_not_of("0123456789") != std::string::npos)
                        continue;
                    auto pcm = Read(folder + "/" + name, 32000);
                    if (pcm.empty()) {
                        pending = true;
                        continue;
                    }
                    const auto sequence = strtoull(name.substr(0, 10).c_str(), nullptr, 10);
                    if (sequence > UINT32_MAX)
                        continue;
                    // Ignore a file sealed just before an uncommitted/crashed manifest.
                    if (sequence * 32000ULL + pcm.size() > captured_bytes)
                        continue;
                    pending = true;
                    auto sha = Digest(pcm);
                    auto receipt = upload_(
                        "PUT", "/" + run + "/chunks/" + std::to_string(sequence) + "?sha256=" + sha,
                        pcm, true);
                    bool confirmed = false;
                    cJSON* ack = cJSON_Parse(receipt.c_str());
                    if (ack) {
                        auto id = cJSON_GetObjectItem(ack, "runId");
                        auto digest = cJSON_GetObjectItem(ack, "sha256");
                        auto seq = cJSON_GetObjectItem(ack, "sequence");
                        auto bytes = cJSON_GetObjectItem(ack, "byteCount");
                        if (cJSON_IsTrue(cJSON_GetObjectItem(ack, "durable")) &&
                            cJSON_IsString(id) && run == id->valuestring &&
                            cJSON_IsString(digest) && sha == digest->valuestring &&
                            cJSON_IsNumber(seq) && seq->valuedouble == sequence &&
                            cJSON_IsNumber(bytes) && bytes->valuedouble == pcm.size()) {
                            confirmed = unlink((folder + "/" + name).c_str()) == 0;
                        }
                        cJSON_Delete(ack);
                    }
                    transferred = transferred || confirmed;
                    // Keep a bounded burst per task so other tasks still report progress.
                    if (!confirmed || ++uploaded_chunks >= 4)
                        break;
                }
                closedir(chunks);
                if (stopped && !pending)
                    upload_("POST", "/" + run + "/complete", "{}", false);
            }
            closedir(dir);
        }
        vTaskDelay(pdMS_TO_TICKS(transferred ? 50 : 2000));
    }
}
