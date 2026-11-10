#ifndef OFFLINE_STORAGE_H
#define OFFLINE_STORAGE_H

#include <Arduino.h>
#include <Preferences.h>
#include <vector>
#include <time.h>

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
 * Structure representing an offline attendance log recorded during Wi-Fi outage
 */
struct OfflineAttendanceRecord {
    char rfidUid[16];
    char timestamp[28];
    char authMethod[16];
};

class OfflineStorageManager {
private:
    Preferences prefProf;
    Preferences prefLogs;
    std::vector<FacultyCacheRecord> facultyCache;
    std::vector<OfflineAttendanceRecord> pendingLogs;

    String offlineSessionAction = "CONTINUE"; // "START", "CLOSE", or "CONTINUE"
    String offlineFacultyUid = "";

    void seedDefaultFaculty() {
        // Pre-seeded authorized instructors according to institutional directory:
        // 1. Prof. Karlito Macadat (Faculty Instructor - AR101 / IT302)
        addOrUpdateFacultyInMemory("892B2E8E", "Karlito Macadat", "FACULTY");
        // 2. Prof. Justine Jay Agbada (Faculty Instructor)
        addOrUpdateFacultyInMemory("71394417", "Justine Jay Agbada", "FACULTY");
        // 3. System Administrator (Master Key Clearance)
        addOrUpdateFacultyInMemory("99AA88BB", "System Administrator", "ADMIN");
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

public:
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
        prefProf.begin("orch_prof", false);
        prefLogs.begin("orch_logs", false);

        seedDefaultFaculty();
        loadFacultyFromNvs();
        loadLogsFromNvs();

        Serial.printf(F("[OFFLINE STORAGE] Initialized. %u faculty cached, %u pending offline logs in NVS.\n"),
                      facultyCache.size(), pendingLogs.size());
    }

    // =========================================================================
    // LOCAL PROFESSOR UID CACHE
    // =========================================================================

    void loadFacultyFromNvs() {
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
        for (const auto &item : newList) {
            addOrUpdateFacultyInMemory(item.rfidUid, item.name, item.role);
        }
        saveFacultyToNvs();
        Serial.printf(F("[OFFLINE STORAGE] Synced %u faculty instructor records from backend.\n"), facultyCache.size());
    }

    bool isAuthorizedProfessor(const String &cardUid, String &outName, String &outRole) {
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
        pendingLogs.clear();
        int count = prefLogs.getInt("log_count", 0);
        if (count <= 0) return;

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
                strncpy(rec.rfidUid, uid.c_str(), sizeof(rec.rfidUid));
                strncpy(rec.timestamp, ts.c_str(), sizeof(rec.timestamp));
                strncpy(rec.authMethod, mth.c_str(), sizeof(rec.authMethod));
                pendingLogs.push_back(rec);
            }
        }
    }

    void saveLogsToNvs() {
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
        String clean = normalizeUid(cardUid);
        for (const auto &rec : pendingLogs) {
            if (clean.equalsIgnoreCase(rec.rfidUid)) {
                return true;
            }
        }
        return false;
    }

    /**
     * Append offline attendance log
     */
    bool addRecord(const String &cardUid, const String &timestamp = "", const String &authMethod = "RFID_TAP") {
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
                timeStr = String("2026-11-10T") + String(millis()) + String("Z");
            }
        }
        strncpy(rec.timestamp, timeStr.c_str(), sizeof(rec.timestamp) - 1);
        rec.timestamp[sizeof(rec.timestamp) - 1] = '\0';

        strncpy(rec.authMethod, authMethod.c_str(), sizeof(rec.authMethod) - 1);
        rec.authMethod[sizeof(rec.authMethod) - 1] = '\0';

        pendingLogs.push_back(rec);
        saveLogsToNvs();

        Serial.printf(F("[OFFLINE STORAGE] Recorded tap for 0x%s at %s. Total pending: %u\n"),
                      rec.rfidUid, rec.timestamp, pendingLogs.size());
        return true;
    }

    int getPendingCount() const {
        return (int)pendingLogs.size();
    }

    const std::vector<OfflineAttendanceRecord>& getRecords() const {
        return pendingLogs;
    }

    void clearRecords() {
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
        offlineSessionAction = "START";
        offlineFacultyUid = normalizeUid(facultyUid);
        saveLogsToNvs();
        Serial.printf(F("[OFFLINE STORAGE] Marked offline session STARTED by Prof 0x%s\n"), offlineFacultyUid.c_str());
    }

    void markOfflineSessionClosed(const String &facultyUid) {
        offlineSessionAction = "CLOSE";
        offlineFacultyUid = normalizeUid(facultyUid);
        saveLogsToNvs();
        Serial.printf(F("[OFFLINE STORAGE] Marked offline session CLOSED by Prof 0x%s\n"), offlineFacultyUid.c_str());
    }

    String getOfflineSessionAction() const {
        return offlineSessionAction;
    }

    String getOfflineFacultyUid() const {
        return offlineFacultyUid;
    }
};

extern OfflineStorageManager offlineStorage;

#endif // OFFLINE_STORAGE_H
