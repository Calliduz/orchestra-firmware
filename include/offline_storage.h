#ifndef OFFLINE_STORAGE_H
#define OFFLINE_STORAGE_H

#include <Arduino.h>
#include <Preferences.h>
#include <vector>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/**
 * Structure representing an authorized professor / faculty instructor
 * Cached in local ESP32 NVS (Non-Volatile Storage) to allow offline session unlock/lock
 */
struct FacultyCacheRecord {
    String rfidUid;
    String name;
    String role;
};

/**
 * Structure representing an enrolled student cached in terminal memory
 * Allows instant offline student name/ID resolution so students get full feedback!
 */
struct StudentCacheRecord {
    String rfidUid;
    String studentId;
    String name;
};

/**
 * Structure representing an offline attendance log recorded during Wi-Fi outage
 */
struct OfflineAttendanceRecord {
    char rfidUid[24];
    char timestamp[32];
    char authMethod[20];
};

class OfflineStorageManager {
private:
    SemaphoreHandle_t storageMutex = NULL;
    Preferences prefProf;
    Preferences prefLogs;
    std::vector<FacultyCacheRecord> facultyCache;
    std::vector<StudentCacheRecord> studentCache;
    std::vector<OfflineAttendanceRecord> pendingLogs;

    String offlineSessionAction = "CONTINUE"; // "START", "CLOSE", or "CONTINUE"
    String offlineFacultyUid = "";

    void ensureMutex() {
        if (storageMutex == NULL) {
            storageMutex = xSemaphoreCreateRecursiveMutex();
        }
    }

    void lock() {
        ensureMutex();
        if (storageMutex) {
            xSemaphoreTakeRecursive(storageMutex, portMAX_DELAY);
        }
    }

    void unlock() {
        if (storageMutex) {
            xSemaphoreGiveRecursive(storageMutex);
        }
    }

    class LockGuard {
        OfflineStorageManager &mgr;
    public:
        explicit LockGuard(OfflineStorageManager &m) : mgr(m) { mgr.lock(); }
        ~LockGuard() { mgr.unlock(); }
    };

    void seedDefaultFaculty() {
        // Purged hardcoded faculty PII. Authorized faculty dynamically synced from backend
        // via syncTerminalStatus() and cached in persistent NVS.
    }

    void seedDefaultStudents() {
        // Purged hardcoded student PII. Terminal delegates student identity verification to
        // live backend tap validation and NVS offline log buffering.
    }

    void addOrUpdateFacultyInMemory(const String &uid, const String &name, const String &role) {
        String cleanUid = normalizeUid(uid);
        for (auto &f : facultyCache) {
            if (f.rfidUid.equalsIgnoreCase(cleanUid)) {
                f.name = name;
                f.role = role;
                return;
            }
        }
        facultyCache.push_back({cleanUid, name, role});
    }

    void addOrUpdateStudentInMemory(const String &uid, const String &studentId, const String &name) {
        String cleanUid = normalizeUid(uid);
        for (auto &s : studentCache) {
            if (s.rfidUid.equalsIgnoreCase(cleanUid)) {
                s.studentId = studentId;
                s.name = name;
                return;
            }
        }
        studentCache.push_back({cleanUid, studentId, name});
    }

public:
    OfflineStorageManager() {
        ensureMutex();
    }

    static String normalizeUid(const String &uid) {
        String clean = "";
        for (size_t i = 0; i < uid.length(); i++) {
            char c = uid.charAt(i);
            if (isHexadecimalDigit(c)) {
                clean += (char)toupper(c);
            }
        }
        return clean;
    }

    void begin() {
        LockGuard guard(*this);
        prefProf.begin("orch_prof", false);
        prefLogs.begin("orch_logs", false);

        seedDefaultFaculty();
        seedDefaultStudents();
        loadFacultyFromNvs();
        loadLogsFromNvs();

        Serial.printf(F("[OFFLINE STORAGE] Initialized. %u faculty, %u students cached, %u pending logs in NVS.\n"),
                      facultyCache.size(), studentCache.size(), pendingLogs.size());
    }

    bool findStudent(const String &cardUid, String &outName, String &outStudentId) {
        LockGuard guard(*this);
        String clean = normalizeUid(cardUid);
        for (const auto &s : studentCache) {
            if (s.rfidUid.equalsIgnoreCase(clean)) {
                outName = s.name;
                outStudentId = s.studentId;
                return true;
            }
        }
        if (clean.length() > 0) {
            outName = "Student (0x" + clean + ")";
            outStudentId = "OFFLINE-TAG";
            return true;
        }
        return false;
    }

    // =========================================================================
    // LOCAL PROFESSOR UID CACHE
    // =========================================================================

    void loadFacultyFromNvs() {
        LockGuard guard(*this);
        int count = prefProf.getInt("count", 0);
        if (count <= 0) return;

        for (int i = 0; i < count && i < 20; i++) {
            char keyUid[16], keyName[16], keyRole[16];
            snprintf(keyUid, sizeof(keyUid), "u_%d", i);
            snprintf(keyName, sizeof(keyName), "n_%d", i);
            snprintf(keyRole, sizeof(keyRole), "r_%d", i);

            String uid = prefProf.getString(keyUid, "");
            String name = prefProf.getString(keyName, "");
            String role = prefProf.getString(keyRole, "FACULTY");

            if (uid.length() > 0) {
                addOrUpdateFacultyInMemory(uid, name, role);
            }
        }
    }

    void saveFacultyToNvs() {
        LockGuard guard(*this);
        prefProf.clear();
        int count = (int)facultyCache.size();
        prefProf.putInt("count", count);

        for (int i = 0; i < count && i < 20; i++) {
            char keyUid[16], keyName[16], keyRole[16];
            snprintf(keyUid, sizeof(keyUid), "u_%d", i);
            snprintf(keyName, sizeof(keyName), "n_%d", i);
            snprintf(keyRole, sizeof(keyRole), "r_%d", i);

            prefProf.putString(keyUid, facultyCache[i].rfidUid);
            prefProf.putString(keyName, facultyCache[i].name);
            prefProf.putString(keyRole, facultyCache[i].role);
        }
    }

    void updateFacultyList(const std::vector<FacultyCacheRecord> &newList) {
        LockGuard guard(*this);
        // Compare against existing cache to prevent flash write spam every sync cycle
        bool changed = (newList.size() != facultyCache.size());
        if (!changed) {
            for (size_t i = 0; i < newList.size(); i++) {
                if (!newList[i].rfidUid.equalsIgnoreCase(facultyCache[i].rfidUid) ||
                    !newList[i].name.equals(facultyCache[i].name) ||
                    !newList[i].role.equals(facultyCache[i].role)) {
                    changed = true;
                    break;
                }
            }
        }

        if (changed) {
            for (const auto &item : newList) {
                addOrUpdateFacultyInMemory(item.rfidUid, item.name, item.role);
            }
            saveFacultyToNvs();
            Serial.printf(F("[OFFLINE STORAGE] Synced %u faculty instructor records from backend.\n"), facultyCache.size());
        }
    }

    bool isAuthorizedProfessor(const String &cardUid, String &outName, String &outRole) {
        LockGuard guard(*this);
        String clean = normalizeUid(cardUid);
        for (const auto &f : facultyCache) {
            if (f.rfidUid.equalsIgnoreCase(clean)) {
                outName = f.name;
                outRole = f.role;
                return true;
            }
        }
        return false;
    }

    // =========================================================================
    // OFFLINE ATTENDANCE LOGGING (NVS BACKED)
    // =========================================================================

    void loadLogsFromNvs() {
        LockGuard guard(*this);
        pendingLogs.clear();
        int count = prefLogs.getInt("log_count", 0);
        if (count <= 0) {
            offlineSessionAction = "CONTINUE";
            offlineFacultyUid = "";
            return;
        }

        offlineSessionAction = prefLogs.getString("act", "CONTINUE");
        offlineFacultyUid = prefLogs.getString("fac_uid", "");

        for (int i = 0; i < count && i < 100; i++) {
            char kU[16], kT[16], kM[16];
            snprintf(kU, sizeof(kU), "lu_%d", i);
            snprintf(kT, sizeof(kT), "lt_%d", i);
            snprintf(kM, sizeof(kM), "lm_%d", i);

            OfflineAttendanceRecord rec;
            String uid = prefLogs.getString(kU, "");
            String ts = prefLogs.getString(kT, "");
            String mth = prefLogs.getString(kM, "RFID_TAP");

            if (uid.length() > 0) {
                strncpy(rec.rfidUid, uid.c_str(), sizeof(rec.rfidUid) - 1);
                rec.rfidUid[sizeof(rec.rfidUid) - 1] = '\0';
                strncpy(rec.timestamp, ts.c_str(), sizeof(rec.timestamp) - 1);
                rec.timestamp[sizeof(rec.timestamp) - 1] = '\0';
                strncpy(rec.authMethod, mth.c_str(), sizeof(rec.authMethod) - 1);
                rec.authMethod[sizeof(rec.authMethod) - 1] = '\0';
                pendingLogs.push_back(rec);
            }
        }
    }

    void appendLogToNvs(const OfflineAttendanceRecord &rec, int index) {
        LockGuard guard(*this);
        char kU[16], kT[16], kM[16];
        snprintf(kU, sizeof(kU), "lu_%d", index);
        snprintf(kT, sizeof(kT), "lt_%d", index);
        snprintf(kM, sizeof(kM), "lm_%d", index);

        prefLogs.putString(kU, rec.rfidUid);
        prefLogs.putString(kT, rec.timestamp);
        prefLogs.putString(kM, rec.authMethod);
        prefLogs.putInt("log_count", index + 1);
        prefLogs.putString("act", offlineSessionAction);
        if (offlineFacultyUid.length() > 0) {
            prefLogs.putString("fac_uid", offlineFacultyUid);
        }
    }

    void saveLogsToNvs() {
        LockGuard guard(*this);
        prefLogs.clear();
        int count = (int)pendingLogs.size();
        prefLogs.putInt("log_count", count);
        prefLogs.putString("act", offlineSessionAction);
        prefLogs.putString("fac_uid", offlineFacultyUid);

        for (int i = 0; i < count && i < 100; i++) {
            char kU[16], kT[16], kM[16];
            snprintf(kU, sizeof(kU), "lu_%d", i);
            snprintf(kT, sizeof(kT), "lt_%d", i);
            snprintf(kM, sizeof(kM), "lm_%d", i);

            prefLogs.putString(kU, pendingLogs[i].rfidUid);
            prefLogs.putString(kT, pendingLogs[i].timestamp);
            prefLogs.putString(kM, pendingLogs[i].authMethod);
        }
    }

    /**
     * Local Anti-Passback check against offline buffer
     */
    bool isAlreadyLogged(const String &cardUid) {
        LockGuard guard(*this);
        String clean = normalizeUid(cardUid);
        for (const auto &rec : pendingLogs) {
            if (clean.equalsIgnoreCase(rec.rfidUid)) {
                return true;
            }
        }
        return false;
    }

    /**
     * Append offline attendance log (Fast 2ms append write)
     */
    bool addRecord(const String &cardUid, const String &timestamp = "", const String &authMethod = "RFID_TAP") {
        LockGuard guard(*this);
        if (pendingLogs.size() >= 100) {
            Serial.println(F("[OFFLINE STORAGE] Storage queue full (max 100 records)."));
            return false;
        }

        OfflineAttendanceRecord rec;
        String cleanUid = normalizeUid(cardUid);
        strncpy(rec.rfidUid, cleanUid.c_str(), sizeof(rec.rfidUid) - 1);
        rec.rfidUid[sizeof(rec.rfidUid) - 1] = '\0';

        String timeStr = timestamp;
        if (timeStr.length() == 0) {
            time_t nowSecs;
            time(&nowSecs);
            if (nowSecs > 100000) {
                struct tm timeinfo;
                localtime_r(&nowSecs, &timeinfo);
                char buf[32];
                strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.000Z", &timeinfo);
                timeStr = String(buf);
            } else {
                // If SNTP has not synced yet, format a valid ISO 8601 string from elapsed uptime
                unsigned long s = millis() / 1000;
                unsigned int hh = (s / 3600) % 24;
                unsigned int mm = (s / 60) % 60;
                unsigned int ss = s % 60;
                char buf[32];
                snprintf(buf, sizeof(buf), "2026-11-12T%02u:%02u:%02u.000Z", hh, mm, ss);
                timeStr = String(buf);
            }
        }
        strncpy(rec.timestamp, timeStr.c_str(), sizeof(rec.timestamp) - 1);
        rec.timestamp[sizeof(rec.timestamp) - 1] = '\0';

        strncpy(rec.authMethod, authMethod.c_str(), sizeof(rec.authMethod) - 1);
        rec.authMethod[sizeof(rec.authMethod) - 1] = '\0';

        int index = (int)pendingLogs.size();
        pendingLogs.push_back(rec);
        appendLogToNvs(rec, index);

        Serial.printf(F("[OFFLINE STORAGE] Recorded tap for 0x%s at %s. Total pending: %u\n"),
                      rec.rfidUid, rec.timestamp, pendingLogs.size());
        return true;
    }

    int getPendingCount() {
        LockGuard guard(*this);
        return (int)pendingLogs.size();
    }

    std::vector<OfflineAttendanceRecord> getRecords() {
        LockGuard guard(*this);
        return pendingLogs;
    }

    void clearRecords() {
        LockGuard guard(*this);
        pendingLogs.clear();
        offlineSessionAction = "CONTINUE";
        offlineFacultyUid = "";
        saveLogsToNvs();
        Serial.println(F("[OFFLINE STORAGE] Cleared offline attendance queue in memory and NVS."));
    }

    // =========================================================================
    // OFFLINE SESSION TRANSITIONS
    // =========================================================================

    void markOfflineSessionStarted(const String &facultyUid) {
        LockGuard guard(*this);
        offlineSessionAction = "START";
        offlineFacultyUid = normalizeUid(facultyUid);
        saveLogsToNvs();
        Serial.printf(F("[OFFLINE STORAGE] Marked offline session STARTED by Prof 0x%s\n"), offlineFacultyUid.c_str());
    }

    void markOfflineSessionClosed(const String &facultyUid) {
        LockGuard guard(*this);
        offlineSessionAction = "CLOSE";
        offlineFacultyUid = normalizeUid(facultyUid);
        saveLogsToNvs();
        Serial.printf(F("[OFFLINE STORAGE] Marked offline session CLOSED by Prof 0x%s\n"), offlineFacultyUid.c_str());
    }

    void resetSessionState() {
        LockGuard guard(*this);
        offlineSessionAction = "CONTINUE";
        offlineFacultyUid = "";
        prefLogs.putString("act", "CONTINUE");
        prefLogs.putString("fac_uid", "");
        Serial.println(F("[OFFLINE STORAGE] Reset offline session action markers to CONTINUE."));
    }

    String getOfflineSessionAction() {
        LockGuard guard(*this);
        return offlineSessionAction;
    }

    String getOfflineFacultyUid() {
        LockGuard guard(*this);
        return offlineFacultyUid;
    }
};

extern OfflineStorageManager offlineStorage;

#endif // OFFLINE_STORAGE_H
